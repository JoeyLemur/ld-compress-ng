#include "opencl_analysis.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace ldcompress::opencl_detail {
namespace {

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

}  // namespace

std::size_t mono_analysis_tasks_per_frame(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.frame_samples == 0 || options.min_fixed_order > options.max_fixed_order ||
        options.min_fixed_order > 4 || options.max_fixed_order > 4) {
        return 0;
    }

    return static_cast<std::size_t>(options.max_lpc_order) +
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
        for (unsigned order = 1; order <= options.max_lpc_order; ++order) {
            plan.residual_tasks.push_back(make_common_task(options, frame, kFlacClSubframeLpc, order));
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

}  // namespace ldcompress::opencl_detail
