#include "opencl_backend.h"

#include "opencl_devices.h"

#include <stdexcept>

namespace ldcompress {

ConversionStats compress_lds_to_opencl_native_flac(
    std::istream&,
    const std::string&,
    const OpenClCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("opencl backend writes native FLAC only");
    }

    (void)options.native_stats;
    (void)select_opencl_device(options.device_index);
    throw std::runtime_error("OpenCL compression backend is not implemented yet");
}

}  // namespace ldcompress
