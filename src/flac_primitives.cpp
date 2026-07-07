#include "flac_primitives.h"

#include <algorithm>
#include <stdexcept>

namespace ldcompress {

void BitWriter::write_bits(std::uint64_t value, unsigned bits)
{
    if (bits > 64) {
        throw std::runtime_error("cannot write more than 64 bits at once");
    }

    while (bits != 0) {
        if ((bit_count_ % 8U) == 0) {
            bytes_.push_back(0);
        }

        const auto byte_offset = static_cast<unsigned>(bit_count_ % 8U);
        const auto available = 8U - byte_offset;
        const auto take = std::min(bits, available);
        const auto shift = bits - take;
        const auto mask = (std::uint64_t {1} << take) - 1U;
        const auto chunk = static_cast<std::uint8_t>((value >> shift) & mask);
        bytes_.back() |= static_cast<std::uint8_t>(chunk << (available - take));
        bit_count_ += take;
        bits -= take;
    }
}

void BitWriter::write_signed(std::int64_t value, unsigned bits)
{
    if (bits == 0 || bits > 64) {
        throw std::runtime_error("signed bit width must be 1..64");
    }

    const auto raw = static_cast<std::uint64_t>(value);
    if (bits == 64) {
        write_bits(raw, bits);
        return;
    }

    const auto mask = (std::uint64_t {1} << bits) - 1U;
    write_bits(raw & mask, bits);
}

void BitWriter::write_unary(unsigned zero_count)
{
    write_zero_bits(zero_count);
    write_bits(1, 1);
}

void BitWriter::align_zero()
{
    const auto remainder = bit_count_ % 8U;
    if (remainder != 0) {
        write_bits(0, static_cast<unsigned>(8U - remainder));
    }
}

void BitWriter::clear()
{
    bytes_.clear();
    bit_count_ = 0;
}

void BitWriter::reserve_bits(std::size_t bits)
{
    bytes_.reserve((bits + 7U) / 8U);
}

bool BitWriter::byte_aligned() const
{
    return (bit_count_ % 8U) == 0;
}

void BitWriter::write_zero_bits(std::size_t bits)
{
    while (bits != 0) {
        if ((bit_count_ % 8U) == 0) {
            bytes_.push_back(0);
        }
        const auto byte_offset = bit_count_ % 8U;
        const auto available = 8U - byte_offset;
        const auto take = std::min<std::size_t>(bits, available);
        bit_count_ += take;
        bits -= take;
    }
}

void FlacCrc8::update(const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc_ ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc_ = (crc_ & 0x80U) != 0
                ? static_cast<std::uint8_t>((crc_ << 1U) ^ 0x07U)
                : static_cast<std::uint8_t>(crc_ << 1U);
        }
    }
}

void FlacCrc16::update(const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc_ ^= static_cast<std::uint16_t>(data[i]) << 8U;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc_ = (crc_ & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc_ << 1U) ^ 0x8005U)
                : static_cast<std::uint16_t>(crc_ << 1U);
        }
    }
}

std::uint8_t flac_crc8(const std::uint8_t* data, std::size_t size)
{
    FlacCrc8 crc;
    crc.update(data, size);
    return crc.value();
}

std::uint16_t flac_crc16(const std::uint8_t* data, std::size_t size)
{
    FlacCrc16 crc;
    crc.update(data, size);
    return crc.value();
}

}  // namespace ldcompress
