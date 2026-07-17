#pragma once

#include "lds_codec.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace ldcompress {

struct FileDigest;

enum class FlacContainer {
    Ogg,
    Native,
};

struct FlacEncodeOptions {
    FlacContainer container = FlacContainer::Ogg;
    unsigned compression_level = 11;
    unsigned sample_rate = 40000;
};

// Called after STREAMINFO and after each decoded FLAC frame. A total of zero
// means the stream did not declare its total sample count.
using DecompressionProgressCallback = std::function<void(
    std::uint64_t decoded_samples,
    std::uint64_t total_samples)>;

ConversionStats compress_lds_to_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const FlacEncodeOptions& options);

ConversionStats decompress_flac_to_lds(
    const std::string& input_path,
    std::ostream& lds_output,
    DecompressionProgressCallback progress_callback = {});

// Decodes sequentially while hashing every compressed input byte read.  This
// is used by verify so it does not need a second full pass over a capture.
ConversionStats decompress_flac_to_lds_with_input_digest(
    const std::string& input_path,
    std::ostream& lds_output,
    FileDigest& input_digest,
    DecompressionProgressCallback progress_callback = {});

FlacContainer detect_flac_container(const std::string& input_path);

}  // namespace ldcompress
