#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>

namespace ldcompress {

struct ConversionStats {
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t samples = 0;
};

using PackedLdsGroup = std::array<std::uint8_t, 5>;
using SampleGroup = std::array<std::int16_t, 4>;

SampleGroup unpack_group(PackedLdsGroup bytes);
PackedLdsGroup pack_group(SampleGroup samples);

ConversionStats unpack_lds10_to_s16le(std::istream& input, std::ostream& output);
ConversionStats pack_s16le_to_lds10(std::istream& input, std::ostream& output);

}  // namespace ldcompress
