#include "opencl_analysis.h"

#include <limits>
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

#if LDCOMPRESS_HAVE_OPENCL

constexpr cl_int kPlatformNotFound = -1001;

const char* mono_best_method_kernel_source()
{
    return R"CLC(
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
}

#endif

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

    const char* source = mono_best_method_kernel_source();
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
        .device_name = selected_device.name,
    };
#else
    (void)plan;
    (void)requested_device_index;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

}  // namespace ldcompress::opencl_detail
