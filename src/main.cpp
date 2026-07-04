#include "compressor.h"
#include "flac_codec.h"
#include "hash.h"
#include "lds_codec.h"
#include "opencl_devices.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Options {
    bool overwrite = false;
    bool pack = false;
    bool unpack = false;
    bool container_explicit = false;
    unsigned level = 11;
    ldcompress::CompressionBackend backend = ldcompress::CompressionBackend::CpuLibFlac;
    ldcompress::FlacContainer container = ldcompress::FlacContainer::Ogg;
    std::string input;
    std::string output;
    std::string source;
};

[[noreturn]] void usage(int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out << "Usage:\n"
        << "  ld-compress-ng compress [--backend cpu|native-verbatim|opencl] [--level N] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng verify [--source ORIGINAL.lds] INPUT\n"
        << "  ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng --help\n";
    std::exit(exit_code);
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
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
    if (options.backend == ldcompress::CompressionBackend::NativeVerbatimFlac ||
        options.backend == ldcompress::CompressionBackend::OpenClNativeFlac ||
        options.container == ldcompress::FlacContainer::Native) {
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

Options parse_compress(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--backend") {
            if (++i >= argc) {
                throw std::runtime_error("--backend requires a value");
            }
            const std::string_view backend(argv[i]);
            if (backend == "cpu" || backend == "libflac") {
                options.backend = ldcompress::CompressionBackend::CpuLibFlac;
            } else if (backend == "native-verbatim" || backend == "verbatim") {
                options.backend = ldcompress::CompressionBackend::NativeVerbatimFlac;
                if (!options.container_explicit) {
                    options.container = ldcompress::FlacContainer::Native;
                }
            } else if (backend == "opencl" || backend == "gpu") {
                options.backend = ldcompress::CompressionBackend::OpenClNativeFlac;
                if (!options.container_explicit) {
                    options.container = ldcompress::FlacContainer::Native;
                }
            } else {
                throw std::runtime_error("unknown backend: " + std::string(backend));
            }
        } else if (arg == "--level") {
            if (++i >= argc) {
                throw std::runtime_error("--level requires a value");
            }
            options.level = parse_level(argv[i]);
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

    if (options.backend == ldcompress::CompressionBackend::NativeVerbatimFlac &&
        options.container != ldcompress::FlacContainer::Native) {
        throw std::runtime_error("native-verbatim backend writes native FLAC only");
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

int run_compress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    const ldcompress::CompressionOptions compress_options {
        .backend = options.backend,
        .container = options.container,
        .compression_level = options.level,
        .sample_rate = 40000,
    };
    const auto stats = ldcompress::compress_lds(input, options.output, compress_options);
    std::cerr << "compressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples, " << ldcompress::backend_name(options.backend) << " backend)\n";
    return 0;
}

int run_decompress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);
    std::ofstream output(options.output, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + options.output);
    }

    const auto stats = ldcompress::decompress_flac_to_lds(options.input, output);
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
        return 0;
    }

    std::cout << "OpenCL support: built\n";
    const auto devices = ldcompress::list_opencl_devices();
    if (devices.empty()) {
        std::cout << "No OpenCL devices found\n";
        return 0;
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        std::cout << '[' << i << "] " << device.device_name << '\n'
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
