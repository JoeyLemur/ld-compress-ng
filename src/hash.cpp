#include "hash.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ldcompress {
namespace {

std::array<std::uint32_t, 256> make_crc32_table()
{
    std::array<std::uint32_t, 256> table {};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
        table[i] = crc;
    }
    return table;
}

const std::array<std::uint32_t, 256>& crc32_table()
{
    static const auto table = make_crc32_table();
    return table;
}

}  // namespace

void Crc32::update(const void* data, std::uint64_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    const auto& table = crc32_table();
    for (std::uint64_t i = 0; i < size; ++i) {
        state_ = table[(state_ ^ bytes[i]) & 0xffU] ^ (state_ >> 8U);
    }
}

std::uint32_t Crc32::value() const
{
    return state_ ^ 0xffffffffU;
}

std::string Crc32::hex() const
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << value();
    return out.str();
}

FileHash crc32_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + path);
    }

    FileHash hash;
    std::array<char, 64 * 1024> buffer {};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        hash.crc.update(buffer.data(), static_cast<std::uint64_t>(got));
        hash.bytes += static_cast<std::uint64_t>(got);
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read input: " + path);
    }
    return hash;
}

}  // namespace ldcompress
