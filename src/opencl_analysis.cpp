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

constexpr std::size_t kOpenClAnalysisMaxBlockSize = 8192;
constexpr std::size_t kOpenClAnalysisBitsPerSample = 16;

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
 * - Uses the FLACCLSubframeTask ABI but omits stereo, LPC autocorrelation,
 *   partition/Rice-output, and bitstream output kernels.
 * - Keeps the CPU-style one-work-item estimate path as the first portable
 *   fixed/constant analysis slice.
 */

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

std::size_t mono_plan_frame_count(const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    return plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
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
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis block size is unsupported");
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
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_fixed_constant_analysis_inputs(samples, plan);

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
    ClKernel estimate_kernel(clCreateKernel(program.get(), "ldcompressEstimateResidual", &status));
    require_cl(status, "clCreateKernel(ldcompressEstimateResidual)");
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

    require_cl(clSetKernelArg(estimate_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(estimate.samples)");
    require_cl(clSetKernelArg(estimate_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(estimate.selectedTasks)");
    require_cl(clSetKernelArg(estimate_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(estimate.tasks)");

    const std::size_t estimate_global_work_size = plan.selected_tasks.size();
    require_cl(clEnqueueNDRangeKernel(queue.get(), estimate_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressEstimateResidual)");

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
        .device_name = selected_device.name,
    };
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

}  // namespace ldcompress::opencl_detail
