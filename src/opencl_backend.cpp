#include "opencl_backend.h"

#include "compressor.h"
#include "flac_native_writer.h"
#include "hash.h"
#include "opencl_analysis.h"
#include "opencl_devices.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr std::size_t kOpenClBatchFrames = 32;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxOpenClFrameSamples = 4608;
constexpr unsigned kMaxOpenClLpcOrder = 12;
constexpr unsigned kMinOpenClLpcPrecision = 1;
constexpr unsigned kMaxOpenClLpcPrecision = 15;
constexpr unsigned kMaxOpenClRicePartitionOrder = 8;

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
        throw std::runtime_error("failed to rewind LDS input for OpenCL FLAC encoding");
    }
}

FlacStreamInfo make_streaminfo(
    const ConversionStats& stats,
    unsigned frame_sample_count,
    unsigned sample_rate,
    const std::array<std::uint8_t, 16>& md5)
{
    std::uint64_t min_block = kMinimumStreamInfoBlockSize;
    std::uint64_t max_block = kMinimumStreamInfoBlockSize;
    if (stats.samples > 0 && stats.samples <= frame_sample_count) {
        min_block = stats.samples;
        max_block = stats.samples;
    } else if (stats.samples > frame_sample_count) {
        const auto remainder = stats.samples % frame_sample_count;
        min_block = remainder == 0 ? frame_sample_count : remainder;
        max_block = frame_sample_count;
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

struct OpenClSelectedFrameAnalysis {
    std::vector<FlacSubframeDecision> decisions;
    std::vector<FlacSelectedSubframe> selected_subframes;
};

OpenClSelectedFrameAnalysis analyze_opencl_selected_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> device_index)
{
    if (frame_info.max_lpc_order > 0) {
        auto result = opencl_detail::analyze_opencl_mono_generated_frames(
            samples, frame_info, frame_samples, device_index);
        return OpenClSelectedFrameAnalysis {
            .decisions = std::move(result.decisions),
            .selected_subframes = std::move(result.selected_subframes),
        };
    }

    opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = frame_samples;
    task_options.bits_per_sample = frame_info.bits_per_sample;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto frame_count = samples.size() / frame_samples;
    const auto plan = opencl_detail::build_mono_analysis_task_plan(frame_count, task_options);
    auto result = opencl_detail::run_opencl_mono_fixed_constant_analysis(
        samples, plan, device_index, frame_info.max_rice_partition_order);

    OpenClSelectedFrameAnalysis selected;
    selected.decisions.reserve(result.best_tasks.size());
    selected.selected_subframes.reserve(result.best_tasks.size());
    for (const auto& task : result.best_tasks) {
        selected.decisions.push_back(opencl_detail::flaccl_task_to_subframe_decision(task));
        selected.selected_subframes.push_back(opencl_detail::flaccl_task_to_selected_subframe(task));
    }
    return selected;
}

FlacFrameInfo make_frame_info(
    std::uint64_t frame_number,
    const OpenClCompressionOptions& options)
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

void write_opencl_selected_batch(
    std::ostream& output,
    const std::vector<std::int32_t>& batch_samples,
    std::uint64_t first_frame_number,
    const OpenClCompressionOptions& options,
    std::optional<std::size_t> device_index)
{
    if (batch_samples.empty()) {
        return;
    }
    if ((batch_samples.size() % options.frame_samples) != 0) {
        throw std::runtime_error("internal OpenCL batch was not frame-aligned");
    }

    const auto frame_count = batch_samples.size() / options.frame_samples;
    auto base_frame_info = make_frame_info(first_frame_number, options);
    const auto analysis = analyze_opencl_selected_frames(
        batch_samples, base_frame_info, options.frame_samples, device_index);
    if (analysis.selected_subframes.size() != frame_count ||
        analysis.decisions.size() != frame_count) {
        throw std::runtime_error("OpenCL selected frame count did not match input batch");
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
    const OpenClCompressionOptions& options)
{
    if (samples.empty()) {
        return;
    }

    const auto decision = write_mono_best_frame(
        output, samples, make_frame_info(frame_number, options));
    record_native_stats(options.native_stats, decision);
}

void validate_opencl_options(const OpenClCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("opencl backend writes native FLAC only");
    }
    if (options.thread_count != 1) {
        throw std::runtime_error("OpenCL FLAC backend currently requires --threads 1");
    }
    if (options.frame_samples < kMinimumStreamInfoBlockSize ||
        options.frame_samples > kMaxOpenClFrameSamples) {
        throw std::runtime_error("OpenCL FLAC frame sample count must be 16..4608");
    }
    if (options.max_lpc_order > kMaxOpenClLpcOrder) {
        throw std::runtime_error("OpenCL FLAC max LPC order must be 0..12");
    }
    if (options.lpc_precision < kMinOpenClLpcPrecision ||
        options.lpc_precision > kMaxOpenClLpcPrecision) {
        throw std::runtime_error("OpenCL FLAC LPC coefficient precision must be 1..15");
    }
    if (options.max_rice_partition_order > kMaxOpenClRicePartitionOrder) {
        throw std::runtime_error("OpenCL FLAC max Rice partition order must be 0..8");
    }
}

}  // namespace

ConversionStats compress_lds_to_opencl_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const OpenClCompressionOptions& options)
{
    validate_opencl_options(options);
    const auto selected_device = select_opencl_device(options.device_index);
    const auto device_index = std::optional<std::size_t>(selected_device.flat_index);

    const auto start = lds_input.tellg();
    if (start == std::streampos(-1)) {
        throw std::runtime_error("OpenCL FLAC backend requires a seekable LDS input");
    }

    Md5 pcm_md5;
    auto stats = process_lds_samples(lds_input, [&pcm_md5](std::int16_t sample) {
        update_md5_s16le(pcm_md5, sample);
    });
    const auto streaminfo = make_streaminfo(
        stats, options.frame_samples, options.sample_rate, pcm_md5.digest());

    rewind_to(lds_input, start);

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(output, streaminfo);

    std::vector<std::int32_t> frame_samples;
    frame_samples.reserve(options.frame_samples);
    std::vector<std::int32_t> batch_samples;
    batch_samples.reserve(static_cast<std::size_t>(options.frame_samples) * kOpenClBatchFrames);
    std::uint64_t frame_number = 0;

    const auto flush_batch = [&] {
        if (batch_samples.empty()) {
            return;
        }
        const auto batch_first_frame =
            frame_number - (batch_samples.size() / options.frame_samples);
        write_opencl_selected_batch(
            output, batch_samples, batch_first_frame, options, device_index);
        batch_samples.clear();
    };

    const auto encoded_stats = process_lds_samples(lds_input, [&](std::int16_t sample) {
        frame_samples.push_back(sample);
        if (frame_samples.size() == options.frame_samples) {
            batch_samples.insert(batch_samples.end(), frame_samples.begin(), frame_samples.end());
            ++frame_number;
            frame_samples.clear();
            if ((batch_samples.size() / options.frame_samples) == kOpenClBatchFrames) {
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
        throw std::runtime_error("LDS input changed while OpenCL FLAC backend was encoding");
    }

    output.close();
    if (!output) {
        throw std::runtime_error("failed to finish OpenCL FLAC output: " + output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

}  // namespace ldcompress
