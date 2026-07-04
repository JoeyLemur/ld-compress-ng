#pragma once

#include "lds_codec.h"

#include <cstdint>
#include <iosfwd>
#include <string>

namespace ldcompress {

enum class FlacContainer {
    Ogg,
    Native,
};

struct FlacEncodeOptions {
    FlacContainer container = FlacContainer::Ogg;
    unsigned compression_level = 11;
    unsigned sample_rate = 40000;
};

ConversionStats compress_lds_to_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const FlacEncodeOptions& options);

ConversionStats decompress_flac_to_lds(
    const std::string& input_path,
    std::ostream& lds_output);

FlacContainer detect_flac_container(const std::string& input_path);

}  // namespace ldcompress
