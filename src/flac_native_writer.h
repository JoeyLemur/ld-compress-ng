#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace ldcompress {

struct FlacStreamInfo {
    unsigned min_block_size = 0;
    unsigned max_block_size = 0;
    unsigned min_frame_size = 0;
    unsigned max_frame_size = 0;
    unsigned sample_rate = 0;
    unsigned channels = 1;
    unsigned bits_per_sample = 16;
    std::uint64_t total_samples = 0;
    std::array<std::uint8_t, 16> md5 {};
};

struct FlacFrameInfo {
    std::uint64_t frame_number = 0;
    unsigned sample_rate = 0;
    unsigned bits_per_sample = 16;
    unsigned max_lpc_order = 12;
    unsigned lpc_coefficient_precision = 12;
    unsigned max_rice_partition_order = 4;
};

enum class FlacSubframeKind {
    Constant,
    Verbatim,
    FixedRice,
    LpcRice,
};

struct FlacSubframeDecision {
    FlacSubframeKind kind = FlacSubframeKind::Verbatim;
    unsigned fixed_order = 0;
    unsigned lpc_order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    std::uint64_t estimated_bits = 0;
};

void write_native_flac_streaminfo(std::ostream& output, const FlacStreamInfo& info);

FlacSubframeDecision write_mono_verbatim_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_constant_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_fixed_rice_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

}  // namespace ldcompress
