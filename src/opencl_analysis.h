#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace ldcompress::opencl_detail {

constexpr std::int32_t kFlacClSubframeConstant = 0;
constexpr std::int32_t kFlacClSubframeVerbatim = 1;
constexpr std::int32_t kFlacClSubframeFixed = 8;
constexpr std::int32_t kFlacClSubframeLpc = 32;
constexpr std::size_t kFlacClMaxOrder = 32;

// ABI mirror of FlaLDF/CUETools.Codecs.FLACCL/flac.cl. Field names intentionally
// match FLACCL so host/device buffer dumps can be compared directly.
struct FlacClSubframeData {
    std::int32_t residualOrder = 0;
    std::int32_t samplesOffs = 0;
    std::int32_t shift = 0;
    std::int32_t cbits = 0;
    std::int32_t size = 0;
    std::int32_t type = 0;
    std::int32_t obits = 0;
    std::int32_t blocksize = 0;
    std::int32_t coding_method = 0;
    std::int32_t channel = 0;
    std::int32_t residualOffs = 0;
    std::int32_t wbits = 0;
    std::int32_t abits = 0;
    std::int32_t porder = 0;
    std::int32_t headerLen = 0;
    std::int32_t encodingOffset = 0;
};

struct FlacClSubframeTask {
    FlacClSubframeData data;
    std::array<std::int32_t, kFlacClMaxOrder> coefs {};
};

static_assert(std::is_standard_layout_v<FlacClSubframeData>);
static_assert(std::is_standard_layout_v<FlacClSubframeTask>);
static_assert(sizeof(FlacClSubframeData) == 64);
static_assert(sizeof(FlacClSubframeTask) == 192);

struct OpenClMonoAnalysisTaskOptions {
    unsigned frame_samples = 4608;
    unsigned bits_per_sample = 16;
    unsigned max_lpc_order = 12;
    unsigned min_fixed_order = 0;
    unsigned max_fixed_order = 4;
    bool include_constant = true;
};

struct OpenClMonoAnalysisTaskPlan {
    std::vector<FlacClSubframeTask> residual_tasks;
    std::vector<std::int32_t> selected_tasks;
    std::size_t residual_tasks_per_frame = 0;
    std::size_t estimate_tasks_per_frame = 0;
};

struct OpenClMonoBestMethodResult {
    std::vector<FlacClSubframeTask> best_tasks;
    std::string device_name;
};

std::size_t mono_analysis_tasks_per_frame(const OpenClMonoAnalysisTaskOptions& options);

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan(
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options);

OpenClMonoBestMethodResult run_opencl_mono_best_method(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt);

}  // namespace ldcompress::opencl_detail
