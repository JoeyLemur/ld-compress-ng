#include "metal_analysis.h"

#include "flac_native_writer.h"
#include "metal_devices.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ldcompress::metal_detail {
namespace {

using opencl_detail::FlacClRiceParameterSet;
using opencl_detail::FlacClSubframeTask;
using opencl_detail::OpenClMonoAnalysisTaskPlan;
using opencl_detail::OpenClMonoBestMethodResult;
using opencl_detail::OpenClMonoFixedConstantAnalysisResult;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kWorkgroupSize = 64;
constexpr std::size_t kMetalMaxBlockSize = 8192;

struct ExactParams {
    std::uint32_t selected_task_count = 0;
    std::uint32_t max_rice_partition_order = 0;
    std::uint32_t analysis_profile = 0;
    std::uint32_t reserved = 0;
};

struct ChooseParams {
    std::uint32_t frame_count = 0;
    std::uint32_t tasks_per_frame = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point finish)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += elapsed_ns(start, Clock::now());
}

std::size_t checked_buffer_bytes(std::size_t count, std::size_t element_size, const char* name)
{
    if (count == 0) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " buffer is empty");
    }
    if (count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " buffer is too large");
    }
    return count * element_size;
}

std::uint32_t checked_u32(std::size_t value, const char* name)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

std::string ns_string_to_string(NSString* value)
{
    if (value == nil) {
        return {};
    }
    const char* text = [value UTF8String];
    return text == nullptr ? std::string {} : std::string(text);
}

std::string ns_error_text(NSError* error)
{
    if (error == nil) {
        return {};
    }
    NSString* text = [error localizedDescription];
    return ns_string_to_string(text);
}

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("Metal mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("Metal mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("Metal mono analysis currently supports full mono selection only");
    }
    if ((plan.selected_tasks.size() % plan.estimate_tasks_per_frame) != 0 ||
        (plan.residual_tasks.size() % plan.residual_tasks_per_frame) != 0) {
        throw std::runtime_error("Metal mono analysis task plan is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("Metal mono analysis selected/residual frame counts differ");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 || static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("Metal mono analysis selected task index is out of range");
        }
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        if (first_task.data.blocksize <= 0 ||
            static_cast<std::size_t>(first_task.data.blocksize) > kMetalMaxBlockSize) {
            throw std::runtime_error("Metal mono analysis block size is unsupported");
        }
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("Metal mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base || selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("Metal mono analysis selected task crosses frame group");
            }
        }
    }
}

std::size_t mono_plan_frame_count(const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    return plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
}

std::uint32_t metal_analysis_profile_arg(NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return 0;
    case NativeAnalysisProfile::OrderGuessExactRice:
        return 1;
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return 2;
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return 3;
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        return 4;
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        return 5;
    }
    return 0;
}

FlacFrameInfo frame_info_for_task(
    const FlacClSubframeTask& task,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    unsigned max_lpc_order)
{
    return FlacFrameInfo {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = static_cast<unsigned>(task.data.obits),
        .max_lpc_order = max_lpc_order,
        .lpc_coefficient_precision = lpc_coefficient_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
}

std::vector<std::int32_t> frame_samples_for_task(
    const std::vector<std::int32_t>& samples,
    const FlacClSubframeTask& task)
{
    if (task.data.samplesOffs < 0 || task.data.blocksize <= 0) {
        throw std::runtime_error("Metal generated LPC task has invalid sample range");
    }
    const auto offset = static_cast<std::size_t>(task.data.samplesOffs);
    const auto count = static_cast<std::size_t>(task.data.blocksize);
    if (offset > samples.size() || count > samples.size() - offset) {
        throw std::runtime_error("Metal generated LPC task samples are out of range");
    }
    return std::vector<std::int32_t>(
        samples.begin() + static_cast<std::ptrdiff_t>(offset),
        samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

void populate_lpc_task_from_analysis(
    FlacClSubframeTask& task,
    const FlacLpcSubframeAnalysis& analysis)
{
    task.data.residualOrder = static_cast<std::int32_t>(analysis.order);
    task.data.shift = analysis.quantization_shift;
    task.data.cbits = static_cast<std::int32_t>(analysis.coefficient_precision);
    task.data.wbits = static_cast<std::int32_t>(analysis.wasted_bits);
    task.data.porder = static_cast<std::int32_t>(analysis.rice_partition_order);
    task.data.size = analysis.estimated_bits >
            static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
        ? std::numeric_limits<std::int32_t>::max()
        : static_cast<std::int32_t>(analysis.estimated_bits);
    std::fill(task.coefs.begin(), task.coefs.end(), 0);
    for (std::size_t i = 0; i < analysis.coefficients.size() && i < task.coefs.size(); ++i) {
        task.coefs[i] = analysis.coefficients[i];
    }
}

void populate_generated_lpc_tasks_on_host(
    const std::vector<std::int32_t>& samples,
    OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    if (plan.max_lpc_order == 0) {
        return;
    }
    const auto frame_count = mono_plan_frame_count(plan);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_base = frame * plan.residual_tasks_per_frame;
        const auto first_task = plan.residual_tasks.at(frame_base);
        const auto frame_samples = frame_samples_for_task(samples, first_task);
        const auto frame_info = frame_info_for_task(
            first_task, lpc_coefficient_precision, max_rice_partition_order,
            plan.max_lpc_order);

        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            auto& task = plan.residual_tasks[frame_base + i];
            if (task.data.type != opencl_detail::kFlacClSubframeLpc) {
                continue;
            }
            const auto requested_order = task.data.residualOrder <= 0
                ? plan.max_lpc_order
                : std::min<unsigned>(
                    static_cast<unsigned>(task.data.residualOrder), plan.max_lpc_order);
            const auto candidates =
                analyze_mono_lpc_order_candidates(frame_samples, frame_info, requested_order);
            if (candidates.empty()) {
                task.data.size = std::numeric_limits<std::int32_t>::max();
                continue;
            }
            const auto best = std::min_element(
                candidates.begin(), candidates.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.estimated_bits < rhs.estimated_bits;
                });
            populate_lpc_task_from_analysis(task, *best);
        }
    }
}

const char* metal_analysis_source()
{
    return R"MSL(
/*
 * Portions adapted from CUETools.FLACCL-compatible mono analysis kernels.
 * Copyright (c) 2010-2022 Gregory S. Chudov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Local modifications for ld-compress-ng, 2026-07-08:
 * - Ported exact mono task costing and best-task sidecar output to Metal.
 */
#include <metal_stdlib>
using namespace metal;

constant uint kWorkgroupSize = 64u;
constant uint kMaxOrder = 32u;
constant uint kMaxRiceParameter = 14u;
constant uint kMaxRicePartitionCount = 256u;
constant int kSubframeConstant = 0;
constant int kSubframeFixed = 8;
constant int kSubframeLpc = 32;
constant int kIntMax = 2147483647;

struct FlacClSubframeData {
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
};

struct FlacClSubframeTask {
    FlacClSubframeData data;
    int coefs[32];
};

struct ExactParams {
    uint selectedTaskCount;
    uint maxRicePartitionOrder;
    uint analysisProfile;
    uint reserved;
};

struct ChooseParams {
    uint frameCount;
    uint tasksPerFrame;
    uint reserved0;
    uint reserved1;
};

struct RiceChoice {
    ulong bits;
    uint parameter;
};

uint bit_width_u32(uint value)
{
    uint bits = 0u;
    while (value != 0u) {
        value >>= 1u;
        ++bits;
    }
    return bits;
}

int sample_trailing_zero_bits(int smp, int bits_per_sample)
{
    if (smp == 0) {
        return bits_per_sample;
    }
    const uint mask = bits_per_sample >= 32
        ? 0xffffffffu
        : ((1u << uint(bits_per_sample)) - 1u);
    uint raw = uint(smp) & mask;
    int zeros = 0;
    while (zeros < bits_per_sample && (raw & 1u) == 0u) {
        raw >>= 1u;
        ++zeros;
    }
    return zeros;
}

int reduce_min_i32(int value, threadgroup int* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] = min(scratch[lane], scratch[lane + stride]);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

uint reduce_or_u32(uint value, threadgroup uint* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] |= scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

ulong reduce_sum_u64(ulong value, threadgroup ulong* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] += scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

int common_wasted_bits(device const int* data, int blocksize, int obits,
    threadgroup int* scratch, uint lane)
{
    int wasted = obits;
    for (int i = int(lane); i < blocksize; i += int(kWorkgroupSize)) {
        wasted = min(wasted, sample_trailing_zero_bits(data[i], obits));
    }
    return min(reduce_min_i32(wasted, scratch, lane), obits - 1);
}

int amplitude_bits(device const int* data, int blocksize, int wasted_bits,
    threadgroup uint* scratch, uint lane)
{
    uint amplitude_or = 0u;
    for (int i = int(lane); i < blocksize; i += int(kWorkgroupSize)) {
        const int sample = data[i];
        const uint folded = sample < 0
            ? uint(-(long(sample) + 1L))
            : uint(sample);
        amplitude_or |= folded;
    }
    const uint reduced = reduce_or_u32(amplitude_or, scratch, lane);
    if (reduced == 0u) {
        return 1;
    }
    const uint bits = bit_width_u32(reduced);
    return int(max(1u, bits > uint(wasted_bits) ? bits - uint(wasted_bits) : 0u));
}

int all_samples_equal(device const int* data, int blocksize,
    threadgroup uint* scratch, uint lane)
{
    const int first = data[0];
    uint mismatch = 0u;
    for (int i = int(lane) + 1; i < blocksize; i += int(kWorkgroupSize)) {
        if (data[i] != first) {
            mismatch = 1u;
        }
    }
    return reduce_or_u32(mismatch, scratch, lane) == 0u;
}

long shifted_sample(device const int* data, int pos, int wbits)
{
    const long sample = long(data[pos]);
    return wbits == 0 ? sample : sample / (1L << wbits);
}

long fixed_residual(device const int* data, int pos, int order, int wbits)
{
    const long sample = shifted_sample(data, pos, wbits);
    switch (order) {
    case 0:
        return sample;
    case 1:
        return sample - shifted_sample(data, pos - 1, wbits);
    case 2:
        return sample - (2L * shifted_sample(data, pos - 1, wbits)) +
            shifted_sample(data, pos - 2, wbits);
    case 3:
        return sample - (3L * shifted_sample(data, pos - 1, wbits)) +
            (3L * shifted_sample(data, pos - 2, wbits)) -
            shifted_sample(data, pos - 3, wbits);
    case 4:
        return sample - (4L * shifted_sample(data, pos - 1, wbits)) +
            (6L * shifted_sample(data, pos - 2, wbits)) -
            (4L * shifted_sample(data, pos - 3, wbits)) +
            shifted_sample(data, pos - 4, wbits);
    default:
        return 0L;
    }
}

long arithmetic_shift_right(long value, int shift)
{
    if (shift == 0) {
        return value;
    }
    if (value >= 0) {
        return value >> shift;
    }
    const long divisor = 1L << shift;
    return -(((-value) + divisor - 1L) >> shift);
}

long lpc_residual(device const int* data, int pos, thread const FlacClSubframeTask& task)
{
    const int order = task.data.residualOrder;
    long sum = 0L;
    for (int i = 0; i < order; ++i) {
        sum += long(task.coefs[i]) *
            shifted_sample(data, pos - order + i, task.data.wbits);
    }
    return shifted_sample(data, pos, task.data.wbits) -
        arithmetic_shift_right(sum, task.data.shift);
}

long residual(device const int* data, int pos, thread const FlacClSubframeTask& task)
{
    if (task.data.type == kSubframeLpc) {
        return lpc_residual(data, pos, task);
    }
    return fixed_residual(data, pos, task.data.residualOrder, task.data.wbits);
}

ulong fold_residual(long value)
{
    return value >= 0L
        ? ulong(value) << 1u
        : (ulong(-(value + 1L)) << 1u) + 1u;
}

int signed_value_fits_bits(int value, int bits)
{
    if (bits <= 0 || bits > 31) {
        return 0;
    }
    const int min_value = -(1 << (bits - 1));
    const int max_value = (1 << (bits - 1)) - 1;
    return value >= min_value && value <= max_value;
}

int valid_partition_order(int blocksize, int predictor_order, int partition_order)
{
    const int partition_count = 1 << partition_order;
    if ((blocksize % partition_count) != 0) {
        return 0;
    }
    return (blocksize / partition_count) > predictor_order;
}

RiceChoice best_rice_for_partition(device const int* data, int partition_start,
    int residual_count, thread const FlacClSubframeTask& task, threadgroup ulong* scratch,
    uint lane)
{
    RiceChoice best;
    best.bits = 0xffffffffffffffffUL;
    best.parameter = 0u;
    for (uint parameter = 0u; parameter <= kMaxRiceParameter; ++parameter) {
        ulong local_bits = 0UL;
        for (int i = int(lane); i < residual_count; i += int(kWorkgroupSize)) {
            const ulong folded = fold_residual(residual(data, partition_start + i, task));
            local_bits += (folded >> parameter) + 1UL + ulong(parameter);
        }
        const ulong bits = reduce_sum_u64(local_bits, scratch, lane);
        if (bits < best.bits) {
            best.bits = bits;
            best.parameter = parameter;
        }
    }
    return best;
}

kernel void analyze_exact(
    device const int* samples [[buffer(0)]],
    device const int* selected_tasks [[buffer(1)]],
    device FlacClSubframeTask* tasks [[buffer(2)]],
    device uint* task_rice_parameters [[buffer(3)]],
    constant ExactParams& params [[buffer(4)]],
    uint lane [[thread_index_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]])
{
    threadgroup int reduce_ints[64];
    threadgroup uint reduce_uints[64];
    threadgroup ulong reduce_ulongs[64];
    threadgroup uint candidate_rice_parameters[256];
    threadgroup uint best_rice_parameters[256];

    if (group >= params.selectedTaskCount) {
        return;
    }

    const int selected_task = selected_tasks[group];
    FlacClSubframeTask task = tasks[selected_task];
    const int rice_output_base = selected_task * int(kMaxRicePartitionCount);
    for (int i = int(lane); i < int(kMaxRicePartitionCount); i += int(kWorkgroupSize)) {
        task_rice_parameters[rice_output_base + i] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (task.data.size == kIntMax) {
        if (lane == 0u) {
            tasks[selected_task] = task;
        }
        return;
    }

    device const int* data = samples + task.data.samplesOffs;
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
    task.data.wbits = common_wasted_bits(data, bs, task.data.obits, reduce_ints, lane);
    task.data.abits = amplitude_bits(data, bs, task.data.wbits, reduce_uints, lane);
    task.data.porder = 0;
    const int wbits = task.data.wbits;
    const int obits = task.data.obits - wbits;

    if (task.data.type == kSubframeConstant) {
        const int equal = all_samples_equal(data, bs, reduce_uints, lane);
        if (lane == 0u) {
            task.data.size = equal
                ? 8 + obits + (wbits == 0 ? 0 : wbits)
                : kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if ((task.data.type != kSubframeFixed && task.data.type != kSubframeLpc) ||
        ro < 0 ||
        ro >= bs ||
        (task.data.type == kSubframeFixed && ro > 4) ||
        (task.data.type == kSubframeLpc &&
            (ro == 0 || ro > int(kMaxOrder) || task.data.shift < 0 ||
                task.data.shift > 15 || task.data.cbits <= 0 ||
                task.data.cbits > 15))) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if (task.data.type == kSubframeLpc) {
        int valid = 1;
        for (int i = 0; i < ro; ++i) {
            if (!signed_value_fits_bits(task.coefs[i], task.data.cbits)) {
                valid = 0;
            }
        }
        if (!valid) {
            if (lane == 0u) {
                task.data.size = kIntMax;
                tasks[selected_task] = task;
            }
            return;
        }
    }

    ulong best_bits = 0xffffffffffffffffUL;
    int best_partition_order = 0;
    const int max_partition_order = min(int(params.maxRicePartitionOrder), 8);
    const ulong base_bits = task.data.type == kSubframeLpc
        ? 8UL + (wbits == 0 ? 0UL : ulong(wbits)) +
            (ulong(ro) * ulong(obits)) + 4UL + 5UL +
            (ulong(ro) * ulong(task.data.cbits)) + 2UL + 4UL
        : 8UL + (ulong(ro) * ulong(obits)) + 2UL + 4UL +
            (wbits == 0 ? 0UL : ulong(wbits));

    for (int partition_order = 0; partition_order <= max_partition_order; ++partition_order) {
        if (!valid_partition_order(bs, ro, partition_order)) {
            continue;
        }
        const int partition_count = 1 << partition_order;
        const int partition_samples = bs >> partition_order;
        ulong bits = base_bits;
        for (int partition = 0; partition < partition_count; ++partition) {
            const int residual_count = partition == 0
                ? partition_samples - ro
                : partition_samples;
            const int partition_start = partition == 0
                ? ro
                : partition * partition_samples;
            const RiceChoice choice =
                best_rice_for_partition(data, partition_start, residual_count,
                    task, reduce_ulongs, lane);
            bits += 4UL + choice.bits;
            if (lane == 0u) {
                candidate_rice_parameters[partition] = choice.parameter;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (bits < best_bits) {
            best_bits = bits;
            best_partition_order = partition_order;
            for (int partition = int(lane); partition < partition_count;
                 partition += int(kWorkgroupSize)) {
                best_rice_parameters[partition] = candidate_rice_parameters[partition];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    const int best_partition_count = 1 << best_partition_order;
    for (int partition = int(lane); partition < best_partition_count;
         partition += int(kWorkgroupSize)) {
        task_rice_parameters[rice_output_base + partition] = best_rice_parameters[partition];
    }

    if (lane == 0u) {
        task.data.porder = best_partition_order;
        task.data.size = best_bits > ulong(kIntMax) ? kIntMax : int(best_bits);
        tasks[selected_task] = task;
    }
}

kernel void choose_best(
    device const int* selected_tasks [[buffer(0)]],
    device const FlacClSubframeTask* tasks [[buffer(1)]],
    device const uint* task_rice_parameters [[buffer(2)]],
    device FlacClSubframeTask* best_tasks [[buffer(3)]],
    device uint* best_rice_parameters [[buffer(4)]],
    constant ChooseParams& params [[buffer(5)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= params.frameCount) {
        return;
    }
    const uint selected_base = gid * params.tasksPerFrame;
    int best_task = selected_tasks[selected_base];
    int best_size = tasks[best_task].data.size;
    for (uint i = 1u; i < params.tasksPerFrame; ++i) {
        const int task = selected_tasks[selected_base + i];
        const int size = tasks[task].data.size;
        if (size < best_size) {
            best_task = task;
            best_size = size;
        }
    }
    best_tasks[gid] = tasks[best_task];
    const uint input_base = uint(best_task) * kMaxRicePartitionCount;
    const uint output_base = gid * kMaxRicePartitionCount;
    for (uint i = 0u; i < kMaxRicePartitionCount; ++i) {
        best_rice_parameters[output_base + i] = task_rice_parameters[input_base + i];
    }
}
)MSL";
}

NSArray<id<MTLDevice>>* copy_all_devices()
{
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    if (devices != nil && [devices count] != 0) {
        return devices;
    }
    id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
    if (default_device == nil) {
        return @[];
    }
    return @[ default_device ];
}

id<MTLDevice> copy_selected_device(std::optional<std::size_t> requested_index)
{
    const auto selected = select_metal_device(requested_index);
    NSArray<id<MTLDevice>>* devices = copy_all_devices();
    if (selected.index >= [devices count]) {
        throw std::runtime_error("selected Metal device disappeared during setup");
    }
    id<MTLDevice> device = [devices objectAtIndex:selected.index];
    if (device == nil) {
        throw std::runtime_error("selected Metal device is nil");
    }
    return device;
}

id<MTLBuffer> ensure_buffer(
    id<MTLDevice> device,
    id<MTLBuffer> buffer,
    std::size_t& capacity_bytes,
    std::size_t required_bytes,
    const char* name)
{
    if (required_bytes == 0) {
        throw std::runtime_error(std::string("Metal ") + name + " buffer is empty");
    }
    if (required_bytes <= capacity_bytes && buffer != nil) {
        return buffer;
    }
    buffer = [device newBufferWithLength:required_bytes options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw std::runtime_error(std::string("failed to allocate Metal ") + name + " buffer");
    }
    capacity_bytes = required_bytes;
    return buffer;
}

void copy_to_buffer(id<MTLBuffer> buffer, const void* data, std::size_t bytes)
{
    std::memcpy([buffer contents], data, bytes);
}

void copy_from_buffer(void* data, id<MTLBuffer> buffer, std::size_t bytes)
{
    std::memcpy(data, [buffer contents], bytes);
}

}  // namespace

class MetalMonoAnalysisSession::Impl final {
public:
    explicit Impl(std::optional<std::size_t> requested_device_index)
    {
        @autoreleasepool {
            device_ = copy_selected_device(requested_device_index);
            device_name_ = ns_string_to_string([device_ name]);
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                throw std::runtime_error("failed to create Metal command queue");
            }

            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:metal_analysis_source()];
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:source options:nil error:&error];
            if (library == nil) {
                throw std::runtime_error("failed to compile Metal analysis source: " +
                    ns_error_text(error));
            }

            id<MTLFunction> exact_function = [library newFunctionWithName:@"analyze_exact"];
            id<MTLFunction> choose_function = [library newFunctionWithName:@"choose_best"];
            if (exact_function == nil || choose_function == nil) {
                throw std::runtime_error("failed to resolve Metal analysis kernel functions");
            }

            exact_pipeline_ =
                [device_ newComputePipelineStateWithFunction:exact_function error:&error];
            if (exact_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal exact-analysis pipeline: " +
                    ns_error_text(error));
            }
            choose_pipeline_ =
                [device_ newComputePipelineStateWithFunction:choose_function error:&error];
            if (choose_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal choose-best pipeline: " +
                    ns_error_text(error));
            }
        }
    }

    OpenClMonoFixedConstantAnalysisResult run_analysis(
        const std::vector<std::int32_t>& samples,
        OpenClMonoAnalysisTaskPlan plan,
        unsigned lpc_coefficient_precision,
        unsigned max_rice_partition_order,
        bool populate_lpc,
        bool read_analyzed_tasks,
        MetalGpuTimingStats* timings)
    {
        @autoreleasepool {
            if (max_rice_partition_order > opencl_detail::kFlacClMaxRicePartitionOrder) {
                throw std::runtime_error("Metal exact analysis max Rice partition order must be 0..8");
            }
            const auto frame_count = mono_plan_frame_count(plan);
            if (samples.empty()) {
                throw std::runtime_error("Metal analysis samples are empty");
            }
            if (populate_lpc) {
                const auto lpc_started = Clock::now();
                populate_generated_lpc_tasks_on_host(
                    samples, plan, lpc_coefficient_precision, max_rice_partition_order);
                if (timings != nullptr) {
                    add_elapsed_ns(timings->lpc_generation_ns, lpc_started);
                }
            }

            const auto samples_bytes =
                checked_buffer_bytes(samples.size(), sizeof(std::int32_t), "samples");
            const auto tasks_bytes = checked_buffer_bytes(
                plan.residual_tasks.size(), sizeof(FlacClSubframeTask), "tasks");
            const auto selected_bytes = checked_buffer_bytes(
                plan.selected_tasks.size(), sizeof(std::int32_t), "selectedTasks");
            const auto task_rice_bytes = checked_buffer_bytes(
                plan.residual_tasks.size(),
                sizeof(FlacClRiceParameterSet),
                "taskRiceParameters");
            const auto best_bytes =
                checked_buffer_bytes(frame_count, sizeof(FlacClSubframeTask), "bestTasks");
            const auto best_rice_bytes =
                checked_buffer_bytes(frame_count, sizeof(FlacClRiceParameterSet),
                    "bestRiceParameters");

            samples_buffer_ = ensure_buffer(device_, samples_buffer_, samples_buffer_bytes_,
                samples_bytes, "samples");
            tasks_buffer_ = ensure_buffer(device_, tasks_buffer_, tasks_buffer_bytes_,
                tasks_bytes, "tasks");
            selected_buffer_ = ensure_buffer(device_, selected_buffer_, selected_buffer_bytes_,
                selected_bytes, "selectedTasks");
            task_rice_parameters_buffer_ = ensure_buffer(device_, task_rice_parameters_buffer_,
                task_rice_parameters_buffer_bytes_, task_rice_bytes,
                "taskRiceParameters");
            best_buffer_ = ensure_buffer(device_, best_buffer_, best_buffer_bytes_,
                best_bytes, "bestTasks");
            best_rice_parameters_buffer_ = ensure_buffer(device_, best_rice_parameters_buffer_,
                best_rice_parameters_buffer_bytes_, best_rice_bytes,
                "bestRiceParameters");

            const auto upload_started = Clock::now();
            copy_to_buffer(samples_buffer_, samples.data(), samples_bytes);
            copy_to_buffer(tasks_buffer_, plan.residual_tasks.data(), tasks_bytes);
            copy_to_buffer(selected_buffer_, plan.selected_tasks.data(), selected_bytes);
            if (timings != nullptr) {
                add_elapsed_ns(timings->upload_ns, upload_started);
            }

            ExactParams exact_params {
                .selected_task_count = checked_u32(plan.selected_tasks.size(), "selected tasks"),
                .max_rice_partition_order = max_rice_partition_order,
                .analysis_profile = metal_analysis_profile_arg(plan.analysis_profile),
                .reserved = 0,
            };
            ChooseParams choose_params {
                .frame_count = checked_u32(frame_count, "frame count"),
                .tasks_per_frame = checked_u32(plan.estimate_tasks_per_frame, "tasks per frame"),
                .reserved0 = 0,
                .reserved1 = 0,
            };

            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) {
                throw std::runtime_error("failed to create Metal command buffer");
            }

            const auto exact_started = Clock::now();
            id<MTLComputeCommandEncoder> exact_encoder = [command computeCommandEncoder];
            if (exact_encoder == nil) {
                throw std::runtime_error("failed to create Metal exact-analysis encoder");
            }
            [exact_encoder setComputePipelineState:exact_pipeline_];
            [exact_encoder setBuffer:samples_buffer_ offset:0 atIndex:0];
            [exact_encoder setBuffer:selected_buffer_ offset:0 atIndex:1];
            [exact_encoder setBuffer:tasks_buffer_ offset:0 atIndex:2];
            [exact_encoder setBuffer:task_rice_parameters_buffer_ offset:0 atIndex:3];
            [exact_encoder setBytes:&exact_params length:sizeof(exact_params) atIndex:4];
            [exact_encoder dispatchThreadgroups:MTLSizeMake(plan.selected_tasks.size(), 1, 1)
                          threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
            [exact_encoder endEncoding];

            id<MTLComputeCommandEncoder> choose_encoder = [command computeCommandEncoder];
            if (choose_encoder == nil) {
                throw std::runtime_error("failed to create Metal choose-best encoder");
            }
            [choose_encoder setComputePipelineState:choose_pipeline_];
            [choose_encoder setBuffer:selected_buffer_ offset:0 atIndex:0];
            [choose_encoder setBuffer:tasks_buffer_ offset:0 atIndex:1];
            [choose_encoder setBuffer:task_rice_parameters_buffer_ offset:0 atIndex:2];
            [choose_encoder setBuffer:best_buffer_ offset:0 atIndex:3];
            [choose_encoder setBuffer:best_rice_parameters_buffer_ offset:0 atIndex:4];
            [choose_encoder setBytes:&choose_params length:sizeof(choose_params) atIndex:5];
            [choose_encoder dispatchThreads:MTLSizeMake(frame_count, 1, 1)
                       threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
            [choose_encoder endEncoding];

            [command commit];
            [command waitUntilCompleted];
            if ([command status] != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal analysis command failed: " +
                    ns_error_text([command error]));
            }
            if (timings != nullptr) {
                add_elapsed_ns(timings->exact_analysis_ns, exact_started);
                ++timings->batches;
            }

            const auto readback_started = Clock::now();
            std::vector<FlacClSubframeTask> analyzed_tasks;
            if (read_analyzed_tasks) {
                analyzed_tasks.resize(plan.residual_tasks.size());
                copy_from_buffer(analyzed_tasks.data(), tasks_buffer_, tasks_bytes);
            }
            std::vector<FlacClSubframeTask> best_tasks(frame_count);
            std::vector<FlacClRiceParameterSet> best_rice_parameters(frame_count);
            copy_from_buffer(best_tasks.data(), best_buffer_, best_bytes);
            copy_from_buffer(best_rice_parameters.data(),
                best_rice_parameters_buffer_, best_rice_bytes);
            if (timings != nullptr) {
                add_elapsed_ns(timings->readback_ns, readback_started);
            }

            return OpenClMonoFixedConstantAnalysisResult {
                .analyzed_tasks = std::move(analyzed_tasks),
                .best_tasks = std::move(best_tasks),
                .best_rice_parameters = std::move(best_rice_parameters),
                .device_name = device_name_,
            };
        }
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> exact_pipeline_ = nil;
    id<MTLComputePipelineState> choose_pipeline_ = nil;
    id<MTLBuffer> samples_buffer_ = nil;
    id<MTLBuffer> tasks_buffer_ = nil;
    id<MTLBuffer> selected_buffer_ = nil;
    id<MTLBuffer> task_rice_parameters_buffer_ = nil;
    id<MTLBuffer> best_buffer_ = nil;
    id<MTLBuffer> best_rice_parameters_buffer_ = nil;
    std::size_t samples_buffer_bytes_ = 0;
    std::size_t tasks_buffer_bytes_ = 0;
    std::size_t selected_buffer_bytes_ = 0;
    std::size_t task_rice_parameters_buffer_bytes_ = 0;
    std::size_t best_buffer_bytes_ = 0;
    std::size_t best_rice_parameters_buffer_bytes_ = 0;
    std::string device_name_;
};

MetalMonoAnalysisSession::MetalMonoAnalysisSession(
    std::optional<std::size_t> requested_device_index)
    : impl_(std::make_unique<Impl>(requested_device_index))
{
}

MetalMonoAnalysisSession::~MetalMonoAnalysisSession() = default;

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, true, nullptr);
}

OpenClMonoBestMethodResult MetalMonoAnalysisSession::run_fixed_constant_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    auto analysis = impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, false, gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
}

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, true, nullptr);
}

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order,
        true, true, nullptr);
}

OpenClMonoBestMethodResult MetalMonoAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    auto analysis = impl_->run_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order,
        true, false, gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_fixed_constant_analysis(samples, plan, max_rice_partition_order);
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_lpc_analysis(samples, plan, max_rice_partition_order);
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_generated_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
}

}  // namespace ldcompress::metal_detail
