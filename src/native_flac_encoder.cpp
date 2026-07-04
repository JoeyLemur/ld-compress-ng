#include "native_flac_encoder.h"

#include "flac_native_writer.h"
#include "hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr std::size_t kFrameSamples = 4608;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;

enum class NativeFrameCoding {
    Verbatim,
    FixedRice,
};

void update_md5_s16le(Md5& md5, std::int16_t sample)
{
    const auto value = static_cast<std::uint16_t>(sample);
    const std::array<std::uint8_t, 2> bytes {
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
    };
    md5.update(bytes.data(), bytes.size());
}

template <typename OnSample>
ConversionStats process_lds_samples(std::istream& input, OnSample&& on_sample)
{
    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);

    while (input) {
        input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        for (std::size_t i = 0; i < groups; ++i) {
            PackedLdsGroup packed;
            std::memcpy(packed.data(), input_buffer.data() + (i * 5), packed.size());
            const SampleGroup samples = unpack_group(packed);
            for (const auto sample : samples) {
                on_sample(sample);
            }
        }

        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    return stats;
}

void rewind_to(std::istream& input, std::streampos position)
{
    input.clear();
    input.seekg(position);
    if (!input) {
        throw std::runtime_error("failed to rewind LDS input for native FLAC encoding");
    }
}

FlacStreamInfo make_streaminfo(
    const ConversionStats& stats,
    unsigned sample_rate,
    const std::array<std::uint8_t, 16>& md5)
{
    std::uint64_t min_block = kMinimumStreamInfoBlockSize;
    std::uint64_t max_block = kMinimumStreamInfoBlockSize;
    if (stats.samples >= kMinimumStreamInfoBlockSize && stats.samples <= kFrameSamples) {
        min_block = stats.samples;
        max_block = stats.samples;
    } else if (stats.samples > kFrameSamples) {
        min_block = kFrameSamples;
        max_block = kFrameSamples;
    }

    return FlacStreamInfo {
        .min_block_size = static_cast<unsigned>(min_block),
        .max_block_size = static_cast<unsigned>(max_block),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = sample_rate,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = stats.samples,
        .md5 = md5,
    };
}

void record_native_stats(
    NativeCompressionStats* stats,
    const FlacSubframeDecision& decision)
{
    if (stats == nullptr) {
        return;
    }

    ++stats->frames;
    stats->estimated_subframe_bits += decision.estimated_bits;
    if (decision.wasted_bits < stats->wasted_bits_counts.size()) {
        ++stats->wasted_bits_counts[decision.wasted_bits];
    }

    switch (decision.kind) {
    case FlacSubframeKind::Constant:
        ++stats->constant_frames;
        return;
    case FlacSubframeKind::Verbatim:
        ++stats->verbatim_frames;
        return;
    case FlacSubframeKind::FixedRice:
        ++stats->fixed_rice_frames;
        if (decision.fixed_order < stats->fixed_order_counts.size()) {
            ++stats->fixed_order_counts[decision.fixed_order];
        }
        if (decision.rice_partition_order < stats->partition_order_counts.size()) {
            ++stats->partition_order_counts[decision.rice_partition_order];
        }
        return;
    }

    throw std::runtime_error("unknown native FLAC subframe kind");
}

FlacSubframeDecision write_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    std::uint64_t frame_number,
    unsigned sample_rate,
    NativeFrameCoding coding)
{
    const FlacFrameInfo frame_info {
        .frame_number = frame_number,
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
    };
    switch (coding) {
    case NativeFrameCoding::Verbatim:
        return write_mono_verbatim_frame(output, samples, frame_info);
    case NativeFrameCoding::FixedRice:
        return write_mono_best_frame(output, samples, frame_info);
    }
    throw std::runtime_error("unknown native FLAC frame coding");
}

struct EncodedFrame {
    std::string bytes;
    FlacSubframeDecision decision;
};

EncodedFrame encode_frame(
    std::vector<std::int32_t> samples,
    std::uint64_t frame_number,
    unsigned sample_rate,
    NativeFrameCoding coding)
{
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto decision = write_frame(output, samples, frame_number, sample_rate, coding);
    if (!output) {
        throw std::runtime_error("failed to encode native FLAC frame");
    }
    return EncodedFrame {
        .bytes = output.str(),
        .decision = decision,
    };
}

void write_frame_bytes(std::ostream& output, const std::string& bytes)
{
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC frame output");
    }
}

struct PendingFrame {
    std::uint64_t frame_number = 0;
    std::future<EncodedFrame> frame;
};

void flush_next_pending_frame(
    std::ostream& output,
    std::deque<PendingFrame>& pending_frames,
    std::uint64_t& next_frame_to_write,
    NativeCompressionStats* stats)
{
    if (pending_frames.empty()) {
        return;
    }
    auto pending = std::move(pending_frames.front());
    pending_frames.pop_front();
    if (pending.frame_number != next_frame_to_write) {
        throw std::runtime_error("internal native FLAC frame ordering error");
    }
    const auto encoded_frame = pending.frame.get();
    write_frame_bytes(output, encoded_frame.bytes);
    record_native_stats(stats, encoded_frame.decision);
    ++next_frame_to_write;
}

}  // namespace

ConversionStats compress_lds_to_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    NativeFrameCoding coding,
    unsigned thread_count,
    NativeCompressionStats* native_stats)
{
    if (thread_count == 0) {
        throw std::runtime_error("native FLAC thread count must be at least 1");
    }

    const auto start = lds_input.tellg();
    if (start == std::streampos(-1)) {
        throw std::runtime_error("native FLAC backend requires a seekable LDS input");
    }

    Md5 pcm_md5;
    auto stats = process_lds_samples(lds_input, [&pcm_md5](std::int16_t sample) {
        update_md5_s16le(pcm_md5, sample);
    });
    const auto streaminfo = make_streaminfo(stats, sample_rate, pcm_md5.digest());

    rewind_to(lds_input, start);

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(output, streaminfo);

    std::vector<std::int32_t> frame_samples;
    frame_samples.reserve(kFrameSamples);
    std::uint64_t frame_number = 0;
    std::uint64_t next_frame_to_write = 0;
    std::deque<PendingFrame> pending_frames;
    const auto submit_frame = [&](std::vector<std::int32_t> samples, std::uint64_t current_frame) {
        if (thread_count == 1) {
            const auto decision = write_frame(output, samples, current_frame, sample_rate, coding);
            record_native_stats(native_stats, decision);
            ++next_frame_to_write;
            return;
        }

        pending_frames.push_back(PendingFrame {
            .frame_number = current_frame,
            .frame = std::async(
                std::launch::async, encode_frame, std::move(samples),
                current_frame, sample_rate, coding),
        });
        while (pending_frames.size() > thread_count) {
            flush_next_pending_frame(output, pending_frames, next_frame_to_write, native_stats);
        }
    };

    const auto encoded_stats = process_lds_samples(lds_input, [&](std::int16_t sample) {
        frame_samples.push_back(sample);
        if (frame_samples.size() == kFrameSamples) {
            submit_frame(std::move(frame_samples), frame_number);
            ++frame_number;
            frame_samples.clear();
            frame_samples.reserve(kFrameSamples);
        }
    });
    if (!frame_samples.empty()) {
        submit_frame(std::move(frame_samples), frame_number);
        ++frame_number;
    }
    while (!pending_frames.empty()) {
        flush_next_pending_frame(output, pending_frames, next_frame_to_write, native_stats);
    }
    if (next_frame_to_write != frame_number) {
        throw std::runtime_error("internal native FLAC frame count mismatch");
    }

    if (encoded_stats.input_bytes != stats.input_bytes ||
        encoded_stats.samples != stats.samples) {
        throw std::runtime_error("LDS input changed while native FLAC backend was encoding");
    }

    output.close();
    if (!output) {
        throw std::runtime_error("failed to finish native FLAC output: " + output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

ConversionStats compress_lds_to_native_verbatim_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    NativeCompressionStats* stats)
{
    return compress_lds_to_native_flac(
        lds_input, output_path, sample_rate, NativeFrameCoding::Verbatim, thread_count, stats);
}

ConversionStats compress_lds_to_native_fixed_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    NativeCompressionStats* stats)
{
    return compress_lds_to_native_flac(
        lds_input, output_path, sample_rate, NativeFrameCoding::FixedRice, thread_count, stats);
}

}  // namespace ldcompress
