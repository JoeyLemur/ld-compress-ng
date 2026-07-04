#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace ldcompress {

class Md5 {
public:
    void update(const void* data, std::uint64_t size);
    std::array<std::uint8_t, 16> digest() const;
    std::string hex() const;

private:
    void transform(const std::uint8_t* block);
    void finish();

    std::array<std::uint32_t, 4> state_ {
        0x67452301U,
        0xefcdab89U,
        0x98badcfeU,
        0x10325476U,
    };
    std::array<std::uint8_t, 64> buffer_ {};
    std::uint64_t bytes_ = 0;
    std::size_t buffer_size_ = 0;
    bool finalized_ = false;
};

struct FileDigest {
    std::uint64_t bytes = 0;
    Md5 md5;
};

FileDigest md5_file(const std::string& path);

}  // namespace ldcompress
