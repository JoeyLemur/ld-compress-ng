#pragma once

#include "flac_codec.h"
#include "lds_codec.h"

#include <array>
#include <iosfwd>
#include <cstdint>
#include <string>

namespace ldcompress {

enum class CompressionBackend {
    CpuLibFlac,
    NativeVerbatimFlac,
    NativeFixedFlac,
    OpenClNativeFlac,
};

struct NativeCompressionStats {
    std::uint64_t frames = 0;
    std::uint64_t constant_frames = 0;
    std::uint64_t verbatim_frames = 0;
    std::uint64_t fixed_rice_frames = 0;
    std::uint64_t lpc_rice_frames = 0;
    std::uint64_t estimated_subframe_bits = 0;
    std::array<std::uint64_t, 5> fixed_order_counts {};
    std::array<std::uint64_t, 33> lpc_order_counts {};
    std::array<std::uint64_t, 16> partition_order_counts {};
    std::array<std::uint64_t, 33> wasted_bits_counts {};
};

struct CompressionOptions {
    CompressionBackend backend = CompressionBackend::CpuLibFlac;
    FlacContainer container = FlacContainer::Ogg;
    unsigned compression_level = 11;
    unsigned sample_rate = 40000;
    unsigned thread_count = 1;
    unsigned native_frame_samples = 4608;
    unsigned native_max_lpc_order = 12;
    NativeCompressionStats* native_stats = nullptr;
};

const char* backend_name(CompressionBackend backend);

ConversionStats compress_lds(
    std::istream& lds_input,
    const std::string& output_path,
    const CompressionOptions& options);

}  // namespace ldcompress
