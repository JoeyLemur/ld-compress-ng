#include "opencl_analysis.h"
#include "vulkan_analysis.h"
#include "vulkan_devices.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t parse_device_index(std::string_view text)
{
    if (text.empty()) {
        throw std::runtime_error("empty Vulkan device index");
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid Vulkan device index: " + std::string(text));
        }
        value = (value * 10U) + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

struct Options {
    std::optional<std::size_t> device_index;
};

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error("--device requires a value");
            }
            options.device_index = parse_device_index(argv[i]);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    return options;
}

std::optional<std::size_t> first_available_vulkan_analysis_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::vulkan_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_vulkan_devices();
    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("requested Vulkan device index is out of range");
        }
        const auto& device = devices[*requested_index];
        if (device.available && device.shader_int64) {
            return device.index;
        }
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && device.shader_int64) {
            return device.index;
        }
    }
    return std::nullopt;
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

std::vector<std::int32_t> make_fixed_constant_samples()
{
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
    return samples;
}

void test_vulkan_fixed_constant_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan fixed/constant analysis skipped: Vulkan support was not built\n";
        return;
    }

    const auto device_index =
        first_available_vulkan_analysis_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Vulkan fixed/constant analysis skipped: no suitable Vulkan device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 64;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto tasks_per_frame = mono_analysis_tasks_per_frame(task_options);
    require(tasks_per_frame == 6, "unexpected fixed/constant task count");
    const auto plan = build_mono_analysis_task_plan(3, task_options);
    const auto samples = make_fixed_constant_samples();

    const auto expected = analyze_mono_fixed_constant_exact(samples, plan, 4);
    const auto result = ldcompress::vulkan_detail::run_vulkan_mono_fixed_constant_analysis(
        samples, plan, device_index, 4);
    require(!result.device_name.empty(), "Vulkan fixed/constant result did not report a device");
    require_analysis_matches(
        result, expected, "Vulkan exact fixed/constant analysis diverged from scalar oracle");

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "constant frame did not select constant task");
    require(result.best_tasks[0].data.size == 24,
        "constant frame selected size mismatch");
    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "linear frame did not select fixed task");
    require(result.best_tasks[1].data.residualOrder == 2,
        "linear frame did not select fixed order 2");
    require(result.best_tasks[2].data.type == kFlacClSubframeFixed,
        "partitioned frame did not select fixed task");
    require(result.best_tasks[2].data.residualOrder == 0,
        "partitioned frame did not select fixed order 0");
    require(result.best_tasks[2].data.porder == 1,
        "partitioned frame did not select Rice partition order 1");

    const auto expected_unpartitioned = analyze_mono_fixed_constant_exact(samples, plan, 0);
    const auto result_unpartitioned =
        ldcompress::vulkan_detail::run_vulkan_mono_fixed_constant_analysis(
            samples, plan, device_index, 0);
    require_analysis_matches(result_unpartitioned, expected_unpartitioned,
        "Vulkan exact fixed/constant analysis did not honor max partition order 0");
    require(result_unpartitioned.best_tasks[2].data.porder == 0,
        "partitioned frame did not honor max partition order 0");
    require(result_unpartitioned.best_tasks[2].data.size > result.best_tasks[2].data.size,
        "Vulkan exact partition search did not improve fixed task size");
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        test_vulkan_fixed_constant_analysis(parse_args(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << "test_vulkan_analysis: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
