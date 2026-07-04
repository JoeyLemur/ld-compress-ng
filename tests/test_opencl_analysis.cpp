#include "opencl_analysis.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const char* message)
{
    try {
        fn();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

void require_common_task_fields(
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    int expected_type,
    int expected_order,
    int expected_offset)
{
    require(task.data.residualOrder == expected_order, "unexpected residualOrder");
    require(task.data.samplesOffs == expected_offset, "unexpected samplesOffs");
    require(task.data.shift == 0, "unexpected shift");
    require(task.data.cbits == 0, "unexpected cbits");
    require(task.data.size == 16 * 4608, "unexpected raw task size");
    require(task.data.type == expected_type, "unexpected subframe type");
    require(task.data.obits == 16, "unexpected obits");
    require(task.data.blocksize == 4608, "unexpected blocksize");
    require(task.data.coding_method == 0, "unexpected coding method");
    require(task.data.channel == 0, "unexpected channel");
    require(task.data.residualOffs == expected_offset, "unexpected residualOffs");
    require(task.data.wbits == 0, "unexpected wbits");
    require(task.data.abits == 16, "unexpected abits");
    require(task.data.porder == 0, "unexpected initial porder");
    require(task.data.headerLen == 0, "unexpected headerLen");
    require(task.data.encodingOffset == 0, "unexpected encodingOffset");
}

void test_flaccl_abi_layout()
{
    using ldcompress::opencl_detail::FlacClSubframeData;
    using ldcompress::opencl_detail::FlacClSubframeTask;
    using namespace ldcompress::opencl_detail;

    require(kFlacClSubframeConstant == 0, "FLACCL constant type mismatch");
    require(kFlacClSubframeVerbatim == 1, "FLACCL verbatim type mismatch");
    require(kFlacClSubframeFixed == 8, "FLACCL fixed type mismatch");
    require(kFlacClSubframeLpc == 32, "FLACCL LPC type mismatch");
    require(kFlacClMaxOrder == 32, "FLACCL max order mismatch");

    require(std::is_standard_layout_v<FlacClSubframeData>, "FLACCL data is not standard layout");
    require(std::is_standard_layout_v<FlacClSubframeTask>, "FLACCL task is not standard layout");
    require(sizeof(FlacClSubframeData) == 64, "FLACCL data size mismatch");
    require(sizeof(FlacClSubframeTask) == 192, "FLACCL task size mismatch");

    require(offsetof(FlacClSubframeData, residualOrder) == 0, "residualOrder offset mismatch");
    require(offsetof(FlacClSubframeData, samplesOffs) == 4, "samplesOffs offset mismatch");
    require(offsetof(FlacClSubframeData, shift) == 8, "shift offset mismatch");
    require(offsetof(FlacClSubframeData, cbits) == 12, "cbits offset mismatch");
    require(offsetof(FlacClSubframeData, size) == 16, "size offset mismatch");
    require(offsetof(FlacClSubframeData, type) == 20, "type offset mismatch");
    require(offsetof(FlacClSubframeData, obits) == 24, "obits offset mismatch");
    require(offsetof(FlacClSubframeData, blocksize) == 28, "blocksize offset mismatch");
    require(offsetof(FlacClSubframeData, coding_method) == 32, "coding_method offset mismatch");
    require(offsetof(FlacClSubframeData, channel) == 36, "channel offset mismatch");
    require(offsetof(FlacClSubframeData, residualOffs) == 40, "residualOffs offset mismatch");
    require(offsetof(FlacClSubframeData, wbits) == 44, "wbits offset mismatch");
    require(offsetof(FlacClSubframeData, abits) == 48, "abits offset mismatch");
    require(offsetof(FlacClSubframeData, porder) == 52, "porder offset mismatch");
    require(offsetof(FlacClSubframeData, headerLen) == 56, "headerLen offset mismatch");
    require(offsetof(FlacClSubframeData, encodingOffset) == 60, "encodingOffset offset mismatch");
    require(offsetof(FlacClSubframeTask, coefs) == 64, "coefs offset mismatch");
}

void test_default_mono_task_plan()
{
    using namespace ldcompress::opencl_detail;

    const OpenClMonoAnalysisTaskOptions options;
    require(mono_analysis_tasks_per_frame(options) == 18, "unexpected default task count");

    const auto plan = build_mono_analysis_task_plan(2, options);
    require(plan.residual_tasks_per_frame == 18, "unexpected residual tasks per frame");
    require(plan.estimate_tasks_per_frame == 18, "unexpected estimate tasks per frame");
    require(plan.residual_tasks.size() == 36, "unexpected residual task vector size");
    require(plan.selected_tasks.size() == 36, "unexpected selected task vector size");

    for (std::size_t i = 0; i < plan.selected_tasks.size(); ++i) {
        require(plan.selected_tasks[i] == static_cast<int>(i), "selected task index mismatch");
    }

    for (int order = 1; order <= 12; ++order) {
        require_common_task_fields(plan.residual_tasks[static_cast<std::size_t>(order - 1)],
            kFlacClSubframeLpc,
            order,
            0);
    }

    const auto& constant = plan.residual_tasks[12];
    require_common_task_fields(constant, kFlacClSubframeConstant, 1, 0);
    require(constant.coefs[0] == 1, "constant coefficient mismatch");

    const auto fixed_base = static_cast<std::size_t>(13);
    for (int order = 0; order <= 4; ++order) {
        require_common_task_fields(plan.residual_tasks[fixed_base + static_cast<std::size_t>(order)],
            kFlacClSubframeFixed,
            order,
            0);
    }
    require(plan.residual_tasks[fixed_base + 1].coefs[0] == 1, "fixed order 1 coefficient mismatch");
    require(plan.residual_tasks[fixed_base + 2].coefs[0] == -1, "fixed order 2 coefficient 0 mismatch");
    require(plan.residual_tasks[fixed_base + 2].coefs[1] == 2, "fixed order 2 coefficient 1 mismatch");
    require(plan.residual_tasks[fixed_base + 4].coefs[0] == -1, "fixed order 4 coefficient 0 mismatch");
    require(plan.residual_tasks[fixed_base + 4].coefs[1] == 4, "fixed order 4 coefficient 1 mismatch");
    require(plan.residual_tasks[fixed_base + 4].coefs[2] == -6, "fixed order 4 coefficient 2 mismatch");
    require(plan.residual_tasks[fixed_base + 4].coefs[3] == 4, "fixed order 4 coefficient 3 mismatch");

    require_common_task_fields(plan.residual_tasks[18], kFlacClSubframeLpc, 1, 4608);
    require_common_task_fields(plan.residual_tasks[35], kFlacClSubframeFixed, 4, 4608);
}

void test_small_custom_task_plan()
{
    using namespace ldcompress::opencl_detail;

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 3;
    options.bits_per_sample = 20;
    options.max_lpc_order = 0;
    options.include_constant = false;
    options.min_fixed_order = 1;
    options.max_fixed_order = 4;

    require(mono_analysis_tasks_per_frame(options) == 2, "unexpected capped fixed task count");
    const auto plan = build_mono_analysis_task_plan(1, options);
    require(plan.residual_tasks.size() == 2, "unexpected small task plan size");
    require(plan.residual_tasks[0].data.type == kFlacClSubframeFixed, "unexpected first small type");
    require(plan.residual_tasks[0].data.residualOrder == 1, "unexpected first small order");
    require(plan.residual_tasks[0].data.size == 60, "unexpected small raw size");
    require(plan.residual_tasks[0].data.coding_method == 1, "unexpected high-bit-depth coding method");
    require(plan.residual_tasks[1].data.residualOrder == 2, "unexpected capped fixed order");
}

void test_invalid_options()
{
    using namespace ldcompress::opencl_detail;

    require_throws([] {
        OpenClMonoAnalysisTaskOptions options;
        options.frame_samples = 0;
        (void)build_mono_analysis_task_plan(1, options);
    }, "zero frame size accepted");

    require_throws([] {
        OpenClMonoAnalysisTaskOptions options;
        options.frame_samples = 8;
        options.max_lpc_order = 8;
        (void)build_mono_analysis_task_plan(1, options);
    }, "LPC order equal to frame size accepted");

    require_throws([] {
        OpenClMonoAnalysisTaskOptions options;
        options.max_lpc_order = 28;
        (void)build_mono_analysis_task_plan(1, options);
    }, "too many residual tasks accepted");

    require_throws([] {
        OpenClMonoAnalysisTaskOptions options;
        options.max_fixed_order = 5;
        (void)build_mono_analysis_task_plan(1, options);
    }, "invalid fixed order accepted");
}

}  // namespace

int main()
{
    try {
        test_flaccl_abi_layout();
        test_default_mono_task_plan();
        test_small_custom_task_plan();
        test_invalid_options();
    } catch (const std::exception& ex) {
        std::cerr << "test_opencl_analysis: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
