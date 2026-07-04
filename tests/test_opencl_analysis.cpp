#include "flac_native_writer.h"
#include "opencl_analysis.h"
#include "opencl_devices.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

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

template <typename Fn>
void require_throws_containing(Fn&& fn, const char* expected, const char* message)
{
    try {
        fn();
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find(expected) != std::string::npos) {
            return;
        }
        throw std::runtime_error(message);
    }
    throw std::runtime_error(message);
}

std::vector<std::int32_t> make_lpc_friendly_samples()
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<std::int32_t> samples;
    samples.reserve(512);
    for (int i = 0; i < 512; ++i) {
        const double sample =
            (std::sin((2.0 * kPi * i) / 31.0) * 11000.0) +
            (std::sin((2.0 * kPi * i) / 11.0) * 3500.0);
        auto quantized = static_cast<int>(std::lround(sample / 64.0)) * 64;
        quantized = std::clamp(quantized, -32768, 32704);
        samples.push_back(quantized);
    }
    return samples;
}

std::optional<std::size_t> first_available_opencl_device_index()
{
    if (!ldcompress::opencl_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_opencl_devices()) {
        if (device.available) {
            return device.flat_index;
        }
    }

    return std::nullopt;
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

ldcompress::FlacFrameInfo make_fixed_only_frame_info(unsigned max_rice_partition_order)
{
    return ldcompress::FlacFrameInfo {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = 0,
        .lpc_coefficient_precision = 12,
        .max_rice_partition_order = max_rice_partition_order,
    };
}

void require_task_matches_decision(
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    const ldcompress::FlacSubframeDecision& decision,
    const char* label)
{
    using namespace ldcompress::opencl_detail;

    switch (decision.kind) {
    case ldcompress::FlacSubframeKind::Constant:
        require(task.data.type == kFlacClSubframeConstant, label);
        break;
    case ldcompress::FlacSubframeKind::FixedRice:
        require(task.data.type == kFlacClSubframeFixed, label);
        require(task.data.residualOrder == static_cast<int>(decision.fixed_order), label);
        require(task.data.porder == static_cast<int>(decision.rice_partition_order), label);
        break;
    case ldcompress::FlacSubframeKind::Verbatim:
    case ldcompress::FlacSubframeKind::LpcRice:
        throw std::runtime_error("test expected a constant or fixed/Rice scalar decision");
    }

    require(task.data.wbits == static_cast<int>(decision.wasted_bits), label);
    require(task.data.size == static_cast<int>(decision.estimated_bits), label);
}

void require_task_matches_task(
    const ldcompress::opencl_detail::FlacClSubframeTask& actual,
    const ldcompress::opencl_detail::FlacClSubframeTask& expected,
    const char* label)
{
    require(actual.data.type == expected.data.type, label);
    require(actual.data.residualOrder == expected.data.residualOrder, label);
    require(actual.data.samplesOffs == expected.data.samplesOffs, label);
    require(actual.data.wbits == expected.data.wbits, label);
    require(actual.data.abits == expected.data.abits, label);
    require(actual.data.porder == expected.data.porder, label);
    require(actual.data.size == expected.data.size, label);
}

void require_analysis_matches(
    const ldcompress::opencl_detail::OpenClMonoFixedConstantAnalysisResult& actual,
    const ldcompress::opencl_detail::OpenClMonoFixedConstantAnalysisResult& expected,
    const char* label)
{
    require(actual.analyzed_tasks.size() == expected.analyzed_tasks.size(), label);
    require(actual.best_tasks.size() == expected.best_tasks.size(), label);

    for (std::size_t i = 0; i < expected.analyzed_tasks.size(); ++i) {
        require_task_matches_task(actual.analyzed_tasks[i], expected.analyzed_tasks[i], label);
    }
    for (std::size_t i = 0; i < expected.best_tasks.size(); ++i) {
        require_task_matches_task(actual.best_tasks[i], expected.best_tasks[i], label);
    }
}

void require_lpc_task_matches(
    const ldcompress::opencl_detail::FlacClSubframeTask& actual,
    const ldcompress::opencl_detail::FlacClSubframeTask& expected,
    const char* label)
{
    require_task_matches_task(actual, expected, label);
    require(actual.data.shift == expected.data.shift, label);
    require(actual.data.cbits == expected.data.cbits, label);
    for (int i = 0; i < expected.data.residualOrder; ++i) {
        require(actual.coefs[static_cast<std::size_t>(i)] ==
                expected.coefs[static_cast<std::size_t>(i)],
            label);
    }
}

ldcompress::opencl_detail::OpenClMonoAnalysisTaskPlan make_single_lpc_task_plan(
    const ldcompress::opencl_detail::FlacClSubframeTask& task)
{
    ldcompress::opencl_detail::OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks.push_back(task);
    plan.selected_tasks.push_back(0);
    plan.residual_tasks_per_frame = 1;
    plan.estimate_tasks_per_frame = 1;
    return plan;
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

void test_scalar_exact_fixed_constant_analysis()
{
    using namespace ldcompress::opencl_detail;

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 64;
    options.max_lpc_order = 0;
    options.include_constant = true;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const auto tasks_per_frame = mono_analysis_tasks_per_frame(options);
    require(tasks_per_frame == 6, "unexpected fixed/constant exact task count");
    const auto plan = build_mono_analysis_task_plan(2, options);

    std::vector<std::int32_t> samples;
    samples.reserve(128);
    for (int i = 0; i < 64; ++i) {
        samples.push_back(1024);
    }
    for (int i = 0; i < 64; ++i) {
        samples.push_back(i * 64);
    }

    const auto result = analyze_mono_fixed_constant_exact(samples, plan, 4);
    require(result.device_name == "scalar-exact", "scalar exact analyzer did not identify itself");
    require(result.analyzed_tasks.size() == 12, "scalar exact analyzed task count mismatch");
    require(result.best_tasks.size() == 2, "scalar exact best task count mismatch");

    for (std::size_t i = 0; i < tasks_per_frame; ++i) {
        require(result.analyzed_tasks[i].data.wbits == 10,
            "scalar exact constant frame wasted-bits mismatch");
    }
    for (std::size_t i = tasks_per_frame; i < tasks_per_frame * 2; ++i) {
        require(result.analyzed_tasks[i].data.wbits == 6,
            "scalar exact linear frame wasted-bits mismatch");
    }

    const std::vector<std::int32_t> constant_frame(samples.begin(), samples.begin() + 64);
    const auto constant_decision = ldcompress::analyze_mono_best_frame(
        constant_frame, make_fixed_only_frame_info(4));
    require(constant_decision.kind == ldcompress::FlacSubframeKind::Constant,
        "scalar native analyzer did not choose constant");
    require_task_matches_decision(result.best_tasks[0], constant_decision,
        "scalar exact constant task did not match native analyzer");

    const std::vector<std::int32_t> linear_frame(samples.begin() + 64, samples.end());
    const auto linear_decision = ldcompress::analyze_mono_best_frame(
        linear_frame, make_fixed_only_frame_info(4));
    require(linear_decision.kind == ldcompress::FlacSubframeKind::FixedRice,
        "scalar native analyzer did not choose fixed/Rice");
    require(linear_decision.fixed_order == 2, "scalar native analyzer did not choose fixed order 2");
    require_task_matches_decision(result.best_tasks[1], linear_decision,
        "scalar exact fixed task did not match native analyzer");
}

void test_scalar_exact_rice_partition_search()
{
    using namespace ldcompress::opencl_detail;

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 64;
    options.max_lpc_order = 0;
    options.include_constant = true;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const std::vector<std::int32_t> samples {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    };
    const auto plan = build_mono_analysis_task_plan(1, options);

    const auto partitioned = analyze_mono_fixed_constant_exact(samples, plan, 4);
    const auto partitioned_decision = ldcompress::analyze_mono_best_frame(
        samples, make_fixed_only_frame_info(4));
    require(partitioned_decision.kind == ldcompress::FlacSubframeKind::FixedRice,
        "scalar native analyzer did not choose fixed/Rice for partitioned samples");
    require(partitioned_decision.fixed_order == 0,
        "scalar native analyzer chose unexpected fixed order for partitioned samples");
    require(partitioned_decision.rice_partition_order == 1,
        "scalar native analyzer did not choose Rice partition order 1");
    require_task_matches_decision(partitioned.best_tasks[0], partitioned_decision,
        "scalar exact partitioned task did not match native analyzer");

    const auto unpartitioned = analyze_mono_fixed_constant_exact(samples, plan, 0);
    require(unpartitioned.best_tasks[0].data.type == kFlacClSubframeFixed,
        "scalar exact unpartitioned task did not choose a fixed candidate");
    require(unpartitioned.best_tasks[0].data.porder == 0,
        "scalar exact unpartitioned task did not honor Rice partition order limit");
    require(unpartitioned.best_tasks[0].data.size > partitioned.best_tasks[0].data.size,
        "scalar exact partition search did not improve the fixed task size");
}

void test_scalar_exact_lpc_task_analysis()
{
    using namespace ldcompress::opencl_detail;

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 512;
    options.max_lpc_order = 12;
    options.include_constant = false;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const auto samples = make_lpc_friendly_samples();
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = 12,
        .lpc_coefficient_precision = 12,
        .max_rice_partition_order = 5,
    };
    const auto best_lpc = ldcompress::analyze_mono_lpc_frame(samples, frame_info);
    require(best_lpc.has_value(), "native LPC analysis did not return a candidate");

    const auto lpc_task = analyze_mono_lpc_exact_task(
        samples, 0, options, best_lpc->order, 12, 5);
    require(lpc_task.has_value(), "scalar exact LPC task analysis did not return a task");
    require(lpc_task->data.type == kFlacClSubframeLpc,
        "scalar exact LPC task type mismatch");
    require(lpc_task->data.residualOrder == static_cast<int>(best_lpc->order),
        "scalar exact LPC task order mismatch");
    require(lpc_task->data.samplesOffs == 0,
        "scalar exact LPC task sample offset mismatch");
    require(lpc_task->data.blocksize == 512,
        "scalar exact LPC task block size mismatch");
    require(lpc_task->data.shift == best_lpc->quantization_shift,
        "scalar exact LPC task quantization shift mismatch");
    require(lpc_task->data.cbits == static_cast<int>(best_lpc->coefficient_precision),
        "scalar exact LPC task coefficient precision mismatch");
    require(lpc_task->data.size == static_cast<int>(best_lpc->estimated_bits),
        "scalar exact LPC task size mismatch");
    require(lpc_task->data.wbits == static_cast<int>(best_lpc->wasted_bits),
        "scalar exact LPC task wasted-bits mismatch");
    require(lpc_task->data.porder == static_cast<int>(best_lpc->rice_partition_order),
        "scalar exact LPC task partition order mismatch");
    for (std::size_t i = 0; i < best_lpc->coefficients.size(); ++i) {
        const auto reversed_index = best_lpc->coefficients.size() - i - 1U;
        require(lpc_task->coefs[i] == best_lpc->coefficients[reversed_index],
            "scalar exact LPC task coefficient mismatch");
    }

    std::vector<std::int32_t> two_frames = samples;
    two_frames.insert(two_frames.end(), samples.begin(), samples.end());
    const auto second_task = analyze_mono_lpc_exact_task(
        two_frames, 1, options, best_lpc->order, 12, 5);
    require(second_task.has_value(), "scalar exact LPC task analysis did not return a second-frame task");
    require(second_task->data.samplesOffs == 512,
        "scalar exact LPC second-frame sample offset mismatch");
    require(second_task->data.size == lpc_task->data.size,
        "scalar exact LPC second-frame size mismatch");

    auto disabled_options = options;
    disabled_options.max_lpc_order = 0;
    require(!analyze_mono_lpc_exact_task(samples, 0, disabled_options, 1, 12, 5).has_value(),
        "scalar exact LPC task analysis returned a task with LPC disabled");

    auto tiny_options = options;
    tiny_options.frame_samples = 64;
    tiny_options.max_lpc_order = 12;
    const std::vector<std::int32_t> tiny_samples(samples.begin(), samples.begin() + 64);
    require(!analyze_mono_lpc_exact_task(tiny_samples, 0, tiny_options, 1, 12, 5).has_value(),
        "scalar exact LPC task analysis returned a task for a too-small frame");
}

void test_opencl_lpc_analysis_input_validation()
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL LPC input validation skipped: OpenCL support was not built\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 512;
    options.max_lpc_order = 12;
    options.include_constant = false;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const auto samples = make_lpc_friendly_samples();
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = 12,
        .lpc_coefficient_precision = 12,
        .max_rice_partition_order = 5,
    };
    const auto best_lpc = ldcompress::analyze_mono_lpc_frame(samples, frame_info);
    require(best_lpc.has_value(), "native LPC analysis did not return a validation candidate");
    const auto expected_task = analyze_mono_lpc_exact_task(
        samples, 0, options, best_lpc->order, 12, 5);
    require(expected_task.has_value(), "scalar exact LPC validation task was not produced");

    auto bad_coefficient_task = *expected_task;
    bad_coefficient_task.data.cbits = 4;
    bad_coefficient_task.coefs[0] = 8;
    auto bad_coefficient_plan = make_single_lpc_task_plan(bad_coefficient_task);
    require_throws_containing([&] {
        (void)run_opencl_mono_lpc_analysis(samples, bad_coefficient_plan, std::nullopt, 5);
    }, "coefficient does not fit precision",
        "OpenCL LPC analysis accepted a coefficient outside cbits");

    auto two_frames = samples;
    two_frames.insert(two_frames.end(), samples.begin(), samples.end());
    OpenClMonoAnalysisTaskPlan mixed_group_plan;
    mixed_group_plan.residual_tasks = { *expected_task, *expected_task };
    mixed_group_plan.residual_tasks[1].data.samplesOffs = 512;
    mixed_group_plan.selected_tasks = { 0, 1 };
    mixed_group_plan.residual_tasks_per_frame = 2;
    mixed_group_plan.estimate_tasks_per_frame = 2;
    require_throws_containing([&] {
        (void)run_opencl_mono_lpc_analysis(two_frames, mixed_group_plan, std::nullopt, 5);
    }, "frame task group mixes sample ranges",
        "OpenCL LPC analysis accepted mixed frame task sample ranges");
}

void test_opencl_best_method_smoke()
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL best-method smoke skipped: OpenCL support was not built\n";
        return;
    }

    const auto device_index = first_available_opencl_device_index();
    if (!device_index.has_value()) {
        std::cout << "OpenCL best-method smoke skipped: no available OpenCL device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions options;
    const auto tasks_per_frame = mono_analysis_tasks_per_frame(options);
    auto plan = build_mono_analysis_task_plan(2, options);

    plan.residual_tasks[5].data.size = 101;
    plan.residual_tasks[12].data.size = 202;
    plan.residual_tasks[17].data.size = 303;

    const auto second_frame = tasks_per_frame;
    plan.residual_tasks[second_frame + 0].data.size = 404;
    plan.residual_tasks[second_frame + 8].data.size = 88;
    plan.residual_tasks[second_frame + 14].data.size = 99;

    const auto result = run_opencl_mono_best_method(plan, device_index);
    require(!result.device_name.empty(), "OpenCL best-method result did not report a device");
    require(result.best_tasks.size() == 2, "OpenCL best-method result frame count mismatch");

    require(result.best_tasks[0].data.size == 101, "OpenCL best-method first frame size mismatch");
    require(result.best_tasks[0].data.type == kFlacClSubframeLpc,
        "OpenCL best-method first frame type mismatch");
    require(result.best_tasks[0].data.residualOrder == 6,
        "OpenCL best-method first frame order mismatch");

    require(result.best_tasks[1].data.size == 88, "OpenCL best-method second frame size mismatch");
    require(result.best_tasks[1].data.type == kFlacClSubframeLpc,
        "OpenCL best-method second frame type mismatch");
    require(result.best_tasks[1].data.residualOrder == 9,
        "OpenCL best-method second frame order mismatch");
}

void test_opencl_fixed_constant_analysis_smoke()
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL fixed/constant analysis smoke skipped: OpenCL support was not built\n";
        return;
    }

    const auto device_index = first_available_opencl_device_index();
    if (!device_index.has_value()) {
        std::cout << "OpenCL fixed/constant analysis smoke skipped: no available OpenCL device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 64;
    options.max_lpc_order = 0;
    options.include_constant = true;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const auto tasks_per_frame = mono_analysis_tasks_per_frame(options);
    require(tasks_per_frame == 6, "unexpected fixed/constant task count");
    const auto plan = build_mono_analysis_task_plan(3, options);

    std::vector<std::int32_t> samples;
    samples.reserve(192);
    for (int i = 0; i < 64; ++i) {
        samples.push_back(1024);
    }
    for (int i = 0; i < 64; ++i) {
        samples.push_back(i * 64);
    }
    const std::vector<std::int32_t> partitioned {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    };
    samples.insert(samples.end(), partitioned.begin(), partitioned.end());

    const auto expected = analyze_mono_fixed_constant_exact(samples, plan, 4);
    const auto result = run_opencl_mono_fixed_constant_analysis(samples, plan, device_index, 4);
    require(!result.device_name.empty(), "OpenCL fixed/constant result did not report a device");
    require(result.analyzed_tasks.size() == 18, "OpenCL analyzed task count mismatch");
    require(result.best_tasks.size() == 3, "OpenCL fixed/constant best task count mismatch");
    require_analysis_matches(result, expected, "OpenCL exact fixed/constant analysis diverged from scalar oracle");

    for (std::size_t i = 0; i < tasks_per_frame; ++i) {
        require(result.analyzed_tasks[i].data.wbits == 10,
            "constant frame wasted-bits mismatch");
    }
    for (std::size_t i = tasks_per_frame; i < tasks_per_frame * 2; ++i) {
        require(result.analyzed_tasks[i].data.wbits == 6,
            "linear frame wasted-bits mismatch");
    }

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "constant frame did not select constant task");
    require(result.best_tasks[0].data.size == 24,
        "constant frame selected size mismatch");

    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "linear frame did not select fixed task");
    require(result.best_tasks[1].data.residualOrder == 2,
        "linear frame did not select fixed order 2");
    require(result.best_tasks[1].data.size < 16 * 64,
        "linear frame fixed task did not beat verbatim baseline");

    require(result.best_tasks[2].data.type == kFlacClSubframeFixed,
        "partitioned frame did not select fixed task");
    require(result.best_tasks[2].data.residualOrder == 0,
        "partitioned frame did not select fixed order 0");
    require(result.best_tasks[2].data.porder == 1,
        "partitioned frame did not select Rice partition order 1");

    const auto expected_unpartitioned = analyze_mono_fixed_constant_exact(samples, plan, 0);
    const auto result_unpartitioned =
        run_opencl_mono_fixed_constant_analysis(samples, plan, device_index, 0);
    require_analysis_matches(result_unpartitioned, expected_unpartitioned,
        "OpenCL exact fixed/constant analysis did not honor max partition order 0");
    require(result_unpartitioned.best_tasks[2].data.porder == 0,
        "partitioned frame did not honor max partition order 0");
    require(result_unpartitioned.best_tasks[2].data.size > result.best_tasks[2].data.size,
        "OpenCL exact partition search did not improve fixed task size");
}

void test_opencl_lpc_analysis_smoke()
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL LPC analysis smoke skipped: OpenCL support was not built\n";
        return;
    }

    const auto device_index = first_available_opencl_device_index();
    if (!device_index.has_value()) {
        std::cout << "OpenCL LPC analysis smoke skipped: no available OpenCL device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = 512;
    options.max_lpc_order = 12;
    options.include_constant = false;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;

    const auto samples = make_lpc_friendly_samples();
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = 12,
        .lpc_coefficient_precision = 12,
        .max_rice_partition_order = 5,
    };
    const auto best_lpc = ldcompress::analyze_mono_lpc_frame(samples, frame_info);
    require(best_lpc.has_value(), "native LPC analysis did not return a candidate for OpenCL test");
    const auto expected_task = analyze_mono_lpc_exact_task(
        samples, 0, options, best_lpc->order, 12, 5);
    require(expected_task.has_value(), "scalar exact LPC task analysis did not return a task for OpenCL test");

    auto input_task = *expected_task;
    input_task.data.size = 16 * 512;
    input_task.data.abits = 0;
    input_task.data.porder = 99;

    auto plan = make_single_lpc_task_plan(input_task);

    const auto result = run_opencl_mono_lpc_analysis(samples, plan, device_index, 5);
    require(!result.device_name.empty(), "OpenCL LPC result did not report a device");
    require(result.analyzed_tasks.size() == 1, "OpenCL LPC analyzed task count mismatch");
    require(result.best_tasks.size() == 1, "OpenCL LPC best task count mismatch");
    require_lpc_task_matches(result.analyzed_tasks[0], *expected_task,
        "OpenCL exact LPC analyzed task diverged from scalar oracle");
    require_lpc_task_matches(result.best_tasks[0], *expected_task,
        "OpenCL exact LPC best task diverged from scalar oracle");
}

}  // namespace

int main()
{
    try {
        test_flaccl_abi_layout();
        test_default_mono_task_plan();
        test_small_custom_task_plan();
        test_invalid_options();
        test_scalar_exact_fixed_constant_analysis();
        test_scalar_exact_rice_partition_search();
        test_scalar_exact_lpc_task_analysis();
        test_opencl_lpc_analysis_input_validation();
        test_opencl_best_method_smoke();
        test_opencl_fixed_constant_analysis_smoke();
        test_opencl_lpc_analysis_smoke();
    } catch (const std::exception& ex) {
        std::cerr << "test_opencl_analysis: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
