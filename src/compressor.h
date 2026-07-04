#pragma once

#include "flac_codec.h"
#include "lds_codec.h"

#include <iosfwd>
#include <string>

namespace ldcompress {

enum class CompressionBackend {
    CpuLibFlac,
    NativeVerbatimFlac,
    NativeFixedFlac,
    OpenClNativeFlac,
};

struct CompressionOptions {
    CompressionBackend backend = CompressionBackend::CpuLibFlac;
    FlacContainer container = FlacContainer::Ogg;
    unsigned compression_level = 11;
    unsigned sample_rate = 40000;
};

const char* backend_name(CompressionBackend backend);

ConversionStats compress_lds(
    std::istream& lds_input,
    const std::string& output_path,
    const CompressionOptions& options);

}  // namespace ldcompress
