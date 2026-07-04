#include "flac_codec.h"
#include "hash.h"
#include "lds_codec.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    bool overwrite = false;
    bool pack = false;
    bool unpack = false;
    unsigned level = 11;
    ldcompress::FlacContainer container = ldcompress::FlacContainer::Ogg;
    std::string input;
    std::string output;
    std::string source;
};

[[noreturn]] void usage(int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out << "Usage:\n"
        << "  ld-compress-ng compress [--level N] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng verify [--source ORIGINAL.lds] INPUT\n"
        << "  ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]\n"
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
    if (options.container == ldcompress::FlacContainer::Native) {
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
    const ldcompress::Crc32& crc() const { return crc_; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        crc_.update(s, static_cast<std::uint64_t>(count));
        bytes_ += static_cast<std::uint64_t>(count);
        return count;
    }

    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }
        const auto byte = static_cast<unsigned char>(ch);
        crc_.update(&byte, 1);
        ++bytes_;
        return ch;
    }

private:
    ldcompress::Crc32 crc_;
    std::uint64_t bytes_ = 0;
};

int run_compress(const Options& options)
{
    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    ensure_output_allowed(options.output, options.overwrite);
    const ldcompress::FlacEncodeOptions encode_options {
        .container = options.container,
        .compression_level = options.level,
        .sample_rate = 40000,
    };
    const auto stats = ldcompress::compress_lds_to_flac(input, options.output, encode_options);
    std::cerr << "compressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples)\n";
    return 0;
}

int run_decompress(const Options& options)
{
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
    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    ensure_output_allowed(options.output, options.overwrite);
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
    const auto compressed = ldcompress::crc32_file(options.input);

    HashingStreambuf decoded_hash_buffer;
    std::ostream decoded_hash_stream(&decoded_hash_buffer);
    const auto decoded_stats = ldcompress::decompress_flac_to_lds(options.input, decoded_hash_stream);
    decoded_hash_stream.flush();
    if (!decoded_hash_stream) {
        throw std::runtime_error("failed to hash decoded stream");
    }

    std::cout << "compressed crc32 " << compressed.crc.hex()
              << "  " << compressed.bytes << " bytes  " << options.input << '\n';
    std::cout << "decoded    crc32 " << decoded_hash_buffer.crc().hex()
              << "  " << decoded_hash_buffer.bytes() << " bytes  "
              << default_decompress_output(options.input) << '\n';

    if (!options.source.empty()) {
        const auto source = ldcompress::crc32_file(options.source);
        const bool match = source.bytes == decoded_hash_buffer.bytes() &&
            source.crc.value() == decoded_hash_buffer.crc().value();
        std::cout << "source     crc32 " << source.crc.hex()
                  << "  " << source.bytes << " bytes  " << options.source << '\n';
        std::cout << "source comparison: " << (match ? "match" : "mismatch") << '\n';
        return match ? 0 : 1;
    }

    (void)decoded_stats;
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

        throw std::runtime_error("unsupported command for this implementation slice: " +
            std::string(command));
    } catch (const std::exception& ex) {
        std::cerr << "ld-compress-ng: " << ex.what() << '\n';
        return 1;
    }
}
