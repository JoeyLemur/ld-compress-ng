#include "accelerated_native_backend.h"

#include "compressor.h"
#include "hash.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;

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

void rewind_to(std::istream& input, std::streampos position, const char* backend_label)
{
    input.clear();
    input.seekg(position);
    if (!input) {
        throw std::runtime_error(
            "failed to rewind LDS input for " + std::string(backend_label) +
            " FLAC encoding");
    }
}

FlacStreamInfo make_streaminfo(
    const ConversionStats& stats,
    unsigned frame_sample_count,
    unsigned sample_rate,
    const std::array<std::uint8_t, 16>& md5)
{
    auto streaminfo_block_size = frame_sample_count;
    if (stats.samples == 0) {
        streaminfo_block_size = kMinimumStreamInfoBlockSize;
    } else if (stats.samples < frame_sample_count) {
        streaminfo_block_size = stats.samples < kMinimumStreamInfoBlockSize
            ? kMinimumStreamInfoBlockSize
            : static_cast<unsigned>(stats.samples);
    }

    return FlacStreamInfo {
        .min_block_size = streaminfo_block_size,
        .max_block_size = streaminfo_block_size,
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
    case FlacSubframeKind::LpcRice:
        ++stats->lpc_rice_frames;
        if (decision.lpc_order < stats->lpc_order_counts.size()) {
            ++stats->lpc_order_counts[decision.lpc_order];
        }
        if (decision.rice_partition_order < stats->partition_order_counts.size()) {
            ++stats->partition_order_counts[decision.rice_partition_order];
        }
        return;
    }

    throw std::runtime_error("unknown native FLAC subframe kind");
}

FlacFrameInfo make_frame_info(
    std::uint64_t frame_number,
    const AcceleratedNativeCompressionOptions& options)
{
    return FlacFrameInfo {
        .frame_number = frame_number,
        .sample_rate = options.sample_rate,
        .bits_per_sample = 16,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
    };
}

void write_accelerated_selected_batch(
    std::ostream& output,
    const std::vector<std::int32_t>& batch_samples,
    std::uint64_t first_frame_number,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer)
{
    if (batch_samples.empty()) {
        return;
    }
    if ((batch_samples.size() % options.frame_samples) != 0) {
        throw std::runtime_error(
            "internal " + std::string(options.backend_label) +
            " batch was not frame-aligned");
    }

    const auto frame_count = batch_samples.size() / options.frame_samples;
    auto base_frame_info = make_frame_info(first_frame_number, options);
    const auto analysis = analyzer(batch_samples, base_frame_info, options.frame_samples);
    if (analysis.selected_subframes.size() != frame_count ||
        analysis.decisions.size() != frame_count) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " selected frame count did not match input batch");
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            batch_samples.begin() + static_cast<std::ptrdiff_t>(offset),
            batch_samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
        auto frame_info = make_frame_info(first_frame_number + frame, options);
        const auto decision = write_mono_selected_frame(
            output, frame_samples, frame_info, analysis.selected_subframes[frame]);
        record_native_stats(options.native_stats, decision);
    }
}

void write_native_tail_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    std::uint64_t frame_number,
    const AcceleratedNativeCompressionOptions& options)
{
    if (samples.empty()) {
        return;
    }

    const auto decision = write_mono_best_frame(
        output, samples, make_frame_info(frame_number, options));
    record_native_stats(options.native_stats, decision);
}

}  // namespace

ConversionStats compress_lds_to_accelerated_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer)
{
    const auto start = lds_input.tellg();
    if (start == std::streampos(-1)) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " FLAC backend requires a seekable LDS input");
    }

    Md5 pcm_md5;
    auto stats = process_lds_samples(lds_input, [&pcm_md5](std::int16_t sample) {
        update_md5_s16le(pcm_md5, sample);
    });
    const auto streaminfo = make_streaminfo(
        stats, options.frame_samples, options.sample_rate, pcm_md5.digest());

    rewind_to(lds_input, start, options.backend_label);

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(output, streaminfo);

    std::vector<std::int32_t> frame_samples;
    frame_samples.reserve(options.frame_samples);
    std::vector<std::int32_t> batch_samples;
    batch_samples.reserve(static_cast<std::size_t>(options.frame_samples) * options.batch_frames);
    std::uint64_t frame_number = 0;

    const auto flush_batch = [&] {
        if (batch_samples.empty()) {
            return;
        }
        const auto batch_first_frame =
            frame_number - (batch_samples.size() / options.frame_samples);
        write_accelerated_selected_batch(
            output, batch_samples, batch_first_frame, options, analyzer);
        batch_samples.clear();
    };

    const auto encoded_stats = process_lds_samples(lds_input, [&](std::int16_t sample) {
        frame_samples.push_back(sample);
        if (frame_samples.size() == options.frame_samples) {
            batch_samples.insert(batch_samples.end(), frame_samples.begin(), frame_samples.end());
            ++frame_number;
            frame_samples.clear();
            if ((batch_samples.size() / options.frame_samples) == options.batch_frames) {
                flush_batch();
            }
        }
    });

    flush_batch();
    if (!frame_samples.empty()) {
        write_native_tail_frame(output, frame_samples, frame_number, options);
        ++frame_number;
    }

    if (encoded_stats.input_bytes != stats.input_bytes ||
        encoded_stats.samples != stats.samples) {
        throw std::runtime_error(
            "LDS input changed while " + std::string(options.backend_label) +
            " FLAC backend was encoding");
    }

    output.close();
    if (!output) {
        throw std::runtime_error(
            "failed to finish " + std::string(options.backend_label) + " FLAC output: " +
            output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

}  // namespace ldcompress
