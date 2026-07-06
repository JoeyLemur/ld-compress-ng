#include "vulkan_backend.h"

#include "accelerated_native_backend.h"
#include "opencl_analysis.h"
#include "vulkan_analysis.h"
#include "vulkan_devices.h"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kVulkanBatchFrames = 32;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxVulkanFrameSamples = 4608;
constexpr unsigned kMaxVulkanLpcOrder = 12;
constexpr unsigned kMinVulkanLpcPrecision = 1;
constexpr unsigned kMaxVulkanLpcPrecision = 15;
constexpr unsigned kMaxVulkanRicePartitionOrder = 8;

opencl_detail::OpenClMonoAnalysisTaskPlan build_vulkan_fixed_constant_task_plan(
    std::size_t frame_count,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples)
{
    opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = frame_samples;
    task_options.bits_per_sample = frame_info.bits_per_sample;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    return opencl_detail::build_mono_analysis_task_plan(frame_count, task_options);
}

opencl_detail::FlacClSubframeTask make_invalid_placeholder_task(
    const opencl_detail::FlacClSubframeTask& frame_template)
{
    auto task = frame_template;
    task.data.type = opencl_detail::kFlacClSubframeVerbatim;
    task.data.residualOrder = 0;
    task.data.shift = 0;
    task.data.cbits = 0;
    task.data.size = std::numeric_limits<std::int32_t>::max();
    task.data.porder = 0;
    task.coefs.fill(0);
    return task;
}

opencl_detail::OpenClMonoAnalysisTaskPlan build_vulkan_mixed_lpc_task_plan(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples)
{
    const auto frame_count = samples.size() / frame_samples;
    auto fixed_plan = build_vulkan_fixed_constant_task_plan(
        frame_count, frame_info, frame_samples);
    if (frame_info.max_lpc_order == 0 || frame_samples < 256) {
        return fixed_plan;
    }

    opencl_detail::OpenClMonoAnalysisTaskOptions lpc_options;
    lpc_options.frame_samples = frame_samples;
    lpc_options.bits_per_sample = frame_info.bits_per_sample;
    lpc_options.max_lpc_order = frame_info.max_lpc_order;
    lpc_options.include_constant = false;
    lpc_options.min_fixed_order = 0;
    lpc_options.max_fixed_order = 0;

    opencl_detail::OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks_per_frame =
        fixed_plan.residual_tasks_per_frame + frame_info.max_lpc_order;
    plan.estimate_tasks_per_frame = plan.residual_tasks_per_frame;
    plan.residual_tasks.reserve(plan.residual_tasks_per_frame * frame_count);
    plan.selected_tasks.reserve(plan.estimate_tasks_per_frame * frame_count);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto output_base = plan.residual_tasks.size();
        const auto fixed_base = frame * fixed_plan.residual_tasks_per_frame;
        for (std::size_t i = 0; i < fixed_plan.residual_tasks_per_frame; ++i) {
            plan.residual_tasks.push_back(fixed_plan.residual_tasks.at(fixed_base + i));
        }

        const auto placeholder_template = fixed_plan.residual_tasks.at(fixed_base);
        for (unsigned order = 1; order <= frame_info.max_lpc_order; ++order) {
            auto task = opencl_detail::analyze_mono_lpc_exact_task(
                samples,
                frame,
                lpc_options,
                order,
                frame_info.lpc_coefficient_precision,
                frame_info.max_rice_partition_order);
            if (task.has_value()) {
                task->data.size = static_cast<std::int32_t>(
                    frame_info.bits_per_sample * frame_samples);
                task->data.abits = 0;
                task->data.porder = 0;
                plan.residual_tasks.push_back(*task);
            } else {
                plan.residual_tasks.push_back(make_invalid_placeholder_task(
                    placeholder_template));
            }
        }

        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            plan.selected_tasks.push_back(static_cast<std::int32_t>(output_base + i));
        }
    }

    return plan;
}

AcceleratedSelectedFrameAnalysis analyze_vulkan_selected_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> device_index)
{
    const auto frame_count = samples.size() / frame_samples;
    const auto plan = frame_info.max_lpc_order == 0
        ? build_vulkan_fixed_constant_task_plan(frame_count, frame_info, frame_samples)
        : build_vulkan_mixed_lpc_task_plan(samples, frame_info, frame_samples);
    auto result = frame_info.max_lpc_order == 0
        ? vulkan_detail::run_vulkan_mono_fixed_constant_analysis(
            samples, plan, device_index, frame_info.max_rice_partition_order)
        : vulkan_detail::run_vulkan_mono_lpc_analysis(
            samples, plan, device_index, frame_info.max_rice_partition_order);

    AcceleratedSelectedFrameAnalysis selected;
    selected.decisions.reserve(result.best_tasks.size());
    selected.selected_subframes.reserve(result.best_tasks.size());
    for (const auto& task : result.best_tasks) {
        selected.decisions.push_back(opencl_detail::flaccl_task_to_subframe_decision(task));
        selected.selected_subframes.push_back(opencl_detail::flaccl_task_to_selected_subframe(task));
    }
    return selected;
}

void validate_vulkan_options(const VulkanCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("vulkan backend writes native FLAC only");
    }
    if (options.thread_count != 1) {
        throw std::runtime_error("Vulkan FLAC backend currently requires --threads 1");
    }
    if (options.frame_samples < kMinimumStreamInfoBlockSize ||
        options.frame_samples > kMaxVulkanFrameSamples) {
        throw std::runtime_error("Vulkan FLAC frame sample count must be 16..4608");
    }
    if (options.max_lpc_order > kMaxVulkanLpcOrder) {
        throw std::runtime_error("Vulkan FLAC max LPC order must be 0..12");
    }
    if (options.lpc_precision < kMinVulkanLpcPrecision ||
        options.lpc_precision > kMaxVulkanLpcPrecision) {
        throw std::runtime_error("Vulkan FLAC LPC coefficient precision must be 1..15");
    }
    if (options.max_rice_partition_order > kMaxVulkanRicePartitionOrder) {
        throw std::runtime_error("Vulkan FLAC max Rice partition order must be 0..8");
    }
}

VulkanDeviceInfo select_vulkan_analysis_device(std::optional<std::size_t> requested_index)
{
    if (requested_index.has_value()) {
        auto device = select_vulkan_device(requested_index);
        if (!device.shader_int64) {
            throw std::runtime_error("selected Vulkan device does not support shaderInt64: " +
                device.device_name);
        }
        return device;
    }

    const auto devices = list_vulkan_devices();
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }
    for (const auto& device : devices) {
        if (device.available && device.shader_int64) {
            return device;
        }
    }
    throw std::runtime_error("no available Vulkan compute devices with shaderInt64 found");
}

}  // namespace

ConversionStats compress_lds_to_vulkan_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const VulkanCompressionOptions& options)
{
    validate_vulkan_options(options);
    const auto selected_device = select_vulkan_analysis_device(options.device_index);
    const auto device_index = std::optional<std::size_t>(selected_device.index);

    const AcceleratedNativeCompressionOptions accelerated_options {
        .backend_label = "Vulkan",
        .container = options.container,
        .sample_rate = options.sample_rate,
        .thread_count = options.thread_count,
        .frame_samples = options.frame_samples,
        .max_lpc_order = options.max_lpc_order,
        .lpc_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
        .batch_frames = kVulkanBatchFrames,
        .native_stats = options.native_stats,
    };

    return compress_lds_to_accelerated_native_flac(
        lds_input,
        output_path,
        accelerated_options,
        [device_index](
            const std::vector<std::int32_t>& samples,
            const FlacFrameInfo& frame_info,
            unsigned frame_samples) {
            return analyze_vulkan_selected_frames(
                samples, frame_info, frame_samples, device_index);
        });
}

}  // namespace ldcompress
