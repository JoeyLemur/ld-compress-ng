#include "opencl_analysis.h"

#include "flac_native_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef LDCOMPRESS_HAVE_OPENCL
#define LDCOMPRESS_HAVE_OPENCL 0
#endif

#if LDCOMPRESS_HAVE_OPENCL
#if defined(__APPLE__)
#include <OpenCL/opencl.h>
#else
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>
#endif
#endif

#if LDCOMPRESS_HAVE_OPENCL
#define LDCOMPRESS_OPENCL_ONLY_USED
#else
#define LDCOMPRESS_OPENCL_ONLY_USED [[maybe_unused]]
#endif

namespace ldcompress::opencl_detail {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kGeneratedLpcBaseWindowCount = 2;
constexpr std::size_t kGeneratedWelchCandidateTargetCount = 2;
constexpr double kGeneratedTukeyTaperFraction = 0.5;

struct GeneratedLpcPrefixShape {
    std::size_t lpc_tasks_per_window = 0;
    std::size_t window_count = 0;
    std::size_t total_lpc_tasks = 0;
};

std::int32_t checked_i32(std::uint64_t value, const char* name)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error(std::string(name) + " exceeds FLACCL int32 task field");
    }
    return static_cast<std::int32_t>(value);
}

void validate_options(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.frame_samples == 0) {
        throw std::runtime_error("OpenCL mono analysis frame_samples must be positive");
    }
    if (options.bits_per_sample == 0 || options.bits_per_sample > 32) {
        throw std::runtime_error("OpenCL mono analysis bits_per_sample must be in 1..32");
    }
    if (options.max_lpc_order > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono analysis max_lpc_order exceeds FLACCL max order");
    }
    if (options.max_lpc_order >= options.frame_samples && options.max_lpc_order != 0) {
        throw std::runtime_error("OpenCL mono analysis max_lpc_order must be smaller than frame_samples");
    }
    if (options.min_fixed_order > 4 || options.max_fixed_order > 4) {
        throw std::runtime_error("OpenCL mono analysis fixed predictor order must be in 0..4");
    }
    if (options.min_fixed_order > options.max_fixed_order) {
        throw std::runtime_error("OpenCL mono analysis min_fixed_order exceeds max_fixed_order");
    }
    if (static_cast<std::uint64_t>(options.bits_per_sample) * options.frame_samples >
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("OpenCL mono analysis raw task size exceeds int32");
    }

    const auto count = mono_analysis_tasks_per_frame(options);
    if (count == 0) {
        throw std::runtime_error("OpenCL mono analysis must contain at least one residual task");
    }
    if (count > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono analysis has more than 32 residual tasks per frame");
    }
}

std::size_t fixed_task_count(const OpenClMonoAnalysisTaskOptions& options)
{
    const auto largest_usable = static_cast<unsigned>(options.frame_samples - 1U);
    const auto max_order = options.max_fixed_order < largest_usable ? options.max_fixed_order : largest_usable;
    if (options.min_fixed_order > max_order) {
        return 0;
    }
    return static_cast<std::size_t>(max_order - options.min_fixed_order + 1U);
}

std::size_t generated_welch_candidate_count(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.max_lpc_order == 0) {
        return 0;
    }

    const auto base_lpc_tasks =
        static_cast<std::size_t>(options.max_lpc_order) * kGeneratedLpcBaseWindowCount;
    const auto non_lpc_tasks =
        (options.include_constant ? 1U : 0U) + fixed_task_count(options);
    const auto base_tasks = base_lpc_tasks + non_lpc_tasks;
    if (base_tasks >= kFlacClMaxOrder) {
        return 0;
    }

    const auto spare_tasks = kFlacClMaxOrder - base_tasks;
    return std::min<std::size_t>(
        {kGeneratedWelchCandidateTargetCount, options.max_lpc_order, spare_tasks});
}

LDCOMPRESS_OPENCL_ONLY_USED std::vector<float> make_generated_lpc_windows(
    std::size_t blocksize,
    std::size_t window_count)
{
    std::vector<float> windows(blocksize * window_count, 1.0f);
    if (window_count < 2 || blocksize <= 1) {
        return windows;
    }

    auto* tukey = windows.data() + blocksize;
    const auto edge_width = static_cast<std::size_t>(
        (kGeneratedTukeyTaperFraction / 2.0) * static_cast<double>(blocksize));
    if (edge_width != 0) {
        const auto np = edge_width - 1U;
        if (np == 0) {
            tukey[0] = 0.0f;
            tukey[blocksize - 1U] = 0.0f;
        } else {
            for (std::size_t n = 0; n <= np; ++n) {
                const auto left_weight =
                    0.5 - (0.5 * std::cos(kPi * static_cast<double>(n) / static_cast<double>(np)));
                const auto right_weight =
                    0.5 - (0.5 * std::cos(kPi * static_cast<double>(n + np) / static_cast<double>(np)));
                tukey[n] = static_cast<float>(left_weight);
                tukey[blocksize - np - 1U + n] = static_cast<float>(right_weight);
            }
        }
    }
    if (window_count < 3) {
        return windows;
    }

    auto* welch = windows.data() + (2U * blocksize);
    const auto endpoint = static_cast<double>(blocksize - 1U);
    const auto midpoint = endpoint / 2.0;
    for (std::size_t n = 0; n < blocksize; ++n) {
        const auto k = (static_cast<double>(n) - midpoint) / midpoint;
        welch[n] = static_cast<float>(1.0 - (k * k));
    }
    return windows;
}

FlacClSubframeTask make_common_task(
    const OpenClMonoAnalysisTaskOptions& options,
    std::size_t frame_index,
    std::int32_t type,
    unsigned residual_order)
{
    const auto samples_offset = checked_i32(
        static_cast<std::uint64_t>(frame_index) * options.frame_samples,
        "OpenCL mono analysis samplesOffs");
    const auto raw_bits = checked_i32(
        static_cast<std::uint64_t>(options.bits_per_sample) * options.frame_samples,
        "OpenCL mono analysis size");

    FlacClSubframeTask task;
    task.data.residualOrder = checked_i32(residual_order, "OpenCL mono analysis residualOrder");
    task.data.samplesOffs = samples_offset;
    task.data.shift = 0;
    task.data.cbits = 0;
    task.data.size = raw_bits;
    task.data.type = type;
    task.data.obits = checked_i32(options.bits_per_sample, "OpenCL mono analysis obits");
    task.data.blocksize = checked_i32(options.frame_samples, "OpenCL mono analysis blocksize");
    task.data.coding_method = options.bits_per_sample > 16 ? 1 : 0;
    task.data.channel = 0;
    task.data.residualOffs = samples_offset;
    task.data.wbits = 0;
    task.data.abits = task.data.obits;
    task.data.porder = 0;
    task.data.headerLen = 0;
    task.data.encodingOffset = 0;
    return task;
}

void populate_fixed_coefficients(FlacClSubframeTask& task, unsigned order)
{
    switch (order) {
    case 0:
        break;
    case 1:
        task.coefs[0] = 1;
        break;
    case 2:
        task.coefs[1] = 2;
        task.coefs[0] = -1;
        break;
    case 3:
        task.coefs[2] = 3;
        task.coefs[1] = -3;
        task.coefs[0] = 1;
        break;
    case 4:
        task.coefs[3] = 4;
        task.coefs[2] = -6;
        task.coefs[1] = 4;
        task.coefs[0] = -1;
        break;
    default:
        throw std::runtime_error("invalid FLAC fixed predictor order");
    }
}

constexpr std::size_t kOpenClAnalysisMaxBlockSize = 8192;
constexpr std::size_t kOpenClAnalysisBitsPerSample = 16;
constexpr unsigned kExactMaxRicePartitionOrder = 8;
constexpr unsigned kExactMaxRiceParameter = 14;

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("OpenCL mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("OpenCL mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis best-method smoke supports full mono selection only");
    }
    if (plan.selected_tasks.size() % plan.estimate_tasks_per_frame != 0) {
        throw std::runtime_error("OpenCL mono analysis selected task count is not frame-aligned");
    }
    if (plan.residual_tasks.size() % plan.residual_tasks_per_frame != 0) {
        throw std::runtime_error("OpenCL mono analysis residual task count is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis selected/residual frame counts differ");
    }
    if (plan.estimate_tasks_per_frame >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("OpenCL mono analysis task count exceeds OpenCL int argument");
    }
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 ||
            static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("OpenCL mono analysis selected task index is out of range");
        }
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("OpenCL mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base ||
                selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("OpenCL mono analysis selected task crosses frame group");
            }
        }
    }
}

std::size_t mono_plan_frame_count(const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    return plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
}

GeneratedLpcPrefixShape generated_lpc_prefix_shape(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::size_t frame)
{
    const auto task_base = frame * plan.residual_tasks_per_frame;
    std::size_t total_lpc_tasks = 0;
    while (total_lpc_tasks < plan.residual_tasks_per_frame &&
        plan.residual_tasks[task_base + total_lpc_tasks].data.type == kFlacClSubframeLpc) {
        ++total_lpc_tasks;
    }

    if (total_lpc_tasks == 0) {
        throw std::runtime_error("OpenCL mono generated analysis requires an LPC task prefix");
    }

    std::size_t lpc_tasks_per_window = 0;
    while (lpc_tasks_per_window < total_lpc_tasks &&
        plan.residual_tasks[task_base + lpc_tasks_per_window].data.residualOrder ==
            static_cast<std::int32_t>(lpc_tasks_per_window + 1U)) {
        ++lpc_tasks_per_window;
    }
    if (lpc_tasks_per_window == 0 || lpc_tasks_per_window > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono generated analysis has invalid LPC tasks per window");
    }
    const auto window_count =
        (total_lpc_tasks + lpc_tasks_per_window - 1U) / lpc_tasks_per_window;
    for (std::size_t window = 0; window < window_count; ++window) {
        const auto window_base = task_base + window * lpc_tasks_per_window;
        const auto remaining = total_lpc_tasks - (window * lpc_tasks_per_window);
        const auto slots_this_window = std::min(lpc_tasks_per_window, remaining);
        for (std::size_t slot = 0; slot < slots_this_window; ++slot) {
            const auto& task = plan.residual_tasks[window_base + slot];
            if (task.data.type != kFlacClSubframeLpc ||
                task.data.residualOrder <= 0 ||
                task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
                throw std::runtime_error("OpenCL mono generated analysis LPC window group is invalid");
            }
            if (slots_this_window == lpc_tasks_per_window &&
                task.data.residualOrder != static_cast<std::int32_t>(slot + 1U)) {
                throw std::runtime_error("OpenCL mono generated analysis full LPC window group is not sequential");
            }
        }
    }

    return GeneratedLpcPrefixShape {
        .lpc_tasks_per_window = lpc_tasks_per_window,
        .window_count = window_count,
        .total_lpc_tasks = total_lpc_tasks,
    };
}

bool signed_value_fits_bits(std::int32_t value, unsigned bits)
{
    if (bits == 0 || bits > 31) {
        return false;
    }
    const auto min_value = -(std::int64_t {1} << (bits - 1U));
    const auto max_value = (std::int64_t {1} << (bits - 1U)) - 1;
    return value >= min_value && value <= max_value;
}

void validate_fixed_constant_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono fixed/constant analysis has no frames");
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeConstant &&
            task.data.type != kFlacClSubframeFixed) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis received non-fixed task");
        }
        if (task.data.type == kFlacClSubframeFixed &&
            (task.data.residualOrder < 0 || task.data.residualOrder > 4)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis received invalid fixed order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis block size is unsupported");
        }
        if (task.data.type == kFlacClSubframeFixed &&
            task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis fixed order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis task samples are out of range");
        }
        if (task.data.shift < 0 || task.data.shift > 30) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis task shift is unsupported");
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED void validate_lpc_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono LPC analysis has no frames");
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeLpc) {
            throw std::runtime_error("OpenCL mono LPC analysis received non-LPC task");
        }
        if (task.data.residualOrder <= 0 ||
            task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
            throw std::runtime_error("OpenCL mono LPC analysis received invalid LPC order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono LPC analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono LPC analysis block size is unsupported");
        }
        if (task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono LPC analysis LPC order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono LPC analysis task samples are out of range");
        }
        if (task.data.shift < 0 || task.data.shift > 15) {
            throw std::runtime_error("OpenCL mono LPC analysis task shift is unsupported");
        }
        if (task.data.cbits <= 0 || task.data.cbits > 15) {
            throw std::runtime_error("OpenCL mono LPC analysis coefficient precision is unsupported");
        }
        for (int i = 0; i < task.data.residualOrder; ++i) {
            if (!signed_value_fits_bits(task.coefs[static_cast<std::size_t>(i)],
                    static_cast<unsigned>(task.data.cbits))) {
                throw std::runtime_error("OpenCL mono LPC analysis coefficient does not fit precision");
            }
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED void validate_lpc_generation_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono generated LPC analysis has no frames");
    }
    if (plan.residual_tasks_per_frame > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono generated LPC analysis has too many LPC tasks per frame");
    }
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL mono generated LPC analysis coefficient precision must be 1..15");
    }

    std::optional<std::int32_t> blocksize;
    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeLpc) {
            throw std::runtime_error("OpenCL mono generated LPC analysis received non-LPC task");
        }
        if (task.data.residualOrder <= 0 ||
            task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis received invalid LPC order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis block size is unsupported");
        }
        if (task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis LPC order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis task samples are out of range");
        }
        if (!blocksize.has_value()) {
            blocksize = task.data.blocksize;
        } else if (*blocksize != task.data.blocksize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis requires one block size per launch");
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED GeneratedLpcPrefixShape validate_generated_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono generated analysis has no frames");
    }
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL mono generated analysis coefficient precision must be 1..15");
    }

    std::optional<GeneratedLpcPrefixShape> lpc_shape;
    std::optional<std::int32_t> blocksize;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto frame_lpc_shape = generated_lpc_prefix_shape(plan, frame);
        if (!lpc_shape.has_value()) {
            lpc_shape = frame_lpc_shape;
        } else if (lpc_shape->lpc_tasks_per_window != frame_lpc_shape.lpc_tasks_per_window ||
            lpc_shape->window_count != frame_lpc_shape.window_count ||
            lpc_shape->total_lpc_tasks != frame_lpc_shape.total_lpc_tasks) {
            throw std::runtime_error("OpenCL mono generated analysis LPC prefix differs by frame");
        }

        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks[task_base + i];
            if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
                throw std::runtime_error("OpenCL mono generated analysis currently supports 16-bit tasks only");
            }
            if (task.data.blocksize <= 0 ||
                static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
                throw std::runtime_error("OpenCL mono generated analysis block size is unsupported");
            }
            if (task.data.samplesOffs < 0 ||
                static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
                static_cast<std::size_t>(task.data.blocksize) >
                    samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
                throw std::runtime_error("OpenCL mono generated analysis task samples are out of range");
            }
            if (!blocksize.has_value()) {
                blocksize = task.data.blocksize;
            } else if (*blocksize != task.data.blocksize) {
                throw std::runtime_error("OpenCL mono generated analysis requires one block size per launch");
            }

            if (i < frame_lpc_shape.total_lpc_tasks) {
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("OpenCL mono generated analysis LPC order exceeds block size");
                }
            } else if (task.data.type == kFlacClSubframeLpc) {
                throw std::runtime_error("OpenCL mono generated analysis received non-prefix LPC task");
            } else if (task.data.type == kFlacClSubframeFixed) {
                if (task.data.residualOrder < 0 || task.data.residualOrder > 4) {
                    throw std::runtime_error("OpenCL mono generated analysis received invalid fixed order");
                }
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("OpenCL mono generated analysis fixed order exceeds block size");
                }
                if (task.data.shift < 0 || task.data.shift > 30) {
                    throw std::runtime_error("OpenCL mono generated analysis fixed task shift is unsupported");
                }
            } else if (task.data.type != kFlacClSubframeConstant) {
                throw std::runtime_error("OpenCL mono generated analysis received unsupported task type");
            }
        }
    }

    return *lpc_shape;
}

void validate_sample_range(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("OpenCL exact analysis sample is outside the selected bit depth");
    }
}

unsigned sample_trailing_zero_bits(std::int32_t sample, unsigned bits_per_sample)
{
    if (sample == 0) {
        return bits_per_sample;
    }

    const auto mask = (std::uint64_t {1} << bits_per_sample) - 1U;
    auto raw = static_cast<std::uint64_t>(sample) & mask;

    unsigned zeros = 0;
    while ((raw & 1U) == 0U && zeros < bits_per_sample) {
        raw >>= 1U;
        ++zeros;
    }
    return zeros;
}

unsigned bit_width_u32(std::uint32_t value)
{
    unsigned bits = 0;
    while (value != 0) {
        value >>= 1U;
        ++bits;
    }
    return bits;
}

unsigned common_wasted_bits(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = samples.at(offset + i);
        validate_sample_range(sample, bits_per_sample);
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
        if (wasted_bits == 0) {
            return 0;
        }
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

unsigned frame_amplitude_bits(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned wasted_bits)
{
    std::uint32_t amplitude_or = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = samples.at(offset + i);
        const auto folded = sample < 0
            ? static_cast<std::uint32_t>(-(static_cast<std::int64_t>(sample) + 1))
            : static_cast<std::uint32_t>(sample);
        amplitude_or |= folded;
    }
    if (amplitude_or == 0) {
        return 1;
    }
    const auto bits = bit_width_u32(amplitude_or);
    return std::max(1U, bits > wasted_bits ? bits - wasted_bits : 0U);
}

bool all_frame_samples_equal(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count)
{
    const auto first = samples.at(offset);
    for (std::size_t i = 1; i < count; ++i) {
        if (samples.at(offset + i) != first) {
            return false;
        }
    }
    return true;
}

std::vector<std::int32_t> shifted_frame_samples(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned wasted_bits)
{
    std::vector<std::int32_t> shifted;
    shifted.reserve(count);
    if (wasted_bits == 0) {
        shifted.insert(shifted.end(), samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
        return shifted;
    }

    const auto divisor = std::int64_t {1} << wasted_bits;
    for (std::size_t i = 0; i < count; ++i) {
        shifted.push_back(static_cast<std::int32_t>(
            static_cast<std::int64_t>(samples.at(offset + i)) / divisor));
    }
    return shifted;
}

std::uint64_t subframe_wasted_bits_overhead(unsigned wasted_bits)
{
    return wasted_bits == 0 ? 0 : wasted_bits;
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample, unsigned wasted_bits)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + (bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

std::int64_t fixed_prediction_residual(
    const std::vector<std::int32_t>& samples,
    std::size_t index,
    unsigned order)
{
    switch (order) {
    case 0:
        return samples[index];
    case 1:
        return static_cast<std::int64_t>(samples[index]) - samples[index - 1];
    case 2:
        return static_cast<std::int64_t>(samples[index]) -
            (2LL * samples[index - 1]) + samples[index - 2];
    case 3:
        return static_cast<std::int64_t>(samples[index]) -
            (3LL * samples[index - 1]) + (3LL * samples[index - 2]) -
            samples[index - 3];
    case 4:
        return static_cast<std::int64_t>(samples[index]) -
            (4LL * samples[index - 1]) + (6LL * samples[index - 2]) -
            (4LL * samples[index - 3]) + samples[index - 4];
    default:
        throw std::runtime_error("OpenCL exact analysis received invalid fixed order");
    }
}

std::vector<std::int64_t> fixed_residuals(
    const std::vector<std::int32_t>& samples,
    unsigned order)
{
    std::vector<std::int64_t> residuals;
    residuals.reserve(samples.size() - order);
    for (std::size_t i = order; i < samples.size(); ++i) {
        residuals.push_back(fixed_prediction_residual(samples, i, order));
    }
    return residuals;
}

std::uint64_t fold_signed_residual(std::int64_t residual)
{
    if (residual >= 0) {
        return static_cast<std::uint64_t>(residual) << 1U;
    }
    return (static_cast<std::uint64_t>(-(residual + 1)) << 1U) + 1U;
}

std::uint64_t rice_bits(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count,
    unsigned parameter)
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto folded = fold_signed_residual(residuals.at(offset + i));
        bits += (folded >> parameter) + 1U + parameter;
    }
    return bits;
}

unsigned choose_rice_parameter(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count)
{
    if (offset > residuals.size() || count > residuals.size() - offset) {
        throw std::runtime_error("OpenCL exact analysis residual partition range error");
    }

    std::array<std::uint64_t, kExactMaxRiceParameter + 1U> bit_counts {};
    for (unsigned parameter = 0; parameter <= kExactMaxRiceParameter; ++parameter) {
        bit_counts[parameter] = static_cast<std::uint64_t>(count) * (1U + parameter);
    }

    const auto end = offset + count;
    for (std::size_t i = offset; i < end; ++i) {
        const auto folded = fold_signed_residual(residuals[i]);
        for (unsigned parameter = 0; parameter <= kExactMaxRiceParameter; ++parameter) {
            bit_counts[parameter] += folded >> parameter;
        }
    }

    unsigned best_parameter = 0;
    auto best_bits = bit_counts[0];
    for (unsigned parameter = 1; parameter <= kExactMaxRiceParameter; ++parameter) {
        if (bit_counts[parameter] < best_bits) {
            best_bits = bit_counts[parameter];
            best_parameter = parameter;
        }
    }
    return best_parameter;
}

bool valid_partition_order(std::size_t block_size, unsigned predictor_order, unsigned partition_order)
{
    const auto partition_count = std::size_t {1} << partition_order;
    if ((block_size % partition_count) != 0) {
        return false;
    }
    const auto partition_samples = block_size / partition_count;
    return partition_samples > predictor_order;
}

std::size_t partition_residual_count(
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order,
    std::size_t partition)
{
    const auto partition_samples = block_size >> partition_order;
    return partition == 0
        ? partition_samples - predictor_order
        : partition_samples;
}

std::uint64_t fixed_rice_subframe_bits(
    unsigned order,
    unsigned partition_order,
    unsigned wasted_bits,
    const std::vector<unsigned>& rice_parameters,
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    constexpr unsigned kRiceMethodBits = 2;
    constexpr unsigned kPartitionOrderBits = 4;
    constexpr unsigned kRiceParameterBits = 4;

    std::uint64_t bits =
        kSubframeHeaderBits + (static_cast<std::uint64_t>(order) * bits_per_sample) +
        kRiceMethodBits + kPartitionOrderBits + subframe_wasted_bits_overhead(wasted_bits);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            block_size, order, partition_order, partition);
        bits += kRiceParameterBits;
        bits += rice_bits(residuals, residual_offset, residual_count,
            rice_parameters.at(partition));
        residual_offset += residual_count;
    }
    if (residual_offset != residuals.size()) {
        throw std::runtime_error("OpenCL exact analysis residual partition accounting error");
    }
    return bits;
}

struct ExactFixedRiceChoice {
    std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
    unsigned partition_order = 0;
};

ExactFixedRiceChoice choose_exact_fixed_rice(
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned order,
    unsigned wasted_bits,
    unsigned effective_bits_per_sample,
    unsigned max_rice_partition_order)
{
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL exact analysis max Rice partition order must be 0..8");
    }

    ExactFixedRiceChoice best;
    for (unsigned partition_order = 0; partition_order <= max_rice_partition_order; ++partition_order) {
        if (!valid_partition_order(block_size, order, partition_order)) {
            continue;
        }

        const auto partition_count = std::size_t {1} << partition_order;
        std::vector<unsigned> rice_parameters;
        rice_parameters.reserve(partition_count);
        std::size_t residual_offset = 0;
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            const auto residual_count = partition_residual_count(
                block_size, order, partition_order, partition);
            rice_parameters.push_back(choose_rice_parameter(
                residuals, residual_offset, residual_count));
            residual_offset += residual_count;
        }
        if (residual_offset != residuals.size()) {
            throw std::runtime_error("OpenCL exact analysis residual partition accounting error");
        }

        const auto bits = fixed_rice_subframe_bits(
            order, partition_order, wasted_bits, rice_parameters, residuals,
            block_size, effective_bits_per_sample);
        if (bits < best.bits) {
            best.bits = bits;
            best.partition_order = partition_order;
        }
    }

    if (best.bits == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("OpenCL exact analysis found no valid Rice partition order");
    }
    return best;
}

void analyze_fixed_constant_task_exact(
    const std::vector<std::int32_t>& samples,
    FlacClSubframeTask& task,
    unsigned max_rice_partition_order)
{
    const auto offset = static_cast<std::size_t>(task.data.samplesOffs);
    const auto block_size = static_cast<std::size_t>(task.data.blocksize);
    const auto bits_per_sample = static_cast<unsigned>(task.data.obits);
    const auto wasted_bits = common_wasted_bits(samples, offset, block_size, bits_per_sample);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;

    task.data.wbits = checked_i32(wasted_bits, "OpenCL exact analysis wbits");
    task.data.abits = checked_i32(
        frame_amplitude_bits(samples, offset, block_size, wasted_bits),
        "OpenCL exact analysis abits");
    task.data.porder = 0;

    if (task.data.type == kFlacClSubframeConstant) {
        if (!all_frame_samples_equal(samples, offset, block_size)) {
            task.data.size = std::numeric_limits<std::int32_t>::max();
            return;
        }
        task.data.size = checked_i32(
            constant_subframe_bits(bits_per_sample, wasted_bits),
            "OpenCL exact analysis constant size");
        return;
    }

    const auto order = static_cast<unsigned>(task.data.residualOrder);
    const auto shifted = shifted_frame_samples(samples, offset, block_size, wasted_bits);
    const auto residuals = fixed_residuals(shifted, order);
    const auto choice = choose_exact_fixed_rice(
        residuals, block_size, order, wasted_bits, effective_bits_per_sample,
        max_rice_partition_order);
    task.data.porder = checked_i32(choice.partition_order, "OpenCL exact analysis porder");
    task.data.size = checked_i32(choice.bits, "OpenCL exact analysis fixed size");
}

std::vector<FlacClSubframeTask> choose_best_tasks(
    const std::vector<FlacClSubframeTask>& tasks,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    if (tasks.size() != plan.residual_tasks.size()) {
        throw std::runtime_error("OpenCL exact analysis task count mismatch");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> best_tasks;
    best_tasks.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        auto best_index = plan.selected_tasks.at(selected_base);
        auto best_size = tasks.at(static_cast<std::size_t>(best_index)).data.size;
        for (std::size_t i = 1; i < plan.estimate_tasks_per_frame; ++i) {
            const auto task_index = plan.selected_tasks.at(selected_base + i);
            const auto task_size = tasks.at(static_cast<std::size_t>(task_index)).data.size;
            if (task_size < best_size) {
                best_index = task_index;
                best_size = task_size;
            }
        }
        best_tasks.push_back(tasks.at(static_cast<std::size_t>(best_index)));
    }
    return best_tasks;
}

#if LDCOMPRESS_HAVE_OPENCL

constexpr cl_int kPlatformNotFound = -1001;

const char* mono_analysis_kernel_source()
{
    return R"CLC(
/*
 * Portions derived from CUETools.FLACCL: FLAC audio encoder using OpenCL
 * Copyright (c) 2010-2022 Gregory S. Chudov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Local modifications for ld-compress-ng, 2026-07-04:
 * - Reduced to mono analysis kernels used by the native LaserDisc RF encoder.
 * - Uses the FLACCLSubframeTask ABI but omits stereo, Rice-output, and
 *   bitstream output kernels.
 * - Adds a mono LPC autocorrelation/coefficient-generation path with
 *   rectangular and Tukey-window candidates that feeds the exact residual
 *   analyzer without FLACCL's order-pruning pass.
 * - Keeps the CPU-style one-work-item estimate path and adds exact fixed/LPC
 *   Rice partition analysis for scalar parity testing.
 */

#pragma OPENCL FP_CONTRACT OFF

#define FLACCL_CPU
#define MAX_ORDER 32
#define GROUP_SIZE 1
#define BITS_PER_SAMPLE 16
#define MAX_BLOCKSIZE 8192

#if BITS_PER_SAMPLE > 16
#define MAX_RICE_PARAM 30
#define RICE_PARAM_BITS 5
#else
#define MAX_RICE_PARAM 14
#define RICE_PARAM_BITS 4
#endif

typedef enum
{
    Constant = 0,
    Verbatim = 1,
    Fixed = 8,
    LPC = 32
} SubframeType;

typedef struct
{
    int residualOrder;
    int samplesOffs;
    int shift;
    int cbits;
    int size;
    int type;
    int obits;
    int blocksize;
    int coding_method;
    int channel;
    int residualOffs;
    int wbits;
    int abits;
    int porder;
    int headerLen;
    int encodingOffset;
} FLACCLSubframeData;

typedef struct
{
    FLACCLSubframeData data;
    int coefs[32];
} FLACCLSubframeTask;

#define __ffs(a) (32 - clz((a) & (-(a))))

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressFindWastedBits(
    __global FLACCLSubframeTask* tasks,
    __global const int* samples,
    int tasksPerChannel)
{
    __global FLACCLSubframeTask* ptask = &tasks[get_group_id(0) * tasksPerChannel];
    int w_or = 0;
    int a_or = 0;

    for (int pos = 0; pos < ptask->data.blocksize; pos++)
    {
        const int smp = samples[ptask->data.samplesOffs + pos];
        w_or |= smp;
        a_or |= smp ^ (smp >> 31);
    }

    const int w = w_or == 0 ? ptask->data.obits - 1 : max(0, __ffs(w_or) - 1);
    const int a = a_or == 0 ? 1 : max(1, 32 - clz(a_or) - w);

    for (int i = 0; i < tasksPerChannel; i++)
    {
        ptask[i].data.wbits = w;
        ptask[i].data.abits = a;
    }
}

long ldcompressShiftedSample(__global const int* data, int pos, int wbits);

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressComputeAutocor(
    __global float* output,
    __global const int* samples,
    __global const float* window,
    __global FLACCLSubframeTask* tasks,
    int tasksPerFrame)
{
    FLACCLSubframeData task = tasks[get_group_id(0) * tasksPerFrame].data;
    const int blocksize = task.blocksize;
    const int windowOffset = get_group_id(1) * blocksize;
    __global float* out =
        &output[(get_group_id(0) * get_num_groups(1) + get_group_id(1)) * (MAX_ORDER + 1)];

    for (int lag = 0; lag <= MAX_ORDER; lag++)
    {
        float sum = 0.0f;
        for (int pos = lag; pos < blocksize; pos++)
        {
            const float sample0 =
                (float)ldcompressShiftedSample(samples, task.samplesOffs + pos, task.wbits) *
                window[windowOffset + pos];
            const float sample1 =
                (float)ldcompressShiftedSample(samples, task.samplesOffs + pos - lag, task.wbits) *
                window[windowOffset + pos - lag];
            sum += sample0 * sample1;
        }
        out[lag] = sum;
    }
}

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressComputeLpc(
    __global const float* autocor,
    __global float* lpcs,
    int windowCount)
{
    const int frame = get_group_id(0);
    const int window = get_group_id(1);
    const int lpcOffset =
        (frame * windowCount + window) * (MAX_ORDER + 1) * 32;
    const int autocorOffset =
        (frame * windowCount + window) * (MAX_ORDER + 1);
    __global const float* autoc = &autocor[autocorOffset];

    float ldr[32];
    float gen0[32];
    float gen1[32];
    float err[32];

    for (int i = 0; i < MAX_ORDER; i++)
    {
        gen0[i] = autoc[i + 1];
        gen1[i] = autoc[i + 1];
        ldr[i] = 0.0f;
        err[i] = 0.0f;
    }

    float error = autoc[0];
    for (int order = 0; order < MAX_ORDER; order++)
    {
        float reflection = 0.0f;
        if (error > 0.0f)
            reflection = -gen1[0] / error;
        if (!isfinite(reflection))
            reflection = 0.0f;

        error *= 1.0f - (reflection * reflection);
        if (!isfinite(error) || error < 0.0f)
            error = 0.0f;

        for (int j = 0; j < MAX_ORDER - 1 - order; j++)
        {
            const float nextGen1 = gen1[j + 1] + (reflection * gen0[j]);
            gen0[j] = gen1[j + 1] * reflection + gen0[j];
            gen1[j] = nextGen1;
        }

        err[order] = error;

        ldr[order] = reflection;
        for (int j = 0; j < order / 2; j++)
        {
            const float tmp = ldr[j];
            ldr[j] += reflection * ldr[order - 1 - j];
            ldr[order - 1 - j] += reflection * tmp;
        }
        if ((order & 1) != 0)
            ldr[order / 2] += ldr[order / 2] * reflection;

        for (int j = 0; j <= order; j++)
            lpcs[lpcOffset + order * 32 + j] = -ldr[order - j];
    }

    for (int j = 0; j < MAX_ORDER; j++)
        lpcs[lpcOffset + MAX_ORDER * 32 + j] = err[j];
}

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressQuantizeLpcOrders(
    __global FLACCLSubframeTask* tasks,
    __global const float* lpcs,
    int tasksPerFrame,
    int lpcTasksPerWindow,
    int totalLpcTasks,
    int coefficientPrecision)
{
    const int frame = get_group_id(0);
    const int window = get_group_id(1);
    const int lpcOffset =
        (frame * get_num_groups(1) + window) * (MAX_ORDER + 1) * 32;
    const int windowTaskOffset = window * lpcTasksPerWindow;
    const int slotsThisWindow = min(lpcTasksPerWindow, totalLpcTasks - windowTaskOffset);

    for (int slot = 0; slot < slotsThisWindow; slot++)
    {
        const int taskNo = frame * tasksPerFrame + windowTaskOffset + slot;
        FLACCLSubframeTask task = tasks[taskNo];
        const int order = task.data.residualOrder;

        int maxCoefficient = 0;
        for (int i = 0; i < order; i++)
        {
            const float lpc = lpcs[lpcOffset + (order - 1) * 32 + i];
            const int coefficient = convert_int_rte(lpc * (float)(1 << 15));
            maxCoefficient |= coefficient ^ (coefficient >> 31);
        }

        const int desiredBits = clamp(coefficientPrecision, 1, 15);
        int shift = 0;
        if (maxCoefficient != 0)
            shift = clamp(clz(maxCoefficient) - 18 + desiredBits, 0, 15);
        const int limit = (1 << (desiredBits - 1)) - 1;

        int actualMaxCoefficient = 0;
        for (int i = 0; i < MAX_ORDER; i++)
            task.coefs[i] = 0;
        for (int i = 0; i < order; i++)
        {
            const float lpc = lpcs[lpcOffset + (order - 1) * 32 + i];
            const int coefficient = clamp(
                convert_int_rte(lpc * (float)(1 << shift)), -limit, limit);
            actualMaxCoefficient |= coefficient ^ (coefficient >> 31);
            task.coefs[i] = coefficient;
        }

        task.data.residualOrder = order;
        task.data.shift = shift;
        task.data.cbits = actualMaxCoefficient == 0
            ? 1
            : 1 + 32 - clz(actualMaxCoefficient);
        tasks[taskNo] = task;
    }
}

#define TEMPBLOCK 512
#define TEMPBLOCK1 TEMPBLOCK

__kernel __attribute__((vec_type_hint(int4))) __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressEstimateResidual(
    __global const int* samples,
    __global const int* selectedTasks,
    __global FLACCLSubframeTask* tasks)
{
    const int selectedTask = selectedTasks[get_group_id(0)];
    FLACCLSubframeTask task = tasks[selectedTask];
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
#define ERPARTS (MAX_BLOCKSIZE >> 6)
    float len[ERPARTS];

    __global const int* data = &samples[task.data.samplesOffs];
    for (int i = 0; i < ERPARTS; i++)
        len[i] = 0.0f;

    if (ro <= 4)
    {
        float fcoef[4];
        for (int tid = 0; tid < 4; tid++)
            fcoef[tid] = tid + ro - 4 < 0 ? 0.0f : -((float)task.coefs[tid + ro - 4]) / (1 << task.data.shift);
        float4 fc0 = vload4(0, &fcoef[0]);
        float fdata[4];
        for (int pos = 0; pos < 4; pos++)
            fdata[pos] = pos + ro - 4 < 0 ? 0.0f : (float)(data[pos + ro - 4] >> task.data.wbits);
        float4 fd0 = vload4(0, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float4 sum4 = fc0 * fd0;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s1230;
            fd0.s3 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd0.s3 + (sum2.x + sum2.y));
        }
    }
    else if (ro <= 8)
    {
        float fcoef[8];
        for (int tid = 0; tid < 8; tid++)
            fcoef[tid] = tid + ro - 8 < 0 ? 0.0f : -((float)task.coefs[tid + ro - 8]) / (1 << task.data.shift);
        float8 fc0 = vload8(0, &fcoef[0]);
        float fdata[8];
        for (int pos = 0; pos < 8; pos++)
            fdata[pos] = pos + ro - 8 < 0 ? 0.0f : (float)(data[pos + ro - 8] >> task.data.wbits);
        float8 fd0 = vload8(0, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float8 sum8 = fc0 * fd0;
            float4 sum4 = sum8.s0123 + sum8.s4567;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s12345670;
            fd0.s7 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd0.s7 + (sum2.x + sum2.y));
        }
    }
    else if (ro <= 12)
    {
        float fcoef[12];
        for (int tid = 0; tid < 12; tid++)
            fcoef[tid] = tid + ro - 12 >= 0 ? -((float)task.coefs[tid + ro - 12]) / (1 << task.data.shift) : 0.0f;
        float4 fc0 = vload4(0, &fcoef[0]);
        float4 fc1 = vload4(1, &fcoef[0]);
        float4 fc2 = vload4(2, &fcoef[0]);
        float fdata[12];
        for (int pos = 0; pos < 12; pos++)
            fdata[pos] = pos + ro - 12 < 0 ? 0.0f : (float)(data[pos + ro - 12] >> task.data.wbits);
        float4 fd0 = vload4(0, &fdata[0]);
        float4 fd1 = vload4(1, &fdata[0]);
        float4 fd2 = vload4(2, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float4 sum4 = fc0 * fd0 + fc1 * fd1 + fc2 * fd2;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s1230;
            fd1 = fd1.s1230;
            fd2 = fd2.s1230;
            fd0.s3 = fd1.s3;
            fd1.s3 = fd2.s3;
            fd2.s3 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd2.s3 + (sum2.x + sum2.y));
        }
    }
    else
    {
        float fcoef[32];
        for (int tid = 0; tid < 32; tid++)
            fcoef[tid] = tid < MAX_ORDER && tid + ro - MAX_ORDER >= 0 ? -((float)task.coefs[tid + ro - MAX_ORDER]) / (1 << task.data.shift) : 0.0f;

        float4 fc0 = vload4(0, &fcoef[0]);
        float4 fc1 = vload4(1, &fcoef[0]);
        float4 fc2 = vload4(2, &fcoef[0]);

        float fdata[MAX_ORDER + TEMPBLOCK1 + 32];
        for (int pos = 0; pos < MAX_ORDER; pos++)
            fdata[pos] = 0.0f;
        for (int pos = MAX_ORDER + TEMPBLOCK1; pos < MAX_ORDER + TEMPBLOCK1 + 32; pos++)
            fdata[pos] = 0.0f;
        for (int bpos = 0; bpos < bs; bpos += TEMPBLOCK1)
        {
            const int end = min(bpos + TEMPBLOCK1, bs);

            for (int pos = max(bpos - ro, 0); pos < max(bpos, ro); pos++)
                fdata[MAX_ORDER + pos - bpos] = (float)(data[pos] >> task.data.wbits);

            for (int pos = max(bpos, ro); pos < end; pos++)
            {
                float next = (float)(data[pos] >> task.data.wbits);
                float* dptr = fdata + pos - bpos;
                dptr[MAX_ORDER] = next;
                float4 sum =
                    fc0 * vload4(0, dptr) +
                    fc1 * vload4(1, dptr) +
                    fc2 * vload4(2, dptr);
                next += sum.x + sum.y + sum.z + sum.w;
                len[pos >> 6] += fabs(next);
            }
        }
    }

    int total = 0;
    for (int i = 0; i < ERPARTS; i++)
    {
        const int res = convert_int_sat_rte(len[i] * 2);
        const int k = clamp(31 - clz(res) - 6, 0, MAX_RICE_PARAM);
        total += (k << 6) + (res >> k);
    }
    const int partLen = min(0x7ffffff, total) + (bs - ro);
    const int obits = task.data.obits - task.data.wbits;
    const int predicted =
        task.data.type == Fixed ? ro * obits + 6 + RICE_PARAM_BITS + partLen :
        task.data.type == LPC ? ro * obits + 4 + 5 + ro * task.data.cbits + 6 + RICE_PARAM_BITS + partLen :
        task.data.type == Constant ? obits * (partLen != bs - ro ? bs : 1) :
        obits * bs;

    tasks[selectedTask].data.size = min(obits * bs, predicted);
}

ulong ldcompressFoldResidual(long residual)
{
    return residual >= 0
        ? ((ulong)residual) << 1
        : ((((ulong)(-(residual + 1))) << 1) + 1);
}

long ldcompressShiftedSample(__global const int* data, int pos, int wbits)
{
    const long sample = (long)data[pos];
    return wbits == 0 ? sample : sample / ((long)1 << wbits);
}

long ldcompressFixedResidual(__global const int* data, int pos, int order, int wbits)
{
    const long sample = ldcompressShiftedSample(data, pos, wbits);
    switch (order)
    {
    case 0:
        return sample;
    case 1:
        return sample - ldcompressShiftedSample(data, pos - 1, wbits);
    case 2:
        return sample -
            (2L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            ldcompressShiftedSample(data, pos - 2, wbits);
    case 3:
        return sample -
            (3L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            (3L * ldcompressShiftedSample(data, pos - 2, wbits)) -
            ldcompressShiftedSample(data, pos - 3, wbits);
    case 4:
        return sample -
            (4L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            (6L * ldcompressShiftedSample(data, pos - 2, wbits)) -
            (4L * ldcompressShiftedSample(data, pos - 3, wbits)) +
            ldcompressShiftedSample(data, pos - 4, wbits);
    default:
        return 0;
    }
}

long ldcompressArithmeticShiftRight(long value, int shift)
{
    if (shift == 0)
        return value;
    if (value >= 0)
        return value >> shift;
    const long divisor = (long)1 << shift;
    return -(((-value) + divisor - 1) >> shift);
}

long ldcompressLpcResidual(__global const int* data, int pos, FLACCLSubframeTask task)
{
    const int order = task.data.residualOrder;
    long sum = 0;
    for (int i = 0; i < order; i++)
    {
        sum += (long)task.coefs[i] *
            ldcompressShiftedSample(data, pos - order + i, task.data.wbits);
    }
    const long predicted = ldcompressArithmeticShiftRight(sum, task.data.shift);
    return ldcompressShiftedSample(data, pos, task.data.wbits) - predicted;
}

long ldcompressResidual(__global const int* data, int pos, FLACCLSubframeTask task)
{
    if (task.data.type == LPC)
        return ldcompressLpcResidual(data, pos, task);
    return ldcompressFixedResidual(
        data, pos, task.data.residualOrder, task.data.wbits);
}

int ldcompressSignedValueFitsBits(int value, int bits)
{
    if (bits <= 0 || bits > 31)
        return 0;
    const int minValue = -(1 << (bits - 1));
    const int maxValue = (1 << (bits - 1)) - 1;
    return value >= minValue && value <= maxValue;
}

int ldcompressValidPartitionOrder(int blocksize, int predictorOrder, int partitionOrder)
{
    const int partitionCount = 1 << partitionOrder;
    if ((blocksize % partitionCount) != 0)
        return 0;
    return (blocksize / partitionCount) > predictorOrder;
}

ulong ldcompressBestRiceBitsForPartition(
    __global const int* data,
    int partitionStart,
    int residualCount,
    FLACCLSubframeTask task)
{
    ulong bitCounts[MAX_RICE_PARAM + 1];
    for (int parameter = 0; parameter <= MAX_RICE_PARAM; parameter++)
        bitCounts[parameter] = (ulong)residualCount * (ulong)(1 + parameter);

    for (int i = 0; i < residualCount; i++)
    {
        const long residual = ldcompressResidual(data, partitionStart + i, task);
        const ulong folded = ldcompressFoldResidual(residual);
        for (int parameter = 0; parameter <= MAX_RICE_PARAM; parameter++)
            bitCounts[parameter] += folded >> parameter;
    }

    ulong bestBits = bitCounts[0];
    for (int parameter = 1; parameter <= MAX_RICE_PARAM; parameter++)
        if (bitCounts[parameter] < bestBits)
            bestBits = bitCounts[parameter];
    return bestBits;
}

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressAnalyzeSubframeExact(
    __global const int* samples,
    __global const int* selectedTasks,
    __global FLACCLSubframeTask* tasks,
    int maxRicePartitionOrder)
{
    const int selectedTask = selectedTasks[get_group_id(0)];
    FLACCLSubframeTask task = tasks[selectedTask];
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
    const int wbits = task.data.wbits;
    const int obits = task.data.obits - wbits;
    __global const int* data = &samples[task.data.samplesOffs];

    task.data.porder = 0;

    if (task.data.type == Constant)
    {
        int equal = 1;
        const int first = data[0];
        for (int pos = 1; pos < bs; pos++)
            if (data[pos] != first)
                equal = 0;

        if (!equal)
            task.data.size = 0x7fffffff;
        else
            task.data.size = 8 + obits + (wbits == 0 ? 0 : wbits);
        tasks[selectedTask] = task;
        return;
    }

    if ((task.data.type != Fixed && task.data.type != LPC) ||
        ro < 0 ||
        ro >= bs ||
        (task.data.type == Fixed && ro > 4) ||
        (task.data.type == LPC && (ro == 0 || ro > MAX_ORDER || task.data.shift < 0 || task.data.shift > 15 ||
                                      task.data.cbits <= 0 || task.data.cbits > 15)))
    {
        task.data.size = 0x7fffffff;
        tasks[selectedTask] = task;
        return;
    }

    if (task.data.type == LPC)
    {
        int coefficientsValid = 1;
        for (int i = 0; i < ro; i++)
            if (!ldcompressSignedValueFitsBits(task.coefs[i], task.data.cbits))
                coefficientsValid = 0;
        if (!coefficientsValid)
        {
            task.data.size = 0x7fffffff;
            tasks[selectedTask] = task;
            return;
        }
    }

    ulong bestBits = 0xffffffffffffffffUL;
    int bestPartitionOrder = 0;
    const int maxPartitionOrder = min(maxRicePartitionOrder, 8);
    for (int partitionOrder = 0; partitionOrder <= maxPartitionOrder; partitionOrder++)
    {
        if (!ldcompressValidPartitionOrder(bs, ro, partitionOrder))
            continue;

        const int partitionCount = 1 << partitionOrder;
        const int partitionSamples = bs >> partitionOrder;
        ulong bits = task.data.type == LPC
            ? 8UL +
                (wbits == 0 ? 0UL : (ulong)wbits) +
                ((ulong)ro * (ulong)obits) +
                4UL +
                5UL +
                ((ulong)ro * (ulong)task.data.cbits) +
                2UL +
                4UL
            : 8UL +
                ((ulong)ro * (ulong)obits) +
                2UL +
                4UL +
                (wbits == 0 ? 0UL : (ulong)wbits);

        for (int partition = 0; partition < partitionCount; partition++)
        {
            const int residualCount = partition == 0
                ? partitionSamples - ro
                : partitionSamples;
            const int partitionStart = partition == 0
                ? ro
                : partition * partitionSamples;
            bits += 4UL;
            bits += ldcompressBestRiceBitsForPartition(
                data, partitionStart, residualCount, task);
        }

        if (bits < bestBits)
        {
            bestBits = bits;
            bestPartitionOrder = partitionOrder;
        }
    }

    task.data.porder = bestPartitionOrder;
    task.data.size = bestBits > 0x7fffffffUL ? 0x7fffffff : (int)bestBits;
    tasks[selectedTask] = task;
}

__kernel void ldcompressChooseBestMethod(
    __global FLACCLSubframeTask* tasks_out,
    __global const FLACCLSubframeTask* tasks,
    __global const int* selectedTasks,
    int taskCount)
{
    const int base = (int)get_global_id(0) * taskCount;
    int best_no = selectedTasks[base];
    int best_len = tasks[best_no].data.size;

    for (int i = 1; i < taskCount; i++)
    {
        const int task_no = selectedTasks[base + i];
        const int task_len = tasks[task_no].data.size;
        if (task_len < best_len)
        {
            best_no = task_no;
            best_len = task_len;
        }
    }

    tasks_out[get_global_id(0)] = tasks[best_no];
}
)CLC";
}

std::string cl_error_name(cl_int status)
{
    switch (status) {
    case CL_SUCCESS:
        return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND:
        return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE:
        return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE:
        return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
        return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
        return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
        return "CL_OUT_OF_HOST_MEMORY";
    case CL_BUILD_PROGRAM_FAILURE:
        return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_VALUE:
        return "CL_INVALID_VALUE";
    case CL_INVALID_PLATFORM:
        return "CL_INVALID_PLATFORM";
    case CL_INVALID_DEVICE:
        return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT:
        return "CL_INVALID_CONTEXT";
    case CL_INVALID_COMMAND_QUEUE:
        return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_MEM_OBJECT:
        return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_BINARY:
        return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS:
        return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM:
        return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE:
        return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME:
        return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL_DEFINITION:
        return "CL_INVALID_KERNEL_DEFINITION";
    case CL_INVALID_KERNEL:
        return "CL_INVALID_KERNEL";
    case CL_INVALID_ARG_INDEX:
        return "CL_INVALID_ARG_INDEX";
    case CL_INVALID_ARG_VALUE:
        return "CL_INVALID_ARG_VALUE";
    case CL_INVALID_ARG_SIZE:
        return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_KERNEL_ARGS:
        return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_DIMENSION:
        return "CL_INVALID_WORK_DIMENSION";
    case CL_INVALID_WORK_GROUP_SIZE:
        return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_GLOBAL_OFFSET:
        return "CL_INVALID_GLOBAL_OFFSET";
    case CL_INVALID_EVENT:
        return "CL_INVALID_EVENT";
    case CL_INVALID_OPERATION:
        return "CL_INVALID_OPERATION";
    case CL_INVALID_BUFFER_SIZE:
        return "CL_INVALID_BUFFER_SIZE";
    default:
        if (status == kPlatformNotFound) {
            return "CL_PLATFORM_NOT_FOUND_KHR";
        }
        return "OpenCL error " + std::to_string(status);
    }
}

void require_cl(cl_int status, const char* operation)
{
    if (status != CL_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + cl_error_name(status));
    }
}

std::string trim_trailing_nul(std::string value)
{
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string get_device_string(cl_device_id device, cl_device_info param)
{
    std::size_t size = 0;
    require_cl(clGetDeviceInfo(device, param, 0, nullptr, &size), "clGetDeviceInfo");
    std::string value(size, '\0');
    require_cl(clGetDeviceInfo(device, param, value.size(), value.data(), nullptr),
        "clGetDeviceInfo");
    return trim_trailing_nul(std::move(value));
}

template <typename T>
T get_device_value(cl_device_id device, cl_device_info param)
{
    T value {};
    require_cl(clGetDeviceInfo(device, param, sizeof(value), &value, nullptr), "clGetDeviceInfo");
    return value;
}

struct SelectedOpenClDevice {
    cl_device_id id = nullptr;
    std::string name;
};

SelectedOpenClDevice choose_opencl_device(std::optional<std::size_t> requested_index)
{
    cl_uint platform_count = 0;
    const cl_int platform_status = clGetPlatformIDs(0, nullptr, &platform_count);
    if (platform_status == kPlatformNotFound || platform_count == 0) {
        throw std::runtime_error("no OpenCL platforms found");
    }
    require_cl(platform_status, "clGetPlatformIDs");

    std::vector<cl_platform_id> platforms(platform_count);
    require_cl(clGetPlatformIDs(platform_count, platforms.data(), nullptr), "clGetPlatformIDs");

    std::size_t flat_index = 0;
    for (const auto platform : platforms) {
        cl_uint device_count = 0;
        const cl_int device_count_status =
            clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
        if (device_count_status == CL_DEVICE_NOT_FOUND || device_count == 0) {
            continue;
        }
        require_cl(device_count_status, "clGetDeviceIDs");

        std::vector<cl_device_id> devices(device_count);
        require_cl(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr),
            "clGetDeviceIDs");

        for (const auto device : devices) {
            const bool available =
                get_device_value<cl_bool>(device, CL_DEVICE_AVAILABLE) == CL_TRUE;
            if (requested_index.has_value()) {
                if (flat_index == *requested_index) {
                    if (!available) {
                        throw std::runtime_error("selected OpenCL device is not available");
                    }
                    return SelectedOpenClDevice {device, get_device_string(device, CL_DEVICE_NAME)};
                }
            } else if (available) {
                return SelectedOpenClDevice {device, get_device_string(device, CL_DEVICE_NAME)};
            }
            ++flat_index;
        }
    }

    if (requested_index.has_value()) {
        throw std::runtime_error("OpenCL device index out of range: " +
            std::to_string(*requested_index));
    }
    throw std::runtime_error("no available OpenCL devices found");
}

template <typename Handle, cl_int (*Release)(Handle)>
class ClHandle {
public:
    ClHandle() = default;
    explicit ClHandle(Handle handle)
        : handle_(handle)
    {
    }
    ClHandle(const ClHandle&) = delete;
    ClHandle& operator=(const ClHandle&) = delete;
    ClHandle(ClHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    ClHandle& operator=(ClHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~ClHandle()
    {
        reset();
    }

    void reset(Handle handle = nullptr)
    {
        if (handle_ != nullptr) {
            (void)Release(handle_);
        }
        handle_ = handle;
    }

    Handle get() const
    {
        return handle_;
    }

private:
    Handle handle_ = nullptr;
};

using ClContext = ClHandle<cl_context, clReleaseContext>;
using ClCommandQueue = ClHandle<cl_command_queue, clReleaseCommandQueue>;
using ClProgram = ClHandle<cl_program, clReleaseProgram>;
using ClKernel = ClHandle<cl_kernel, clReleaseKernel>;
using ClMem = ClHandle<cl_mem, clReleaseMemObject>;

std::string program_build_log(cl_program program, cl_device_id device)
{
    std::size_t size = 0;
    const auto status = clGetProgramBuildInfo(
        program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
    if (status != CL_SUCCESS || size == 0) {
        return {};
    }
    std::string log(size, '\0');
    if (clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log.size(),
            log.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    return trim_trailing_nul(std::move(log));
}

struct OpenClGeneratedAnalysisRuntime {
    SelectedOpenClDevice selected_device;
    ClContext context;
    ClCommandQueue queue;
    ClProgram program;
    ClKernel wasted_kernel;
    ClKernel autocor_kernel;
    ClKernel lpc_kernel;
    ClKernel quantize_kernel;
    ClKernel exact_kernel;
    ClKernel choose_kernel;
    ClMem samples_buffer;
    ClMem tasks_buffer;
    ClMem selected_buffer;
    ClMem window_buffer;
    ClMem autocor_buffer;
    ClMem lpc_buffer;
    ClMem best_buffer;
    std::size_t samples_buffer_bytes = 0;
    std::size_t tasks_buffer_bytes = 0;
    std::size_t selected_buffer_bytes = 0;
    std::size_t window_buffer_bytes = 0;
    std::size_t autocor_buffer_bytes = 0;
    std::size_t lpc_buffer_bytes = 0;
    std::size_t best_buffer_bytes = 0;
};

OpenClGeneratedAnalysisRuntime make_opencl_generated_analysis_runtime(
    std::optional<std::size_t> requested_device_index)
{
    OpenClGeneratedAnalysisRuntime runtime;
    runtime.selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    runtime.context.reset(clCreateContext(
        nullptr, 1, &runtime.selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    runtime.queue.reset(clCreateCommandQueue(
        runtime.context.get(), runtime.selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    runtime.program.reset(clCreateProgramWithSource(
        runtime.context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(
        runtime.program.get(), 1, &runtime.selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(runtime.program.get(), runtime.selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    runtime.wasted_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressFindWastedBits", &status));
    require_cl(status, "clCreateKernel(ldcompressFindWastedBits)");
    runtime.autocor_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressComputeAutocor", &status));
    require_cl(status, "clCreateKernel(ldcompressComputeAutocor)");
    runtime.lpc_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressComputeLpc", &status));
    require_cl(status, "clCreateKernel(ldcompressComputeLpc)");
    runtime.quantize_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressQuantizeLpcOrders", &status));
    require_cl(status, "clCreateKernel(ldcompressQuantizeLpcOrders)");
    runtime.exact_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressAnalyzeSubframeExact", &status));
    require_cl(status, "clCreateKernel(ldcompressAnalyzeSubframeExact)");
    runtime.choose_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel(ldcompressChooseBestMethod)");

    return runtime;
}

void ensure_opencl_buffer(
    const ClContext& context,
    ClMem& buffer,
    std::size_t& capacity_bytes,
    cl_mem_flags flags,
    std::size_t required_bytes,
    const char* name)
{
    if (required_bytes == 0) {
        throw std::runtime_error(std::string("OpenCL ") + name + " buffer is empty");
    }
    if (required_bytes <= capacity_bytes) {
        return;
    }

    cl_int status = CL_SUCCESS;
    buffer.reset(clCreateBuffer(context.get(), flags, required_bytes, nullptr, &status));
    require_cl(status, (std::string("clCreateBuffer(") + name + ")").c_str());
    capacity_bytes = required_bytes;
}

void write_opencl_buffer(
    const ClCommandQueue& queue,
    const ClMem& buffer,
    const void* data,
    std::size_t bytes,
    const char* name)
{
    require_cl(clEnqueueWriteBuffer(
                   queue.get(), buffer.get(), CL_TRUE, 0, bytes, data, 0, nullptr, nullptr),
        (std::string("clEnqueueWriteBuffer(") + name + ")").c_str());
}

#endif

}  // namespace

std::size_t mono_analysis_tasks_per_frame(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.frame_samples == 0 || options.min_fixed_order > options.max_fixed_order ||
        options.min_fixed_order > 4 || options.max_fixed_order > 4) {
        return 0;
    }

    return (static_cast<std::size_t>(options.max_lpc_order) * kGeneratedLpcBaseWindowCount) +
        generated_welch_candidate_count(options) +
        (options.include_constant ? 1U : 0U) +
        fixed_task_count(options);
}

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan(
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options)
{
    validate_options(options);
    const auto tasks_per_frame = mono_analysis_tasks_per_frame(options);
    if (frame_count != 0 &&
        frame_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis selected task index exceeds int32");
    }
    if (frame_count != 0 &&
        frame_count - 1U >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / options.frame_samples) {
        throw std::runtime_error("OpenCL mono analysis frame offset exceeds int32");
    }

    OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks_per_frame = tasks_per_frame;
    plan.estimate_tasks_per_frame = tasks_per_frame;
    plan.residual_tasks.reserve(frame_count * tasks_per_frame);
    plan.selected_tasks.reserve(frame_count * tasks_per_frame);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_task_base = plan.residual_tasks.size();
        for (std::size_t window = 0; window < kGeneratedLpcBaseWindowCount; ++window) {
            for (unsigned order = 1; order <= options.max_lpc_order; ++order) {
                plan.residual_tasks.push_back(make_common_task(options, frame, kFlacClSubframeLpc, order));
            }
        }
        const auto welch_candidate_count = generated_welch_candidate_count(options);
        const auto first_welch_order =
            options.max_lpc_order - static_cast<unsigned>(welch_candidate_count) + 1U;
        for (std::size_t i = 0; i < welch_candidate_count; ++i) {
            plan.residual_tasks.push_back(make_common_task(
                options,
                frame,
                kFlacClSubframeLpc,
                first_welch_order + static_cast<unsigned>(i)));
        }

        if (options.include_constant) {
            auto task = make_common_task(options, frame, kFlacClSubframeConstant, 1);
            task.coefs[0] = 1;
            plan.residual_tasks.push_back(task);
        }

        const auto largest_usable = static_cast<unsigned>(options.frame_samples - 1U);
        const auto max_fixed = options.max_fixed_order < largest_usable ? options.max_fixed_order : largest_usable;
        for (unsigned order = options.min_fixed_order; order <= max_fixed; ++order) {
            auto task = make_common_task(options, frame, kFlacClSubframeFixed, order);
            populate_fixed_coefficients(task, order);
            plan.residual_tasks.push_back(task);
        }

        for (std::size_t i = 0; i < tasks_per_frame; ++i) {
            plan.selected_tasks.push_back(checked_i32(frame_task_base + i, "OpenCL mono analysis selected task"));
        }
    }

    return plan;
}

FlacSubframeDecision flaccl_task_to_subframe_decision(
    const FlacClSubframeTask& task)
{
    if (task.data.size < 0 || task.data.wbits < 0 || task.data.residualOrder < 0) {
        throw std::runtime_error("OpenCL FLACCL task contains negative decision fields");
    }

    FlacSubframeDecision decision;
    decision.wasted_bits = static_cast<unsigned>(task.data.wbits);
    decision.estimated_bits = static_cast<std::uint64_t>(task.data.size);

    switch (task.data.type) {
    case kFlacClSubframeConstant:
        decision.kind = FlacSubframeKind::Constant;
        break;
    case kFlacClSubframeFixed:
        if (task.data.porder < 0) {
            throw std::runtime_error("OpenCL FLACCL fixed task has invalid partition order");
        }
        if (task.data.residualOrder > 4) {
            throw std::runtime_error("OpenCL FLACCL fixed task has invalid order");
        }
        decision.kind = FlacSubframeKind::FixedRice;
        decision.fixed_order = static_cast<unsigned>(task.data.residualOrder);
        decision.rice_partition_order = static_cast<unsigned>(task.data.porder);
        break;
    case kFlacClSubframeLpc:
        if (task.data.porder < 0) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid partition order");
        }
        if (task.data.residualOrder == 0 ||
            task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid order");
        }
        decision.kind = FlacSubframeKind::LpcRice;
        decision.lpc_order = static_cast<unsigned>(task.data.residualOrder);
        decision.rice_partition_order = static_cast<unsigned>(task.data.porder);
        break;
    default:
        throw std::runtime_error("OpenCL FLACCL task type cannot be mapped to a native decision");
    }

    return decision;
}

FlacSelectedSubframe flaccl_task_to_selected_subframe(
    const FlacClSubframeTask& task)
{
    const auto decision = flaccl_task_to_subframe_decision(task);

    FlacSelectedSubframe selected;
    selected.kind = decision.kind;
    selected.fixed_order = decision.fixed_order;
    selected.lpc_order = decision.lpc_order;
    selected.rice_partition_order = decision.rice_partition_order;
    selected.wasted_bits = decision.wasted_bits;

    if (decision.kind == FlacSubframeKind::LpcRice) {
        if (task.data.cbits <= 0 || task.data.cbits > 15) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid coefficient precision");
        }
        if (task.data.shift < 0 || task.data.shift > 15) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid quantization shift");
        }
        selected.coefficient_precision = static_cast<unsigned>(task.data.cbits);
        selected.quantization_shift = task.data.shift;
        selected.coefficients.reserve(static_cast<std::size_t>(task.data.residualOrder));
        for (int i = task.data.residualOrder - 1; i >= 0; --i) {
            const auto coefficient = task.coefs[static_cast<std::size_t>(i)];
            if (!signed_value_fits_bits(coefficient, selected.coefficient_precision)) {
                throw std::runtime_error("OpenCL FLACCL LPC coefficient does not fit precision");
            }
            selected.coefficients.push_back(coefficient);
        }
    }

    return selected;
}

OpenClMonoFixedConstantAnalysisResult analyze_mono_fixed_constant_exact(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    validate_fixed_constant_analysis_inputs(samples, plan);

    std::vector<FlacClSubframeTask> analyzed_tasks = plan.residual_tasks;
    for (auto& task : analyzed_tasks) {
        analyze_fixed_constant_task_exact(samples, task, max_rice_partition_order);
    }
    auto best_tasks = choose_best_tasks(analyzed_tasks, plan);

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = "scalar-exact",
    };
}

std::optional<FlacClSubframeTask> analyze_mono_lpc_exact_task(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_index,
    const OpenClMonoAnalysisTaskOptions& options,
    unsigned lpc_order,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    validate_options(options);
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL LPC exact analysis coefficient precision must be 1..15");
    }
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL LPC exact analysis max Rice partition order must be 0..8");
    }
    if (frame_index >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / options.frame_samples) {
        throw std::runtime_error("OpenCL LPC exact analysis frame offset exceeds int32");
    }

    const auto offset = frame_index * static_cast<std::size_t>(options.frame_samples);
    if (offset > samples.size() ||
        static_cast<std::size_t>(options.frame_samples) > samples.size() - offset) {
        throw std::runtime_error("OpenCL LPC exact analysis frame samples are out of range");
    }

    const std::vector<std::int32_t> frame_samples(
        samples.begin() + static_cast<std::ptrdiff_t>(offset),
        samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
    const FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 0,
        .bits_per_sample = options.bits_per_sample,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = lpc_coefficient_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
    const auto lpc = analyze_mono_lpc_order(frame_samples, frame_info, lpc_order);
    if (!lpc.has_value()) {
        return std::nullopt;
    }
    if (lpc->order == 0 || lpc->order > kFlacClMaxOrder ||
        lpc->coefficients.size() != lpc->order) {
        throw std::runtime_error("OpenCL LPC exact analysis produced invalid coefficients");
    }

    auto task = make_common_task(options, frame_index, kFlacClSubframeLpc, lpc->order);
    task.data.shift = lpc->quantization_shift;
    task.data.cbits = checked_i32(lpc->coefficient_precision, "OpenCL LPC exact analysis cbits");
    task.data.size = checked_i32(lpc->estimated_bits, "OpenCL LPC exact analysis size");
    task.data.wbits = checked_i32(lpc->wasted_bits, "OpenCL LPC exact analysis wbits");
    task.data.abits = checked_i32(
        frame_amplitude_bits(samples, offset, options.frame_samples, lpc->wasted_bits),
        "OpenCL LPC exact analysis abits");
    task.data.porder = checked_i32(lpc->rice_partition_order, "OpenCL LPC exact analysis porder");

    for (std::size_t i = 0; i < lpc->coefficients.size(); ++i) {
        task.coefs[i] = lpc->coefficients[lpc->coefficients.size() - i - 1U];
    }
    return task;
}

OpenClMonoBestMethodResult run_opencl_mono_best_method(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_best_method_plan(plan);

    const auto selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    ClContext context(clCreateContext(nullptr, 1, &selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    ClCommandQueue queue(clCreateCommandQueue(context.get(), selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    ClProgram program(clCreateProgramWithSource(context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(program.get(), 1, &selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(program.get(), selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    ClKernel kernel(clCreateKernel(program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel");

    ClMem tasks_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.residual_tasks.size() * sizeof(FlacClSubframeTask),
        const_cast<FlacClSubframeTask*>(plan.residual_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(tasks)");

    ClMem selected_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.selected_tasks.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(plan.selected_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(selectedTasks)");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    ClMem best_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_tasks.size() * sizeof(FlacClSubframeTask),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(tasks_out)");

    cl_mem best_mem = best_buffer.get();
    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem selected_mem = selected_buffer.get();
    const auto task_count = static_cast<std::int32_t>(plan.estimate_tasks_per_frame);

    require_cl(clSetKernelArg(kernel.get(), 0, sizeof(best_mem), &best_mem), "clSetKernelArg(tasks_out)");
    require_cl(clSetKernelArg(kernel.get(), 1, sizeof(tasks_mem), &tasks_mem), "clSetKernelArg(tasks)");
    require_cl(clSetKernelArg(kernel.get(), 2, sizeof(selected_mem), &selected_mem), "clSetKernelArg(selectedTasks)");
    require_cl(clSetKernelArg(kernel.get(), 3, sizeof(task_count), &task_count), "clSetKernelArg(taskCount)");

    const std::size_t global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(queue.get(), kernel.get(), 1, nullptr, &global_work_size,
                   nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel");
    require_cl(clEnqueueReadBuffer(queue.get(), best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer");
    require_cl(clFinish(queue.get()), "clFinish");

    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = selected_device.name,
    };
#else
    (void)plan;
    (void)requested_device_index;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_fixed_constant_analysis_inputs(samples, plan);
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL fixed/constant analysis max Rice partition order must be 0..8");
    }

    const auto selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    ClContext context(clCreateContext(nullptr, 1, &selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    ClCommandQueue queue(clCreateCommandQueue(context.get(), selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    ClProgram program(clCreateProgramWithSource(context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(program.get(), 1, &selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(program.get(), selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    ClKernel wasted_kernel(clCreateKernel(program.get(), "ldcompressFindWastedBits", &status));
    require_cl(status, "clCreateKernel(ldcompressFindWastedBits)");
    ClKernel exact_kernel(clCreateKernel(program.get(), "ldcompressAnalyzeSubframeExact", &status));
    require_cl(status, "clCreateKernel(ldcompressAnalyzeSubframeExact)");
    ClKernel choose_kernel(clCreateKernel(program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel(ldcompressChooseBestMethod)");

    ClMem samples_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        samples.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(samples.data()),
        &status));
    require_cl(status, "clCreateBuffer(samples)");

    ClMem tasks_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        plan.residual_tasks.size() * sizeof(FlacClSubframeTask),
        const_cast<FlacClSubframeTask*>(plan.residual_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(tasks)");

    ClMem selected_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.selected_tasks.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(plan.selected_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(selectedTasks)");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> analyzed_tasks(plan.residual_tasks.size());
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    ClMem best_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_tasks.size() * sizeof(FlacClSubframeTask),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(tasks_out)");

    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem samples_mem = samples_buffer.get();
    cl_mem selected_mem = selected_buffer.get();
    cl_mem best_mem = best_buffer.get();
    const auto task_count = static_cast<std::int32_t>(plan.estimate_tasks_per_frame);

    require_cl(clSetKernelArg(wasted_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(wasted.tasks)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(wasted.samples)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 2, sizeof(task_count), &task_count),
        "clSetKernelArg(wasted.tasksPerChannel)");

    const std::size_t one = 1;
    const std::size_t frame_global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(queue.get(), wasted_kernel.get(), 1, nullptr,
                   &frame_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressFindWastedBits)");

    const auto max_rice_partition_order_arg =
        static_cast<std::int32_t>(max_rice_partition_order);
    require_cl(clSetKernelArg(exact_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(exact.samples)");
    require_cl(clSetKernelArg(exact_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(exact.selectedTasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(exact.tasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 3, sizeof(max_rice_partition_order_arg),
                   &max_rice_partition_order_arg),
        "clSetKernelArg(exact.maxRicePartitionOrder)");

    const std::size_t estimate_global_work_size = plan.selected_tasks.size();
    require_cl(clEnqueueNDRangeKernel(queue.get(), exact_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressAnalyzeSubframeExact)");

    require_cl(clSetKernelArg(choose_kernel.get(), 0, sizeof(best_mem), &best_mem),
        "clSetKernelArg(choose.tasks_out)");
    require_cl(clSetKernelArg(choose_kernel.get(), 1, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(choose.tasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 2, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(choose.selectedTasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 3, sizeof(task_count), &task_count),
        "clSetKernelArg(choose.taskCount)");

    require_cl(clEnqueueNDRangeKernel(queue.get(), choose_kernel.get(), 1, nullptr,
                   &frame_global_work_size, nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressChooseBestMethod)");

    require_cl(clEnqueueReadBuffer(queue.get(), tasks_buffer.get(), CL_TRUE, 0,
                   analyzed_tasks.size() * sizeof(FlacClSubframeTask), analyzed_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks)");
    require_cl(clEnqueueReadBuffer(queue.get(), best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks_out)");
    require_cl(clFinish(queue.get()), "clFinish");

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = selected_device.name,
    };
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_lpc_analysis_inputs(samples, plan);
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL LPC analysis max Rice partition order must be 0..8");
    }

    const auto selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    ClContext context(clCreateContext(nullptr, 1, &selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    ClCommandQueue queue(clCreateCommandQueue(context.get(), selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    ClProgram program(clCreateProgramWithSource(context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(program.get(), 1, &selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(program.get(), selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    ClKernel wasted_kernel(clCreateKernel(program.get(), "ldcompressFindWastedBits", &status));
    require_cl(status, "clCreateKernel(ldcompressFindWastedBits)");
    ClKernel exact_kernel(clCreateKernel(program.get(), "ldcompressAnalyzeSubframeExact", &status));
    require_cl(status, "clCreateKernel(ldcompressAnalyzeSubframeExact)");
    ClKernel choose_kernel(clCreateKernel(program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel(ldcompressChooseBestMethod)");

    ClMem samples_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        samples.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(samples.data()),
        &status));
    require_cl(status, "clCreateBuffer(samples)");

    ClMem tasks_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        plan.residual_tasks.size() * sizeof(FlacClSubframeTask),
        const_cast<FlacClSubframeTask*>(plan.residual_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(tasks)");

    ClMem selected_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.selected_tasks.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(plan.selected_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(selectedTasks)");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> analyzed_tasks(plan.residual_tasks.size());
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    ClMem best_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_tasks.size() * sizeof(FlacClSubframeTask),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(tasks_out)");

    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem samples_mem = samples_buffer.get();
    cl_mem selected_mem = selected_buffer.get();
    cl_mem best_mem = best_buffer.get();
    const auto task_count = static_cast<std::int32_t>(plan.estimate_tasks_per_frame);

    require_cl(clSetKernelArg(wasted_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(wasted.tasks)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(wasted.samples)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 2, sizeof(task_count), &task_count),
        "clSetKernelArg(wasted.tasksPerChannel)");

    const std::size_t one = 1;
    const std::size_t frame_global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(queue.get(), wasted_kernel.get(), 1, nullptr,
                   &frame_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressFindWastedBits)");

    const auto max_rice_partition_order_arg =
        static_cast<std::int32_t>(max_rice_partition_order);
    require_cl(clSetKernelArg(exact_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(exact.samples)");
    require_cl(clSetKernelArg(exact_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(exact.selectedTasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(exact.tasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 3, sizeof(max_rice_partition_order_arg),
                   &max_rice_partition_order_arg),
        "clSetKernelArg(exact.maxRicePartitionOrder)");

    const std::size_t estimate_global_work_size = plan.selected_tasks.size();
    require_cl(clEnqueueNDRangeKernel(queue.get(), exact_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressAnalyzeSubframeExact)");

    require_cl(clSetKernelArg(choose_kernel.get(), 0, sizeof(best_mem), &best_mem),
        "clSetKernelArg(choose.tasks_out)");
    require_cl(clSetKernelArg(choose_kernel.get(), 1, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(choose.tasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 2, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(choose.selectedTasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 3, sizeof(task_count), &task_count),
        "clSetKernelArg(choose.taskCount)");

    require_cl(clEnqueueNDRangeKernel(queue.get(), choose_kernel.get(), 1, nullptr,
                   &frame_global_work_size, nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressChooseBestMethod)");

    require_cl(clEnqueueReadBuffer(queue.get(), tasks_buffer.get(), CL_TRUE, 0,
                   analyzed_tasks.size() * sizeof(FlacClSubframeTask), analyzed_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks)");
    require_cl(clEnqueueReadBuffer(queue.get(), best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks_out)");
    require_cl(clFinish(queue.get()), "clFinish");

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = selected_device.name,
    };
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

#if LDCOMPRESS_HAVE_OPENCL
OpenClMonoFixedConstantAnalysisResult run_opencl_mono_generated_analysis_validated(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    std::size_t lpc_tasks_per_window,
    std::size_t total_lpc_tasks,
    std::size_t generated_window_count,
    OpenClGeneratedAnalysisRuntime& runtime,
    bool read_analyzed_tasks)
{
    if (lpc_tasks_per_window == 0 || lpc_tasks_per_window > kFlacClMaxOrder ||
        generated_window_count == 0 ||
        total_lpc_tasks == 0 ||
        total_lpc_tasks > plan.residual_tasks_per_frame ||
        total_lpc_tasks > lpc_tasks_per_window * generated_window_count) {
        throw std::runtime_error("OpenCL generated analysis LPC task count is invalid");
    }
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL generated analysis max Rice partition order must be 0..8");
    }

    const auto samples_bytes = samples.size() * sizeof(std::int32_t);
    ensure_opencl_buffer(runtime.context, runtime.samples_buffer,
        runtime.samples_buffer_bytes, CL_MEM_READ_ONLY, samples_bytes, "samples");
    write_opencl_buffer(
        runtime.queue, runtime.samples_buffer, samples.data(), samples_bytes, "samples");

    const auto tasks_bytes = plan.residual_tasks.size() * sizeof(FlacClSubframeTask);
    ensure_opencl_buffer(runtime.context, runtime.tasks_buffer,
        runtime.tasks_buffer_bytes, CL_MEM_READ_WRITE, tasks_bytes, "tasks");
    write_opencl_buffer(
        runtime.queue, runtime.tasks_buffer, plan.residual_tasks.data(), tasks_bytes, "tasks");

    const auto selected_bytes = plan.selected_tasks.size() * sizeof(std::int32_t);
    ensure_opencl_buffer(runtime.context, runtime.selected_buffer,
        runtime.selected_buffer_bytes, CL_MEM_READ_ONLY, selected_bytes, "selectedTasks");
    write_opencl_buffer(runtime.queue, runtime.selected_buffer,
        plan.selected_tasks.data(), selected_bytes, "selectedTasks");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    const auto blocksize = static_cast<std::size_t>(plan.residual_tasks.front().data.blocksize);
    std::vector<float> window =
        make_generated_lpc_windows(blocksize, generated_window_count);
    const auto window_bytes = window.size() * sizeof(float);
    ensure_opencl_buffer(runtime.context, runtime.window_buffer,
        runtime.window_buffer_bytes, CL_MEM_READ_ONLY, window_bytes, "window");
    write_opencl_buffer(
        runtime.queue, runtime.window_buffer, window.data(), window_bytes, "window");

    const auto autocor_count = frame_count * generated_window_count * (kFlacClMaxOrder + 1U);
    const auto autocor_bytes = autocor_count * sizeof(float);
    ensure_opencl_buffer(runtime.context, runtime.autocor_buffer,
        runtime.autocor_buffer_bytes, CL_MEM_READ_WRITE, autocor_bytes, "autocor");

    const auto lpc_count =
        frame_count * generated_window_count * (kFlacClMaxOrder + 1U) * kFlacClMaxOrder;
    const auto lpc_bytes = lpc_count * sizeof(float);
    ensure_opencl_buffer(runtime.context, runtime.lpc_buffer,
        runtime.lpc_buffer_bytes, CL_MEM_READ_WRITE, lpc_bytes, "lpc");

    std::vector<FlacClSubframeTask> analyzed_tasks;
    if (read_analyzed_tasks) {
        analyzed_tasks.resize(plan.residual_tasks.size());
    }
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    const auto best_bytes = best_tasks.size() * sizeof(FlacClSubframeTask);
    ensure_opencl_buffer(runtime.context, runtime.best_buffer,
        runtime.best_buffer_bytes, CL_MEM_WRITE_ONLY, best_bytes, "tasks_out");

    cl_mem tasks_mem = runtime.tasks_buffer.get();
    cl_mem samples_mem = runtime.samples_buffer.get();
    cl_mem selected_mem = runtime.selected_buffer.get();
    cl_mem window_mem = runtime.window_buffer.get();
    cl_mem autocor_mem = runtime.autocor_buffer.get();
    cl_mem lpc_mem = runtime.lpc_buffer.get();
    cl_mem best_mem = runtime.best_buffer.get();
    const auto residual_tasks_per_frame =
        static_cast<std::int32_t>(plan.residual_tasks_per_frame);
    const auto estimate_tasks_per_frame =
        static_cast<std::int32_t>(plan.estimate_tasks_per_frame);

    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(wasted.tasks)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(wasted.samples)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 2, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(wasted.tasksPerChannel)");

    const std::size_t one = 1;
    const std::size_t frame_global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.wasted_kernel.get(), 1, nullptr,
                   &frame_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressFindWastedBits)");

    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 0, sizeof(autocor_mem), &autocor_mem),
        "clSetKernelArg(autocor.output)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(autocor.samples)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 2, sizeof(window_mem), &window_mem),
        "clSetKernelArg(autocor.window)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 3, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(autocor.tasks)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 4, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(autocor.tasksPerFrame)");

    const std::array<std::size_t, 2> generation_global {
        frame_count,
        generated_window_count,
    };
    const std::array<std::size_t, 2> generation_local { 1, 1 };
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.autocor_kernel.get(), 2, nullptr,
                   generation_global.data(), generation_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressComputeAutocor)");

    const auto window_count_arg = static_cast<std::int32_t>(generated_window_count);
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 0, sizeof(autocor_mem), &autocor_mem),
        "clSetKernelArg(lpc.autocor)");
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 1, sizeof(lpc_mem), &lpc_mem),
        "clSetKernelArg(lpc.lpcs)");
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 2, sizeof(window_count_arg), &window_count_arg),
        "clSetKernelArg(lpc.windowCount)");
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.lpc_kernel.get(), 2, nullptr,
                   generation_global.data(), generation_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressComputeLpc)");

    const auto lpc_tasks_per_window_arg =
        static_cast<std::int32_t>(lpc_tasks_per_window);
    const auto total_lpc_tasks_arg =
        static_cast<std::int32_t>(total_lpc_tasks);
    const auto lpc_coefficient_precision_arg =
        static_cast<std::int32_t>(lpc_coefficient_precision);
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(quantize.tasks)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 1, sizeof(lpc_mem), &lpc_mem),
        "clSetKernelArg(quantize.lpcs)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 2, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(quantize.tasksPerFrame)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 3, sizeof(lpc_tasks_per_window_arg),
                   &lpc_tasks_per_window_arg),
        "clSetKernelArg(quantize.lpcTasksPerWindow)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 4, sizeof(total_lpc_tasks_arg),
                   &total_lpc_tasks_arg),
        "clSetKernelArg(quantize.totalLpcTasks)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 5, sizeof(lpc_coefficient_precision_arg),
                   &lpc_coefficient_precision_arg),
        "clSetKernelArg(quantize.coefficientPrecision)");
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.quantize_kernel.get(), 2, nullptr,
                   generation_global.data(), generation_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressQuantizeLpcOrders)");

    const auto max_rice_partition_order_arg =
        static_cast<std::int32_t>(max_rice_partition_order);
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(exact.samples)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(exact.selectedTasks)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(exact.tasks)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 3, sizeof(max_rice_partition_order_arg),
                   &max_rice_partition_order_arg),
        "clSetKernelArg(exact.maxRicePartitionOrder)");

    const std::size_t estimate_global_work_size = plan.selected_tasks.size();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.exact_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressAnalyzeSubframeExact)");

    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 0, sizeof(best_mem), &best_mem),
        "clSetKernelArg(choose.tasks_out)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 1, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(choose.tasks)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 2, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(choose.selectedTasks)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 3, sizeof(estimate_tasks_per_frame),
                   &estimate_tasks_per_frame),
        "clSetKernelArg(choose.taskCount)");

    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.choose_kernel.get(), 1, nullptr,
                   &frame_global_work_size, nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressChooseBestMethod)");

    if (read_analyzed_tasks) {
        require_cl(clEnqueueReadBuffer(runtime.queue.get(), runtime.tasks_buffer.get(), CL_TRUE, 0,
                       analyzed_tasks.size() * sizeof(FlacClSubframeTask), analyzed_tasks.data(),
                       0, nullptr, nullptr),
            "clEnqueueReadBuffer(tasks)");
    }
    require_cl(clEnqueueReadBuffer(runtime.queue.get(), runtime.best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks_out)");
    require_cl(clFinish(runtime.queue.get()), "clFinish");

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = runtime.selected_device.name,
    };
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_generated_analysis_impl(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    std::size_t lpc_tasks_per_window,
    std::size_t total_lpc_tasks,
    std::size_t generated_window_count)
{
    auto runtime = make_opencl_generated_analysis_runtime(requested_device_index);
    return run_opencl_mono_generated_analysis_validated(
        samples,
        plan,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_tasks_per_window,
        total_lpc_tasks,
        generated_window_count,
        runtime,
        true);
}
#endif

class OpenClMonoAnalysisSession::Impl final {
public:
#if LDCOMPRESS_HAVE_OPENCL
    explicit Impl(std::optional<std::size_t> requested_device_index)
        : runtime_(make_opencl_generated_analysis_runtime(requested_device_index))
    {
    }

    OpenClMonoFixedConstantAnalysisResult run_generated_analysis(
        const std::vector<std::int32_t>& samples,
        const OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision,
        unsigned max_rice_partition_order,
        const GeneratedLpcPrefixShape& lpc_shape,
        bool read_analyzed_tasks)
    {
        return run_opencl_mono_generated_analysis_validated(
            samples,
            plan,
            lpc_coefficient_precision,
            max_rice_partition_order,
            lpc_shape.lpc_tasks_per_window,
            lpc_shape.total_lpc_tasks,
            lpc_shape.window_count,
            runtime_,
            read_analyzed_tasks);
    }

private:
    OpenClGeneratedAnalysisRuntime runtime_;
#endif
};

OpenClMonoAnalysisSession::OpenClMonoAnalysisSession(
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_OPENCL
    impl_ = std::make_unique<Impl>(requested_device_index);
#else
    (void)requested_device_index;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoAnalysisSession::~OpenClMonoAnalysisSession() = default;

OpenClMonoFixedConstantAnalysisResult OpenClMonoAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    return impl_->run_generated_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order, lpc_shape, true);
#else
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoBestMethodResult OpenClMonoAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    auto analysis = impl_->run_generated_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order, lpc_shape, false);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
#else
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_lpc_generation_inputs(samples, plan, lpc_coefficient_precision);
    return run_opencl_mono_generated_analysis_impl(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order,
        plan.residual_tasks_per_frame,
        plan.residual_tasks_per_frame,
        1);
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    return run_opencl_mono_generated_analysis_impl(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_shape.lpc_tasks_per_window,
        lpc_shape.total_lpc_tasks,
        lpc_shape.window_count);
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoBestMethodResult run_opencl_mono_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    OpenClMonoAnalysisSession session(requested_device_index);
    return session.run_generated_best_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
}

OpenClMonoGeneratedFrameAnalysisResult analyze_opencl_mono_generated_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> requested_device_index)
{
    if (frame_samples == 0) {
        throw std::runtime_error("OpenCL generated frame analysis frame_samples must be positive");
    }
    if (samples.empty()) {
        throw std::runtime_error("OpenCL generated frame analysis requires at least one frame");
    }
    if ((samples.size() % frame_samples) != 0) {
        throw std::runtime_error("OpenCL generated frame analysis samples are not frame-aligned");
    }

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = frame_samples;
    options.bits_per_sample = frame_info.bits_per_sample;
    options.max_lpc_order = frame_info.max_lpc_order;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;
    options.include_constant = true;

    const auto frame_count = samples.size() / frame_samples;
    const auto plan = build_mono_analysis_task_plan(frame_count, options);
    auto analysis = run_opencl_mono_generated_analysis(
        samples,
        plan,
        requested_device_index,
        frame_info.lpc_coefficient_precision,
        frame_info.max_rice_partition_order);

    std::vector<FlacSubframeDecision> decisions;
    std::vector<FlacSelectedSubframe> selected_subframes;
    decisions.reserve(analysis.best_tasks.size());
    selected_subframes.reserve(analysis.best_tasks.size());
    for (const auto& task : analysis.best_tasks) {
        decisions.push_back(flaccl_task_to_subframe_decision(task));
        selected_subframes.push_back(flaccl_task_to_selected_subframe(task));
    }

    return OpenClMonoGeneratedFrameAnalysisResult {
        .analyzed_tasks = std::move(analysis.analyzed_tasks),
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .decisions = std::move(decisions),
        .selected_subframes = std::move(selected_subframes),
        .device_name = std::move(analysis.device_name),
    };
}

}  // namespace ldcompress::opencl_detail
