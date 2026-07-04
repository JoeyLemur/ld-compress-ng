#pragma once

#include <cstdint>
#include <string>

namespace ldcompress {

class Crc32 {
public:
    void update(const void* data, std::uint64_t size);
    std::uint32_t value() const;
    std::string hex() const;

private:
    std::uint32_t state_ = 0xffffffffU;
};

struct FileHash {
    std::uint64_t bytes = 0;
    Crc32 crc;
};

FileHash crc32_file(const std::string& path);

}  // namespace ldcompress
