#include "compressor.h"
#include "flac_codec.h"
#include "hash.h"
#include "lds_codec.h"
#include "opencl_devices.h"
#include "vulkan_devices.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef LDCOMPRESS_VERSION
#define LDCOMPRESS_VERSION "unknown"
#endif

namespace {

constexpr unsigned kDefaultNativeFrameSamples = 4608;
constexpr unsigned kDefaultNativeMaxLpcOrder = 12;
constexpr unsigned kDefaultNativeLpcPrecision = 12;
constexpr unsigned kDefaultNativeMaxRicePartitionOrder = 5;

struct Options {
    bool overwrite = false;
    bool pack = false;
    bool unpack = false;
    bool container_explicit = false;
    bool show_stats = false;
    bool bench_include_opencl = false;
    bool bench_include_vulkan = false;
    bool level_explicit = false;
    bool native_frame_samples_explicit = false;
    bool native_max_lpc_order_explicit = false;
    bool native_lpc_precision_explicit = false;
    bool native_max_rice_partition_order_explicit = false;
    bool device_index_explicit = false;
    bool opencl_device_index_explicit = false;
    bool vulkan_device_index_explicit = false;
    unsigned level = 11;
    unsigned threads = 1;
    unsigned native_frame_samples = kDefaultNativeFrameSamples;
    unsigned native_max_lpc_order = kDefaultNativeMaxLpcOrder;
    unsigned native_lpc_precision = kDefaultNativeLpcPrecision;
    unsigned native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder;
    std::optional<std::size_t> device_index;
    std::optional<std::size_t> opencl_device_index;
    std::optional<std::size_t> vulkan_device_index;
    std::vector<unsigned> bench_threads;
    std::vector<unsigned> bench_frame_samples;
    std::vector<unsigned> bench_lpc_orders;
    std::vector<unsigned> bench_lpc_precisions;
    std::vector<unsigned> bench_rice_partition_orders;
    ldcompress::CompressionBackend backend = ldcompress::CompressionBackend::CpuLibFlac;
    ldcompress::FlacContainer container = ldcompress::FlacContainer::Ogg;
    std::string input;
    std::string output;
    std::string source;
};

[[noreturn]] void usage(int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out << "ld-compress-ng compresses LaserDisc RF .lds captures to FLAC-backed .ldf files.\n\n"
        << "Usage:\n"
        << "  ld-compress-ng compress [OPTIONS] INPUT.lds [OUTPUT]\n"
        << "  ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng verify [--source ORIGINAL.lds] INPUT\n"
        << "  ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng bench [--threads 8] [--frame-samples N[,N...]] [--lpc-order N[,N...]] [--lpc-precision N[,N...]] [--rice-partition-order N[,N...]] [--include-opencl] [--include-vulkan] [--device INDEX|--opencl-device INDEX|--vulkan-device INDEX] INPUT\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng --version\n"
        << "  ld-compress-ng --help\n\n"
        << "Common examples:\n"
        << "  ld-compress-ng compress capture.lds\n"
        << "  ld-compress-ng decompress capture.ldf\n"
        << "  ld-compress-ng verify --source capture.lds capture.ldf\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng compress --backend opencl --device 0 capture.lds\n\n"
        << "Commands:\n"
        << "  compress      Compress packed LDS input. Default output is INPUT.ldf for cpu\n"
        << "                and INPUT.flac.ldf for native-fixed/opencl/native-verbatim.\n"
        << "  decompress    Decode Ogg/native FLAC RF input back to packed LDS output.\n"
        << "  verify        Print compressed and decoded MD5; compare with --source when set.\n"
        << "  convert       Convert between packed LDS and signed 16-bit little-endian PCM.\n"
        << "  bench         Compare backend size/speed using temporary output files.\n"
        << "  devices       List OpenCL and Vulkan devices with --device indexes.\n\n"
        << "Compression backends:\n"
        << "  cpu              Default portable Ogg FLAC .ldf backend using libFLAC/libogg.\n"
        << "  native-fixed     Native FLAC .flac.ldf backend with scalar fixed/LPC prediction.\n"
        << "  opencl           Native FLAC .flac.ldf backend using the selected OpenCL device.\n"
        << "  vulkan           Native FLAC .flac.ldf backend using Vulkan compute.\n"
        << "  native-verbatim  Native FLAC .flac.ldf compatibility/debug backend.\n\n"
        << "Compress options:\n"
        << "  --backend cpu|native-verbatim|native-fixed|opencl|vulkan\n"
        << "  --level N                    CPU/libFLAC level, 1..12; default 11.\n"
        << "  --threads N                  Native scalar frame threads; default 1. OpenCL/Vulkan require 1.\n"
        << "  --frame-samples N            Native FLAC block size, 16..4608; default 4608.\n"
        << "  --lpc-order N                Predictive native max LPC order, 0..12; default 12.\n"
        << "  --lpc-precision N            Predictive native LPC precision, 1..15; default 12.\n"
        << "  --rice-partition-order N     Predictive native max Rice partition order, 0..8; default 5.\n"
        << "  --device INDEX               Backend-local OpenCL/Vulkan device index.\n"
        << "  --opencl-device INDEX        Explicit OpenCL device index.\n"
        << "  --vulkan-device INDEX        Explicit Vulkan device index.\n"
        << "  --stats                      Print native backend decision stats and timings.\n"
        << "  --container ogg|flac         cpu can write Ogg or native FLAC; native/opencl write flac.\n"
        << "  --overwrite                  Replace an existing output path.\n\n"
        << "More details: README.md and docs/build-and-testing.md\n";
    std::exit(exit_code);
}

int version()
{
    std::cout << "ld-compress-ng " << LDCOMPRESS_VERSION << '\n';
    return 0;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

bool is_native_flac_backend(ldcompress::CompressionBackend backend)
{
    return backend == ldcompress::CompressionBackend::NativeVerbatimFlac ||
        backend == ldcompress::CompressionBackend::NativeFixedFlac ||
        backend == ldcompress::CompressionBackend::OpenClNativeFlac ||
        backend == ldcompress::CompressionBackend::VulkanNativeFlac;
}

bool is_accelerated_native_backend(ldcompress::CompressionBackend backend)
{
    return backend == ldcompress::CompressionBackend::OpenClNativeFlac ||
        backend == ldcompress::CompressionBackend::VulkanNativeFlac;
}

std::optional<std::size_t> effective_opencl_device_index(const Options& options)
{
    return options.opencl_device_index.has_value()
        ? options.opencl_device_index
        : options.device_index;
}

std::optional<std::size_t> effective_vulkan_device_index(const Options& options)
{
    return options.vulkan_device_index.has_value()
        ? options.vulkan_device_index
        : options.device_index;
}

std::optional<std::size_t> available_opencl_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::opencl_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_opencl_devices()) {
        if (device.available &&
            (!requested_index.has_value() || device.flat_index == *requested_index)) {
            return device.flat_index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> available_vulkan_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::vulkan_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_vulkan_devices();
    if (requested_index.has_value()) {
        if (*requested_index < devices.size()) {
            const auto& device = devices[*requested_index];
            if (device.available && device.shader_int64) {
                return device.index;
            }
        }
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && device.shader_int64 && device.device_type == "discrete-gpu") {
            return device.index;
        }
    }
    for (const auto& device : devices) {
        if (device.available && device.shader_int64 && device.device_type != "cpu") {
            return device.index;
        }
    }

    return std::nullopt;
}

ldcompress::FlacContainer default_container_for_backend(ldcompress::CompressionBackend backend)
{
    return is_native_flac_backend(backend)
        ? ldcompress::FlacContainer::Native
        : ldcompress::FlacContainer::Ogg;
}

std::string default_convert_output(const Options& options)
{
    const std::filesystem::path input_path(options.input);
    if (options.unpack) {
        return input_path.stem().string() + ".s16";
    }
    return input_path.stem().string() + ".lds";
}

std::string default_compress_output(const Options& options)
{
    const std::filesystem::path input_path(options.input);
    if (is_native_flac_backend(options.backend) || options.container == ldcompress::FlacContainer::Native) {
        return input_path.stem().string() + ".flac.ldf";
    }
    return input_path.stem().string() + ".ldf";
}

std::string default_decompress_output(const std::string& input)
{
    const std::filesystem::path input_path(input);
    const auto filename = input_path.filename().string();
    if (ends_with(filename, ".raw.oga")) {
        return filename.substr(0, filename.size() - std::string_view(".raw.oga").size()) + ".lds";
    }
    if (ends_with(filename, ".flac.ldf")) {
        return filename.substr(0, filename.size() - std::string_view(".flac.ldf").size()) + ".lds";
    }
    if (ends_with(filename, ".ldf")) {
        return filename.substr(0, filename.size() - std::string_view(".ldf").size()) + ".lds";
    }
    return input_path.stem().string() + ".lds";
}

void ensure_output_allowed(const std::string& path, bool overwrite)
{
    if (!overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error("output already exists: " + path + " (use --overwrite)");
    }
}

std::filesystem::path normalized_absolute(const std::string& path)
{
    return std::filesystem::absolute(std::filesystem::path(path)).lexically_normal();
}

bool same_or_equivalent_path(const std::string& lhs, const std::string& rhs)
{
    std::error_code ec;
    if (std::filesystem::exists(lhs, ec) && std::filesystem::exists(rhs, ec)) {
        if (std::filesystem::equivalent(lhs, rhs, ec)) {
            return true;
        }
        ec.clear();
    }

    return normalized_absolute(lhs) == normalized_absolute(rhs);
}

void ensure_distinct_input_output(const std::string& input, const std::string& output)
{
    if (same_or_equivalent_path(input, output)) {
        throw std::runtime_error("input and output refer to the same file: " + input);
    }
}

std::filesystem::path make_temporary_output_path(const std::string& output)
{
    const std::filesystem::path output_path(output);
    const auto directory = output_path.parent_path();
    const auto filename = output_path.filename().string();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        auto candidate = directory /
            ("." + filename + ".tmp-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            throw std::runtime_error("could not inspect temporary output path: " +
                candidate.string() + ": " + ec.message());
        }
    }
    throw std::runtime_error("could not allocate temporary output path for: " + output);
}

unsigned parse_level(std::string_view text)
{
    unsigned level = 0;
    if (text.empty()) {
        throw std::runtime_error("empty compression level");
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid compression level: " + std::string(text));
        }
        level = (level * 10U) + static_cast<unsigned>(ch - '0');
    }
    if (level == 0U || level > 12U) {
        throw std::runtime_error("compression level must be 1..12");
    }
    return level;
}

unsigned parse_threads(std::string_view text)
{
    unsigned threads = 0;
    if (text.empty()) {
        throw std::runtime_error("empty thread count");
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid thread count: " + std::string(text));
        }
        threads = (threads * 10U) + static_cast<unsigned>(ch - '0');
    }
    if (threads == 0U || threads > 1024U) {
        throw std::runtime_error("thread count must be 1..1024");
    }
    return threads;
}

unsigned parse_bounded_unsigned(
    std::string_view text,
    std::string_view name,
    unsigned min_value,
    unsigned max_value)
{
    unsigned value = 0;
    if (text.empty()) {
        throw std::runtime_error("empty " + std::string(name));
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid " + std::string(name) + ": " + std::string(text));
        }
        const auto digit = static_cast<unsigned>(ch - '0');
        if (value > (max_value - digit) / 10U) {
            throw std::runtime_error(std::string(name) + " must be " +
                std::to_string(min_value) + ".." + std::to_string(max_value));
        }
        value = (value * 10U) + digit;
    }
    if (value < min_value || value > max_value) {
        throw std::runtime_error(std::string(name) + " must be " +
            std::to_string(min_value) + ".." + std::to_string(max_value));
    }
    return value;
}

std::size_t parse_device_index(std::string_view text)
{
    std::size_t value = 0;
    if (text.empty()) {
        throw std::runtime_error("empty device index");
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid device index: " + std::string(text));
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            throw std::runtime_error("device index is too large: " + std::string(text));
        }
        value = (value * 10U) + digit;
    }
    return value;
}

std::vector<unsigned> parse_thread_list(std::string_view text)
{
    std::vector<unsigned> threads;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string_view item = comma == std::string_view::npos
            ? text.substr(offset)
            : text.substr(offset, comma - offset);
        threads.push_back(parse_threads(item));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return threads;
}

std::vector<unsigned> parse_bounded_unsigned_list(
    std::string_view text,
    std::string_view name,
    unsigned min_value,
    unsigned max_value)
{
    std::vector<unsigned> values;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string_view item = comma == std::string_view::npos
            ? text.substr(offset)
            : text.substr(offset, comma - offset);
        values.push_back(parse_bounded_unsigned(item, name, min_value, max_value));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return values;
}

Options parse_compress(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--stats") {
            options.show_stats = true;
        } else if (arg == "--backend") {
            if (++i >= argc) {
                throw std::runtime_error("--backend requires a value");
            }
            const std::string_view backend(argv[i]);
            if (backend == "cpu" || backend == "libflac") {
                options.backend = ldcompress::CompressionBackend::CpuLibFlac;
            } else if (backend == "native-verbatim" || backend == "verbatim") {
                options.backend = ldcompress::CompressionBackend::NativeVerbatimFlac;
            } else if (backend == "native-fixed" || backend == "fixed-rice") {
                options.backend = ldcompress::CompressionBackend::NativeFixedFlac;
            } else if (backend == "opencl" || backend == "gpu") {
                options.backend = ldcompress::CompressionBackend::OpenClNativeFlac;
            } else if (backend == "vulkan") {
                options.backend = ldcompress::CompressionBackend::VulkanNativeFlac;
            } else {
                throw std::runtime_error("unknown backend: " + std::string(backend));
            }
        } else if (arg == "--level") {
            if (++i >= argc) {
                throw std::runtime_error("--level requires a value");
            }
            options.level_explicit = true;
            options.level = parse_level(argv[i]);
        } else if (arg == "--threads") {
            if (++i >= argc) {
                throw std::runtime_error("--threads requires a value");
            }
            options.threads = parse_threads(argv[i]);
        } else if (arg == "--frame-samples") {
            if (++i >= argc) {
                throw std::runtime_error("--frame-samples requires a value");
            }
            options.native_frame_samples_explicit = true;
            options.native_frame_samples = parse_bounded_unsigned(
                argv[i], "native FLAC frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-order requires a value");
            }
            options.native_max_lpc_order_explicit = true;
            options.native_max_lpc_order = parse_bounded_unsigned(
                argv[i], "native FLAC max LPC order", 0, 12);
        } else if (arg == "--lpc-precision") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-precision requires a value");
            }
            options.native_lpc_precision_explicit = true;
            options.native_lpc_precision = parse_bounded_unsigned(
                argv[i], "native FLAC LPC coefficient precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            if (++i >= argc) {
                throw std::runtime_error("--rice-partition-order requires a value");
            }
            options.native_max_rice_partition_order_explicit = true;
            options.native_max_rice_partition_order = parse_bounded_unsigned(
                argv[i], "native FLAC max Rice partition order", 0, 8);
        } else if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.device_index_explicit = true;
            options.device_index = parse_device_index(argv[i]);
        } else if (arg == "--opencl-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.opencl_device_index_explicit = true;
            options.opencl_device_index = parse_device_index(argv[i]);
        } else if (arg == "--vulkan-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.vulkan_device_index_explicit = true;
            options.vulkan_device_index = parse_device_index(argv[i]);
        } else if (arg == "--container") {
            if (++i >= argc) {
                throw std::runtime_error("--container requires a value");
            }
            const std::string_view container(argv[i]);
            options.container_explicit = true;
            if (container == "ogg") {
                options.container = ldcompress::FlacContainer::Ogg;
            } else if (container == "flac" || container == "native") {
                options.container = ldcompress::FlacContainer::Native;
            } else {
                throw std::runtime_error("unknown container: " + std::string(container));
            }
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("compress expects INPUT and optional OUTPUT");
    }

    if (!options.container_explicit) {
        options.container = default_container_for_backend(options.backend);
    }

    const bool native_predictive_options_explicit =
        options.native_max_lpc_order_explicit ||
        options.native_lpc_precision_explicit ||
        options.native_max_rice_partition_order_explicit;
    const bool native_tuning_options_explicit =
        options.native_frame_samples_explicit || native_predictive_options_explicit;

    if (options.level_explicit &&
        options.backend != ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--level is currently supported only by the cpu backend");
    }
    if (options.threads != 1 &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--threads is currently supported only by native FLAC backends");
    }
    if (options.show_stats &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--stats is currently supported only by native FLAC backends");
    }
    if (options.device_index_explicit && !is_accelerated_native_backend(options.backend)) {
        throw std::runtime_error("--device is currently supported only by opencl and vulkan backends");
    }
    if (options.opencl_device_index_explicit &&
        options.backend != ldcompress::CompressionBackend::OpenClNativeFlac) {
        throw std::runtime_error("--opencl-device is currently supported only by the opencl backend");
    }
    if (options.vulkan_device_index_explicit &&
        options.backend != ldcompress::CompressionBackend::VulkanNativeFlac) {
        throw std::runtime_error("--vulkan-device is currently supported only by the vulkan backend");
    }
    if (options.backend == ldcompress::CompressionBackend::OpenClNativeFlac &&
        options.vulkan_device_index_explicit) {
        throw std::runtime_error("--vulkan-device cannot be used with the opencl backend");
    }
    if (options.backend == ldcompress::CompressionBackend::VulkanNativeFlac &&
        options.opencl_device_index_explicit) {
        throw std::runtime_error("--opencl-device cannot be used with the vulkan backend");
    }
    if (options.backend == ldcompress::CompressionBackend::CpuLibFlac &&
        native_tuning_options_explicit) {
        throw std::runtime_error("--frame-samples, --lpc-order, --lpc-precision, and --rice-partition-order are supported only by native FLAC backends");
    }
    if (options.backend == ldcompress::CompressionBackend::NativeVerbatimFlac &&
        native_predictive_options_explicit) {
        throw std::runtime_error("--lpc-order, --lpc-precision, and --rice-partition-order are supported only by predictive native FLAC backends");
    }

    if (is_native_flac_backend(options.backend) &&
        options.container != ldcompress::FlacContainer::Native) {
        throw std::runtime_error(std::string(ldcompress::backend_name(options.backend)) +
            " backend writes native FLAC only");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_compress_output(options);
    return options;
}

Options parse_decompress(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("decompress expects INPUT and optional OUTPUT");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_decompress_output(options.input);
    return options;
}

Options parse_verify(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--source") {
            if (++i >= argc) {
                throw std::runtime_error("--source requires a value");
            }
            options.source = argv[i];
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.size() != 1) {
        throw std::runtime_error("verify expects exactly one INPUT");
    }

    options.input = positional[0];
    return options;
}

Options parse_convert(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--pack") {
            options.pack = true;
        } else if (arg == "--unpack") {
            options.unpack = true;
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (options.pack == options.unpack) {
        throw std::runtime_error("specify exactly one of --pack or --unpack");
    }
    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("convert expects INPUT and optional OUTPUT");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_convert_output(options);
    return options;
}

Options parse_bench(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--threads") {
            if (++i >= argc) {
                throw std::runtime_error("--threads requires a value");
            }
            options.bench_threads = parse_thread_list(argv[i]);
        } else if (arg == "--frame-samples") {
            if (++i >= argc) {
                throw std::runtime_error("--frame-samples requires a value");
            }
            options.bench_frame_samples = parse_bounded_unsigned_list(
                argv[i], "native FLAC frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-order requires a value");
            }
            options.bench_lpc_orders = parse_bounded_unsigned_list(
                argv[i], "native FLAC max LPC order", 0, 12);
        } else if (arg == "--lpc-precision") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-precision requires a value");
            }
            options.bench_lpc_precisions = parse_bounded_unsigned_list(
                argv[i], "native FLAC LPC coefficient precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            if (++i >= argc) {
                throw std::runtime_error("--rice-partition-order requires a value");
            }
            options.bench_rice_partition_orders = parse_bounded_unsigned_list(
                argv[i], "native FLAC max Rice partition order", 0, 8);
        } else if (arg == "--include-opencl") {
            options.bench_include_opencl = true;
        } else if (arg == "--include-vulkan") {
            options.bench_include_vulkan = true;
        } else if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.device_index_explicit = true;
            options.device_index = parse_device_index(argv[i]);
        } else if (arg == "--opencl-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.opencl_device_index_explicit = true;
            options.opencl_device_index = parse_device_index(argv[i]);
        } else if (arg == "--vulkan-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.vulkan_device_index_explicit = true;
            options.vulkan_device_index = parse_device_index(argv[i]);
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.size() != 1) {
        throw std::runtime_error("bench expects exactly one INPUT");
    }
    if (options.bench_threads.empty()) {
        options.bench_threads.push_back(1);
    }
    if (options.bench_frame_samples.empty()) {
        options.bench_frame_samples.push_back(options.native_frame_samples);
    }
    if (options.bench_lpc_orders.empty()) {
        options.bench_lpc_orders.push_back(options.native_max_lpc_order);
    }
    if (options.bench_lpc_precisions.empty()) {
        options.bench_lpc_precisions.push_back(options.native_lpc_precision);
    }
    if (options.bench_rice_partition_orders.empty()) {
        options.bench_rice_partition_orders.push_back(options.native_max_rice_partition_order);
    }
    if (options.device_index_explicit &&
        !options.bench_include_opencl &&
        !options.bench_include_vulkan) {
        throw std::runtime_error("--device is supported by bench only with --include-opencl or --include-vulkan");
    }
    if (options.device_index_explicit &&
        options.bench_include_opencl &&
        options.bench_include_vulkan) {
        throw std::runtime_error("--device is ambiguous when bench includes both OpenCL and Vulkan; use --opencl-device and --vulkan-device");
    }
    if (options.opencl_device_index_explicit && !options.bench_include_opencl) {
        throw std::runtime_error("--opencl-device is supported by bench only with --include-opencl");
    }
    if (options.vulkan_device_index_explicit && !options.bench_include_vulkan) {
        throw std::runtime_error("--vulkan-device is supported by bench only with --include-vulkan");
    }

    options.input = positional[0];
    return options;
}

class HashingStreambuf final : public std::streambuf {
public:
    std::uint64_t bytes() const { return bytes_; }
    const ldcompress::Md5& md5() const { return md5_; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        md5_.update(s, static_cast<std::uint64_t>(count));
        bytes_ += static_cast<std::uint64_t>(count);
        return count;
    }

    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }
        const auto byte = static_cast<unsigned char>(ch);
        md5_.update(&byte, 1);
        ++bytes_;
        return ch;
    }

private:
    ldcompress::Md5 md5_;
    std::uint64_t bytes_ = 0;
};

template <std::size_t N>
void print_nonzero_counts(
    std::ostream& output,
    std::string_view label,
    const std::array<std::uint64_t, N>& counts)
{
    output << label << ':';
    bool any = false;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) {
            continue;
        }
        output << (any ? ", " : " ") << i << '=' << counts[i];
        any = true;
    }
    if (!any) {
        output << " none";
    }
    output << '\n';
}

template <std::size_t N>
std::string summarize_nonzero_counts(
    const std::array<std::uint64_t, N>& counts,
    std::size_t max_items = 3)
{
    std::vector<std::pair<std::uint64_t, std::size_t>> nonzero;
    nonzero.reserve(counts.size());
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != 0) {
            nonzero.emplace_back(counts[i], i);
        }
    }
    if (nonzero.empty()) {
        return "-";
    }

    std::sort(nonzero.begin(), nonzero.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });

    std::ostringstream out;
    const auto limit = std::min(max_items, nonzero.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            out << ',';
        }
        out << nonzero[i].second << ':' << nonzero[i].first;
    }
    return out.str();
}

std::string summarize_subframes(const ldcompress::NativeCompressionStats& stats)
{
    if (stats.frames == 0) {
        return "-";
    }
    std::ostringstream out;
    out << "c" << stats.constant_frames
        << ",f" << stats.fixed_rice_frames
        << ",l" << stats.lpc_rice_frames
        << ",v" << stats.verbatim_frames;
    return out.str();
}

double seconds_from_ns(std::uint64_t ns)
{
    return static_cast<double>(ns) / 1'000'000'000.0;
}

void print_native_stats(const ldcompress::NativeCompressionStats& stats)
{
    std::cerr << "native stats: frames=" << stats.frames
              << " constant=" << stats.constant_frames
              << " fixed-rice=" << stats.fixed_rice_frames
              << " lpc-rice=" << stats.lpc_rice_frames
              << " verbatim=" << stats.verbatim_frames
              << " estimated-subframe-bits=" << stats.estimated_subframe_bits
              << '\n';
    print_nonzero_counts(std::cerr, "fixed orders", stats.fixed_order_counts);
    print_nonzero_counts(std::cerr, "lpc orders", stats.lpc_order_counts);
    print_nonzero_counts(std::cerr, "partition orders", stats.partition_order_counts);
    print_nonzero_counts(std::cerr, "wasted bits", stats.wasted_bits_counts);
    if (stats.accelerated_batches != 0 || stats.accelerated_total_ns != 0) {
        const auto measured_analyzer_parts =
            stats.accelerated_task_plan_ns + stats.accelerated_exact_analysis_ns;
        const auto analyzer_other_ns = stats.accelerated_analyzer_ns > measured_analyzer_parts
            ? stats.accelerated_analyzer_ns - measured_analyzer_parts
            : 0;
        std::cerr << std::fixed << std::setprecision(6)
                  << "accelerated timings: batches=" << stats.accelerated_batches
                  << " total=" << seconds_from_ns(stats.accelerated_total_ns) << "s"
                  << " scan=" << seconds_from_ns(stats.accelerated_scan_ns) << "s"
                  << " analyzer=" << seconds_from_ns(stats.accelerated_analyzer_ns) << "s"
                  << " selected-write="
                  << seconds_from_ns(stats.accelerated_selected_write_ns) << "s"
                  << " tail-write=" << seconds_from_ns(stats.accelerated_tail_write_ns) << "s"
                  << '\n';
        if (stats.accelerated_task_plan_ns != 0 ||
            stats.accelerated_exact_analysis_ns != 0) {
            std::cerr << "accelerator timings: task-plan="
                      << seconds_from_ns(stats.accelerated_task_plan_ns) << "s"
                      << " exact-analysis="
                      << seconds_from_ns(stats.accelerated_exact_analysis_ns) << "s"
                      << " analyzer-other=" << seconds_from_ns(analyzer_other_ns) << "s"
                      << '\n';
        }
        if (stats.vulkan_gpu_timed_batches != 0) {
            std::cerr << "vulkan gpu timings: batches=" << stats.vulkan_gpu_timed_batches
                      << " total=" << seconds_from_ns(stats.vulkan_gpu_total_ns) << "s"
                      << " upload=" << seconds_from_ns(stats.vulkan_gpu_upload_ns) << "s"
                      << " prepare="
                      << seconds_from_ns(stats.vulkan_gpu_prepare_ns) << "s"
                      << " autocor="
                      << seconds_from_ns(stats.vulkan_gpu_generated_autocorrelation_ns) << "s"
                      << " lpc=" << seconds_from_ns(stats.vulkan_gpu_generated_lpc_ns) << "s"
                      << " quantize="
                      << seconds_from_ns(stats.vulkan_gpu_generated_quantize_ns) << "s"
                      << " exact="
                      << seconds_from_ns(stats.vulkan_gpu_exact_analysis_ns) << "s"
                      << " choose=" << seconds_from_ns(stats.vulkan_gpu_choose_best_ns) << "s"
                      << " readback="
                      << seconds_from_ns(stats.vulkan_gpu_readback_ns) << "s"
                      << '\n';
        }
        std::cerr.unsetf(std::ios::floatfield);
    }
}

int run_compress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    ldcompress::NativeCompressionStats native_stats;
    const ldcompress::CompressionOptions compress_options {
        .backend = options.backend,
        .container = options.container,
        .compression_level = options.level,
        .sample_rate = 40000,
        .thread_count = options.threads,
        .native_frame_samples = options.native_frame_samples,
        .native_max_lpc_order = options.native_max_lpc_order,
        .native_lpc_precision = options.native_lpc_precision,
        .native_max_rice_partition_order = options.native_max_rice_partition_order,
        .native_stats = options.show_stats ? &native_stats : nullptr,
        .opencl_device_index = effective_opencl_device_index(options),
        .vulkan_device_index = effective_vulkan_device_index(options),
    };
    const auto stats = ldcompress::compress_lds(input, options.output, compress_options);
    std::cerr << "compressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples, " << ldcompress::backend_name(options.backend)
              << " backend, " << options.threads << " thread"
              << (options.threads == 1 ? "" : "s") << ")\n";
    if (options.show_stats) {
        print_native_stats(native_stats);
    }
    return 0;
}

int run_decompress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    const auto temp_output = make_temporary_output_path(options.output);
    bool renamed = false;
    ldcompress::ConversionStats stats;
    try {
        {
            std::ofstream output(temp_output, std::ios::binary);
            if (!output) {
                throw std::runtime_error("could not open temporary output: " + temp_output.string());
            }

            stats = ldcompress::decompress_flac_to_lds(options.input, output);
            output.close();
            if (!output) {
                throw std::runtime_error("failed to finish decompressed output: " +
                    temp_output.string());
            }
        }

        ensure_output_allowed(options.output, options.overwrite);
        std::filesystem::rename(temp_output, options.output);
        renamed = true;
    } catch (...) {
        if (!renamed) {
            std::error_code ec;
            std::filesystem::remove(temp_output, ec);
        }
        throw;
    }

    std::cerr << "decompressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples)\n";
    return 0;
}

int run_convert(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    std::ofstream output(options.output, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + options.output);
    }

    const auto stats = options.unpack
        ? ldcompress::unpack_lds10_to_s16le(input, output)
        : ldcompress::pack_s16le_to_lds10(input, output);

    std::cerr << "converted " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples)\n";
    return 0;
}

std::filesystem::path make_bench_temp_dir()
{
    const auto base = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base /
            ("ld-compress-ng-bench-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            throw std::runtime_error("could not create benchmark temp directory: " +
                candidate.string() + ": " + ec.message());
        }
    }
    throw std::runtime_error("could not allocate benchmark temp directory");
}

class ScopedDirectory final {
public:
    explicit ScopedDirectory(std::filesystem::path path) : path_(std::move(path)) {}

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    ~ScopedDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct BenchCase {
    ldcompress::CompressionBackend backend;
    ldcompress::FlacContainer container;
    unsigned threads;
    unsigned native_frame_samples;
    unsigned native_max_lpc_order;
    unsigned native_lpc_precision;
    unsigned native_max_rice_partition_order;
    std::optional<std::size_t> opencl_device_index;
    std::optional<std::size_t> vulkan_device_index;
    bool show_frame_samples;
    bool show_lpc_order;
    bool show_lpc_precision;
    bool show_rice_partition_order;
};

struct BenchResult {
    const char* backend;
    unsigned threads;
    unsigned native_frame_samples;
    unsigned native_max_lpc_order;
    unsigned native_lpc_precision;
    unsigned native_max_rice_partition_order;
    bool show_frame_samples;
    bool show_lpc_order;
    bool show_lpc_precision;
    bool show_rice_partition_order;
    ldcompress::ConversionStats stats;
    ldcompress::NativeCompressionStats native_stats;
    bool show_native_stats;
    double elapsed_seconds;
};

BenchResult run_bench_case(
    const std::string& input_path,
    const std::filesystem::path& output_path,
    const BenchCase& bench_case)
{
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + input_path);
    }

    ldcompress::NativeCompressionStats native_stats;
    const bool collect_native_stats = is_native_flac_backend(bench_case.backend);
    const ldcompress::CompressionOptions compress_options {
        .backend = bench_case.backend,
        .container = bench_case.container,
        .compression_level = 11,
        .sample_rate = 40000,
        .thread_count = bench_case.threads,
        .native_frame_samples = bench_case.native_frame_samples,
        .native_max_lpc_order = bench_case.native_max_lpc_order,
        .native_lpc_precision = bench_case.native_lpc_precision,
        .native_max_rice_partition_order = bench_case.native_max_rice_partition_order,
        .native_stats = collect_native_stats ? &native_stats : nullptr,
        .opencl_device_index = bench_case.opencl_device_index,
        .vulkan_device_index = bench_case.vulkan_device_index,
    };

    const auto started = std::chrono::steady_clock::now();
    const auto stats = ldcompress::compress_lds(input, output_path.string(), compress_options);
    const auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finished - started;

    return BenchResult {
        .backend = ldcompress::backend_name(bench_case.backend),
        .threads = bench_case.threads,
        .native_frame_samples = bench_case.native_frame_samples,
        .native_max_lpc_order = bench_case.native_max_lpc_order,
        .native_lpc_precision = bench_case.native_lpc_precision,
        .native_max_rice_partition_order = bench_case.native_max_rice_partition_order,
        .show_frame_samples = bench_case.show_frame_samples,
        .show_lpc_order = bench_case.show_lpc_order,
        .show_lpc_precision = bench_case.show_lpc_precision,
        .show_rice_partition_order = bench_case.show_rice_partition_order,
        .stats = stats,
        .native_stats = native_stats,
        .show_native_stats = collect_native_stats,
        .elapsed_seconds = elapsed.count(),
    };
}

void print_optional_unsigned(unsigned value, bool show_value, int width)
{
    std::cout << std::right << std::setw(width);
    if (show_value) {
        std::cout << value;
    } else {
        std::cout << '-';
    }
}

void print_bench_result(const BenchResult& result)
{
    const double ratio = result.stats.input_bytes == 0
        ? 0.0
        : static_cast<double>(result.stats.output_bytes) /
            static_cast<double>(result.stats.input_bytes);
    const double mib_per_second = result.elapsed_seconds <= 0.0
        ? 0.0
        : (static_cast<double>(result.stats.input_bytes) / (1024.0 * 1024.0)) /
            result.elapsed_seconds;

    std::cout << std::left << std::setw(17) << result.backend
              << std::right << std::setw(9) << result.threads;
    print_optional_unsigned(result.native_frame_samples, result.show_frame_samples, 15);
    print_optional_unsigned(result.native_max_lpc_order, result.show_lpc_order, 10);
    print_optional_unsigned(result.native_lpc_precision, result.show_lpc_precision, 9);
    print_optional_unsigned(result.native_max_rice_partition_order, result.show_rice_partition_order, 11);
    std::cout << std::setw(14) << result.stats.input_bytes
              << std::setw(15) << result.stats.output_bytes
              << std::setw(12) << result.stats.samples
              << std::setw(10) << std::fixed << std::setprecision(4) << ratio
              << std::setw(11) << std::fixed << std::setprecision(3) << result.elapsed_seconds
              << std::setw(11) << std::fixed << std::setprecision(2) << mib_per_second
              << std::setw(28) << (result.show_native_stats ? summarize_subframes(result.native_stats) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.lpc_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.partition_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.wasted_bits_counts) : "-")
              << '\n';
}

int run_bench(const Options& options)
{
    ScopedDirectory temp_dir(make_bench_temp_dir());

    std::vector<BenchCase> cases {
        {
            .backend = ldcompress::CompressionBackend::CpuLibFlac,
            .container = ldcompress::FlacContainer::Ogg,
            .threads = 1,
            .native_frame_samples = kDefaultNativeFrameSamples,
            .native_max_lpc_order = kDefaultNativeMaxLpcOrder,
            .native_lpc_precision = kDefaultNativeLpcPrecision,
            .native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder,
            .opencl_device_index = std::nullopt,
            .vulkan_device_index = std::nullopt,
            .show_frame_samples = false,
            .show_lpc_order = false,
            .show_lpc_precision = false,
            .show_rice_partition_order = false,
        },
    };

    for (const unsigned frame_samples : options.bench_frame_samples) {
        cases.push_back(BenchCase {
            .backend = ldcompress::CompressionBackend::NativeVerbatimFlac,
            .container = ldcompress::FlacContainer::Native,
            .threads = 1,
            .native_frame_samples = frame_samples,
            .native_max_lpc_order = 0,
            .native_lpc_precision = kDefaultNativeLpcPrecision,
            .native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder,
            .opencl_device_index = std::nullopt,
            .vulkan_device_index = std::nullopt,
            .show_frame_samples = true,
            .show_lpc_order = false,
            .show_lpc_precision = false,
            .show_rice_partition_order = false,
        });
    }

    for (const unsigned frame_samples : options.bench_frame_samples) {
        for (const unsigned lpc_order : options.bench_lpc_orders) {
            for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                    for (const unsigned threads : options.bench_threads) {
                        cases.push_back(BenchCase {
                            .backend = ldcompress::CompressionBackend::NativeFixedFlac,
                            .container = ldcompress::FlacContainer::Native,
                            .threads = threads,
                            .native_frame_samples = frame_samples,
                            .native_max_lpc_order = lpc_order,
                            .native_lpc_precision = lpc_precision,
                            .native_max_rice_partition_order = rice_partition_order,
                            .opencl_device_index = std::nullopt,
                            .vulkan_device_index = std::nullopt,
                            .show_frame_samples = true,
                            .show_lpc_order = true,
                            .show_lpc_precision = true,
                            .show_rice_partition_order = true,
                        });
                    }
                }
            }
        }
    }

    if (options.bench_include_opencl) {
        const auto opencl_device_index =
            available_opencl_device_index(effective_opencl_device_index(options));
        if (opencl_device_index.has_value()) {
            for (const unsigned frame_samples : options.bench_frame_samples) {
                for (const unsigned lpc_order : options.bench_lpc_orders) {
                    for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                        for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                            cases.push_back(BenchCase {
                                .backend = ldcompress::CompressionBackend::OpenClNativeFlac,
                                .container = ldcompress::FlacContainer::Native,
                                .threads = 1,
                                .native_frame_samples = frame_samples,
                                .native_max_lpc_order = lpc_order,
                                .native_lpc_precision = lpc_precision,
                                .native_max_rice_partition_order = rice_partition_order,
                                .opencl_device_index = opencl_device_index,
                                .vulkan_device_index = std::nullopt,
                                .show_frame_samples = true,
                                .show_lpc_order = true,
                                .show_lpc_precision = true,
                                .show_rice_partition_order = true,
                            });
                        }
                    }
                }
            }
        } else {
            const auto requested_opencl_device_index = effective_opencl_device_index(options);
            if (requested_opencl_device_index.has_value()) {
                std::cerr << "bench: requested OpenCL device "
                          << *requested_opencl_device_index
                          << " is not available; omitting opencl rows\n";
            } else {
                std::cerr << "bench: OpenCL requested but no available device was found; omitting opencl rows\n";
            }
        }
    }

    if (options.bench_include_vulkan) {
        const auto vulkan_device_index =
            available_vulkan_device_index(effective_vulkan_device_index(options));
        if (vulkan_device_index.has_value()) {
            for (const unsigned frame_samples : options.bench_frame_samples) {
                for (const unsigned lpc_order : options.bench_lpc_orders) {
                    for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                        for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                            cases.push_back(BenchCase {
                                .backend = ldcompress::CompressionBackend::VulkanNativeFlac,
                                .container = ldcompress::FlacContainer::Native,
                                .threads = 1,
                                .native_frame_samples = frame_samples,
                                .native_max_lpc_order = lpc_order,
                                .native_lpc_precision = lpc_precision,
                                .native_max_rice_partition_order = rice_partition_order,
                                .opencl_device_index = std::nullopt,
                                .vulkan_device_index = vulkan_device_index,
                                .show_frame_samples = true,
                                .show_lpc_order = true,
                                .show_lpc_precision = true,
                                .show_rice_partition_order = true,
                            });
                        }
                    }
                }
            }
        } else {
            const auto requested_vulkan_device_index = effective_vulkan_device_index(options);
            if (requested_vulkan_device_index.has_value()) {
                std::cerr << "bench: requested Vulkan device "
                          << *requested_vulkan_device_index
                          << " is not available or lacks shaderInt64; omitting vulkan rows\n";
            } else {
                std::cerr << "bench: Vulkan requested but no non-CPU device with shaderInt64 was found; omitting vulkan rows\n";
            }
        }
    }

    std::cout << std::left << std::setw(17) << "backend"
              << std::right << std::setw(9) << "threads"
              << std::setw(15) << "frame_samples"
              << std::setw(10) << "lpc_order"
              << std::setw(9) << "lpc_prec"
              << std::setw(11) << "rice_order"
              << std::setw(14) << "input_bytes"
              << std::setw(15) << "output_bytes"
              << std::setw(12) << "samples"
              << std::setw(10) << "ratio"
              << std::setw(11) << "elapsed_s"
              << std::setw(11) << "mib_per_s"
              << std::setw(28) << "subframes"
              << std::setw(24) << "lpc_orders"
              << std::setw(24) << "rice_orders"
              << std::setw(24) << "wasted_bits"
              << '\n';

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto output_path = temp_dir.path() /
            ("case-" + std::to_string(i) +
                (cases[i].container == ldcompress::FlacContainer::Native ? ".flac.ldf" : ".ldf"));
        print_bench_result(run_bench_case(options.input, output_path, cases[i]));
    }

    return 0;
}

int run_verify(const Options& options)
{
    const auto compressed = ldcompress::md5_file(options.input);

    HashingStreambuf decoded_hash_buffer;
    std::ostream decoded_hash_stream(&decoded_hash_buffer);
    const auto decoded_stats = ldcompress::decompress_flac_to_lds(options.input, decoded_hash_stream);
    decoded_hash_stream.flush();
    if (!decoded_hash_stream) {
        throw std::runtime_error("failed to hash decoded stream");
    }

    std::cout << "compressed md5 " << compressed.md5.hex()
              << "  " << compressed.bytes << " bytes  " << options.input << '\n';
    std::cout << "decoded    md5 " << decoded_hash_buffer.md5().hex()
              << "  " << decoded_hash_buffer.bytes() << " bytes  "
              << default_decompress_output(options.input) << '\n';

    if (!options.source.empty()) {
        const auto source = ldcompress::md5_file(options.source);
        const bool match = source.bytes == decoded_hash_buffer.bytes() &&
            source.md5.digest() == decoded_hash_buffer.md5().digest();
        std::cout << "source     md5 " << source.md5.hex()
                  << "  " << source.bytes << " bytes  " << options.source << '\n';
        std::cout << "source comparison: " << (match ? "match" : "mismatch") << '\n';
        return match ? 0 : 1;
    }

    (void)decoded_stats;
    return 0;
}

int run_devices(int argc, char** argv)
{
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            usage(0);
        }
        throw std::runtime_error("unknown option for devices: " + std::string(arg));
    }

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL support: not built\n";
    } else {
        std::cout << "OpenCL support: built\n";
        const auto devices = ldcompress::list_opencl_devices();
        if (devices.empty()) {
            std::cout << "No OpenCL devices found\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                std::cout << '[' << device.flat_index << "] " << device.device_name << '\n'
                          << "    platform/device index: " << device.platform_index << '/'
                          << device.device_index << '\n'
                          << "    type: " << device.type << '\n'
                          << "    available: " << (device.available ? "yes" : "no") << '\n'
                          << "    compute units: " << device.compute_units << '\n'
                          << "    global memory: " << device.global_memory_bytes << " bytes\n"
                          << "    vendor: " << device.device_vendor << '\n'
                          << "    device version: " << device.device_version << '\n'
                          << "    driver version: " << device.driver_version << '\n'
                          << "    platform: " << device.platform_name << '\n'
                          << "    platform vendor: " << device.platform_vendor << '\n'
                          << "    platform version: " << device.platform_version << '\n';
            }
        }
    }

    std::cout << '\n';
    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan support: not built\n";
    } else {
        std::cout << "Vulkan support: built\n";
        const auto devices = ldcompress::list_vulkan_devices();
        if (devices.empty()) {
            std::cout << "No Vulkan devices found\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                std::cout << '[' << device.index << "] " << device.device_name << '\n'
                          << "    type: " << device.device_type << '\n'
                          << "    available: " << (device.available ? "yes" : "no") << '\n'
                          << "    compute queue family: " << device.compute_queue_family_index << '\n'
                          << "    compute queues: " << device.compute_queue_count << '\n'
                          << "    shaderInt64: " << (device.shader_int64 ? "yes" : "no") << '\n'
                          << "    max workgroup invocations: "
                          << device.max_compute_work_group_invocations << '\n'
                          << "    max compute shared memory: "
                          << device.max_compute_shared_memory_bytes << " bytes\n"
                          << "    device-local memory: "
                          << device.device_local_memory_bytes << " bytes\n"
                          << "    vendor: " << device.vendor_name << '\n'
                          << "    vendor id: 0x" << std::hex << std::setw(4)
                          << std::setfill('0') << device.vendor_id
                          << std::dec << std::setfill(' ') << '\n'
                          << "    device id: 0x" << std::hex << std::setw(4)
                          << std::setfill('0') << device.device_id
                          << std::dec << std::setfill(' ') << '\n'
                          << "    api version: " << device.api_version << '\n'
                          << "    driver version: " << device.driver_version << '\n';
            }
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2) {
            usage(2);
        }

        const std::string_view command(argv[1]);
        if (command == "--help" || command == "-h") {
            usage(0);
        }
        if (command == "--version") {
            return version();
        }
        if (command == "compress") {
            return run_compress(parse_compress(argc, argv));
        }
        if (command == "decompress") {
            return run_decompress(parse_decompress(argc, argv));
        }
        if (command == "verify") {
            return run_verify(parse_verify(argc, argv));
        }
        if (command == "convert") {
            return run_convert(parse_convert(argc, argv));
        }
        if (command == "bench") {
            return run_bench(parse_bench(argc, argv));
        }
        if (command == "devices") {
            return run_devices(argc, argv);
        }

        throw std::runtime_error("unsupported command for this implementation slice: " +
            std::string(command));
    } catch (const std::exception& ex) {
        std::cerr << "ld-compress-ng: " << ex.what() << '\n';
        return 1;
    }
}
