#include "opencl_devices.h"

#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const char* message)
{
    try {
        fn();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_opencl_device_selection()
{
    const auto devices = ldcompress::list_opencl_devices();
    if (!ldcompress::opencl_support_built()) {
        require(devices.empty(), "OpenCL-disabled build returned devices");
        require_throws([] {
            (void)ldcompress::select_opencl_device(std::nullopt);
        }, "OpenCL-disabled build selected a default device");
        require_throws([] {
            (void)ldcompress::select_opencl_device(0);
        }, "OpenCL-disabled build selected an explicit device");
        return;
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        require(devices[i].flat_index == i, "OpenCL flattened device index mismatch");
    }

    if (devices.empty()) {
        require_throws([] {
            (void)ldcompress::select_opencl_device(std::nullopt);
        }, "empty OpenCL device list selected a default device");
        require_throws([] {
            (void)ldcompress::select_opencl_device(0);
        }, "empty OpenCL device list selected an explicit device");
        return;
    }

    bool saw_available = false;
    std::size_t first_available = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].available) {
            if (!saw_available) {
                first_available = i;
                saw_available = true;
            }
            const auto selected = ldcompress::select_opencl_device(i);
            require(selected.flat_index == i, "explicit OpenCL device selection returned wrong device");
        } else {
            require_throws([i] {
                (void)ldcompress::select_opencl_device(i);
            }, "unavailable OpenCL device was selected");
        }
    }

    if (saw_available) {
        const auto selected = ldcompress::select_opencl_device(std::nullopt);
        require(selected.flat_index == first_available,
            "default OpenCL device selection did not use first available device");
    } else {
        require_throws([] {
            (void)ldcompress::select_opencl_device(std::nullopt);
        }, "OpenCL default selection succeeded with no available devices");
    }

    require_throws([&] {
        (void)ldcompress::select_opencl_device(devices.size());
    }, "out-of-range OpenCL device index was selected");
}

}  // namespace

int main()
{
    try {
        test_opencl_device_selection();
    } catch (const std::exception& ex) {
        std::cerr << "test_opencl_devices: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
