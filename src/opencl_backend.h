#pragma once

#include "flac_codec.h"
#include "lds_codec.h"
#include "native_analysis_profile.h"

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>

namespace ldcompress {

struct NativeCompressionStats;

struct OpenClCompressionOptions {
    FlacContainer container = FlacContainer::Native;
    unsigned sample_rate = 40000;
    unsigned thread_count = 1;
    unsigned frame_samples = 4608;
    unsigned max_lpc_order = 12;
    unsigned lpc_precision = 12;
    unsigned max_rice_partition_order = 5;
    NativeAnalysisProfile analysis_profile = NativeAnalysisProfile::Exact;
    std::optional<std::size_t> device_index;
    NativeCompressionStats* native_stats = nullptr;
};

ConversionStats compress_lds_to_opencl_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const OpenClCompressionOptions& options);

}  // namespace ldcompress
