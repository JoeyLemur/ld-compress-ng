#include "accelerated_native_backend.h"

#include "compressor.h"
#include "hash.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point finish)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += elapsed_ns(start, Clock::now());
}

void update_md5_s16le(Md5& md5, const SampleGroup& samples)
{
    std::array<std::uint8_t, 8> bytes {};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        bytes[(i * 2U) + 0U] = static_cast<std::uint8_t>(value & 0xffU);
        bytes[(i * 2U) + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    }
    md5.update(bytes.data(), bytes.size());
}

template <typename OnGroup>
ConversionStats process_lds_sample_groups(std::istream& input, OnGroup&& on_group)
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
            on_group(samples);
        }

        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    return stats;
}

void rewrite_native_flac_streaminfo(
    std::ostream& output,
    const FlacStreamInfo& streaminfo,
    const std::string& output_path,
    const char* backend_label)
{
    output.seekp(0, std::ios::beg);
    if (!output) {
        throw std::runtime_error(
            "failed to seek " + std::string(backend_label) +
            " FLAC output for STREAMINFO rewrite: " + output_path);
    }
    write_native_flac_streaminfo(output, streaminfo);
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

void record_selected_write_timings(
    NativeCompressionStats* stats,
    const FlacSelectedFrameWriteTimings& timings)
{
    if (stats == nullptr) {
        return;
    }

    stats->accelerated_selected_validation_ns += timings.validation_wasted_ns;
    stats->accelerated_selected_shift_ns += timings.shift_ns;
    stats->accelerated_selected_residual_ns += timings.residual_ns;
    stats->accelerated_selected_rice_parameter_ns += timings.rice_parameter_ns;
    stats->accelerated_selected_bitstream_ns += timings.bitstream_ns;
    stats->accelerated_selected_frame_output_ns += timings.frame_output_ns;
}

struct SelectedEncodedFrame {
    std::string bytes;
    FlacSubframeDecision decision;
    FlacSelectedFrameWriteTimings timings;
};

struct SelectedFrameJob {
    std::uint64_t frame_number = 0;
    FlacFrameInfo frame_info;
    std::vector<std::int32_t> samples;
    FlacSelectedSubframe selected;
    FlacSubframeDecision decision;
    bool collect_timings = false;
};

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

SelectedEncodedFrame encode_selected_frame(SelectedFrameJob job)
{
    std::ostringstream output(std::ios::out | std::ios::binary);
    FlacSelectedFrameWriteTimings timings;
    const auto decision = write_mono_selected_frame_with_decision(
        output,
        std::span<const std::int32_t>(job.samples.data(), job.samples.size()),
        job.frame_info,
        job.selected,
        job.decision,
        job.collect_timings ? &timings : nullptr);
    if (!output) {
        throw std::runtime_error("failed to encode selected native FLAC frame");
    }
    return SelectedEncodedFrame {
        .bytes = output.str(),
        .decision = decision,
        .timings = timings,
    };
}

void write_frame_bytes(std::ostream& output, const std::string& bytes)
{
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write selected native FLAC frame output");
    }
}

class SelectedFrameWriterPool final {
public:
    explicit SelectedFrameWriterPool(unsigned thread_count)
    {
        workers_.reserve(thread_count);
        for (unsigned i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] {
                worker_loop();
            });
        }
    }

    SelectedFrameWriterPool(const SelectedFrameWriterPool&) = delete;
    SelectedFrameWriterPool& operator=(const SelectedFrameWriterPool&) = delete;

    ~SelectedFrameWriterPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        jobs_available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void submit(SelectedFrameJob job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rethrow_exception_if_present();
            jobs_.push_back(std::move(job));
        }
        jobs_available_.notify_one();
    }

    SelectedEncodedFrame take(std::uint64_t frame_number)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        results_available_.wait(lock, [&] {
            return exception_ != nullptr || results_.find(frame_number) != results_.end();
        });
        rethrow_exception_if_present();

        auto result = std::move(results_.at(frame_number));
        results_.erase(frame_number);
        return result;
    }

private:
    void rethrow_exception_if_present() const
    {
        if (exception_ != nullptr) {
            std::rethrow_exception(exception_);
        }
    }

    void worker_loop()
    {
        while (true) {
            SelectedFrameJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                jobs_available_.wait(lock, [&] {
                    return stopping_ || !jobs_.empty();
                });
                if (stopping_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            try {
                const auto frame_number = job.frame_number;
                auto frame = encode_selected_frame(std::move(job));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    results_.emplace(frame_number, std::move(frame));
                }
                results_available_.notify_all();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (exception_ == nullptr) {
                        exception_ = std::current_exception();
                    }
                    stopping_ = true;
                }
                jobs_available_.notify_all();
                results_available_.notify_all();
                return;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable jobs_available_;
    std::condition_variable results_available_;
    std::deque<SelectedFrameJob> jobs_;
    std::map<std::uint64_t, SelectedEncodedFrame> results_;
    std::vector<std::thread> workers_;
    std::exception_ptr exception_;
    bool stopping_ = false;
};

void write_accelerated_selected_batch(
    std::ostream& output,
    const std::vector<std::int32_t>& batch_samples,
    std::uint64_t first_frame_number,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer,
    SelectedFrameWriterPool* frame_pool)
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
    if (options.native_stats != nullptr) {
        ++options.native_stats->accelerated_batches;
    }
    const auto analyzer_started = Clock::now();
    const auto analysis = analyzer(batch_samples, base_frame_info, options.frame_samples);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_analyzer_ns, analyzer_started);
    }
    if (analysis.selected_subframes.size() != frame_count ||
        analysis.decisions.size() != frame_count) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " selected frame count did not match input batch");
    }

    const auto write_started = Clock::now();
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::span<const std::int32_t> frame_samples(
            batch_samples.data() + offset,
            static_cast<std::size_t>(options.frame_samples));
        auto frame_info = make_frame_info(first_frame_number + frame, options);
        if (frame_pool != nullptr) {
            frame_pool->submit(SelectedFrameJob {
                .frame_number = first_frame_number + frame,
                .frame_info = frame_info,
                .samples = std::vector<std::int32_t>(
                    frame_samples.begin(), frame_samples.end()),
                .selected = analysis.selected_subframes[frame],
                .decision = analysis.decisions[frame],
                .collect_timings = options.native_stats != nullptr,
            });
            continue;
        }
        FlacSelectedFrameWriteTimings writer_timings;
        const auto decision = write_mono_selected_frame_with_decision(
            output,
            frame_samples,
            frame_info,
            analysis.selected_subframes[frame],
            analysis.decisions[frame],
            options.native_stats != nullptr ? &writer_timings : nullptr);
        record_native_stats(options.native_stats, decision);
        record_selected_write_timings(options.native_stats, writer_timings);
    }
    if (frame_pool != nullptr) {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            auto encoded = frame_pool->take(first_frame_number + frame);
            write_frame_bytes(output, encoded.bytes);
            record_native_stats(options.native_stats, encoded.decision);
            record_selected_write_timings(options.native_stats, encoded.timings);
        }
    }
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_selected_write_ns, write_started);
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

    const auto write_started = Clock::now();
    const auto decision = write_mono_best_frame(
        output, samples, make_frame_info(frame_number, options));
    record_native_stats(options.native_stats, decision);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_tail_write_ns, write_started);
    }
}

}  // namespace

ConversionStats compress_lds_to_accelerated_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer)
{
    if (options.thread_count == 0) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " FLAC thread count must be at least 1");
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(
        output,
        make_streaminfo(
            ConversionStats {}, options.frame_samples, options.sample_rate,
            std::array<std::uint8_t, 16> {}));

    Md5 pcm_md5;

    std::vector<std::int32_t> batch_samples;
    const auto frame_sample_count = static_cast<std::size_t>(options.frame_samples);
    const auto batch_sample_count = frame_sample_count * options.batch_frames;
    batch_samples.reserve(batch_sample_count);
    std::uint64_t frame_number = 0;
    std::size_t samples_in_current_frame = 0;
    std::unique_ptr<SelectedFrameWriterPool> frame_pool;
    if (options.thread_count != 1) {
        frame_pool = std::make_unique<SelectedFrameWriterPool>(options.thread_count);
    }

    const auto flush_batch = [&] {
        if (batch_samples.empty()) {
            return;
        }
        const auto batch_first_frame =
            frame_number - (batch_samples.size() / frame_sample_count);
        write_accelerated_selected_batch(
            output, batch_samples, batch_first_frame, options, analyzer, frame_pool.get());
        batch_samples.clear();
    };

    const auto ingest_started = Clock::now();
    const auto analyzer_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_analyzer_ns
        : 0;
    const auto selected_write_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_selected_write_ns
        : 0;
    const auto tail_write_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_tail_write_ns
        : 0;

    const auto append_sample = [&](std::int16_t sample) {
        batch_samples.push_back(sample);
        ++samples_in_current_frame;
        if (samples_in_current_frame == frame_sample_count) {
            samples_in_current_frame = 0;
            ++frame_number;
            if (batch_samples.size() == batch_sample_count) {
                flush_batch();
            }
        }
    };

    auto stats = process_lds_sample_groups(lds_input, [&](const SampleGroup& samples) {
        update_md5_s16le(pcm_md5, samples);
        for (const auto sample : samples) {
            append_sample(sample);
        }
    });

    std::vector<std::int32_t> tail_samples;
    const auto complete_sample_count =
        (batch_samples.size() / frame_sample_count) * frame_sample_count;
    if (complete_sample_count < batch_samples.size()) {
        tail_samples.assign(
            batch_samples.begin() + static_cast<std::ptrdiff_t>(complete_sample_count),
            batch_samples.end());
        batch_samples.resize(complete_sample_count);
    }
    flush_batch();
    if (!tail_samples.empty()) {
        write_native_tail_frame(output, tail_samples, frame_number, options);
        ++frame_number;
    }

    if (options.native_stats != nullptr) {
        const auto ingest_ns = elapsed_ns(ingest_started, Clock::now());
        const auto nested_ns =
            (options.native_stats->accelerated_analyzer_ns - analyzer_before) +
            (options.native_stats->accelerated_selected_write_ns - selected_write_before) +
            (options.native_stats->accelerated_tail_write_ns - tail_write_before);
        if (ingest_ns > nested_ns) {
            options.native_stats->accelerated_scan_ns += ingest_ns - nested_ns;
        }
    }

    const auto streaminfo = make_streaminfo(
        stats, options.frame_samples, options.sample_rate, pcm_md5.digest());
    rewrite_native_flac_streaminfo(output, streaminfo, output_path, options.backend_label);

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
