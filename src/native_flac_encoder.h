#pragma once

#include "compressor.h"
#include "lds_codec.h"

#include <iosfwd>
#include <string>

namespace ldcompress {

ConversionStats compress_lds_to_native_verbatim_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    NativeCompressionStats* stats = nullptr);

ConversionStats compress_lds_to_native_fixed_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    NativeCompressionStats* stats = nullptr);

}  // namespace ldcompress
