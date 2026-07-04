#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ldcompress {

struct OpenClDeviceInfo {
    std::string platform_name;
    std::string platform_vendor;
    std::string platform_version;
    std::string device_name;
    std::string device_vendor;
    std::string device_version;
    std::string driver_version;
    std::string type;
    std::uint32_t compute_units = 0;
    std::uint64_t global_memory_bytes = 0;
    bool available = false;
};

bool opencl_support_built();
std::vector<OpenClDeviceInfo> list_opencl_devices();

}  // namespace ldcompress
