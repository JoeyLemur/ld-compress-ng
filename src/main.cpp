#include "compressor.h"
#include "flac_codec.h"
#include "hash.h"
#include "lds_codec.h"
#include "opencl_devices.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Options {
    bool overwrite = false;
    bool pack = false;
    bool unpack = false;
    bool container_explicit = false;
    bool show_stats = false;
    unsigned level = 11;
    unsigned threads = 1;
    unsigned native_frame_samples = 4608;
    unsigned native_max_lpc_order = 8;
    std::vector<unsigned> bench_threads;
    std::vector<unsigned> bench_frame_samples;
    std::vector<unsigned> bench_lpc_orders;
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
        << "  ld-compress-ng compress [--backend cpu|native-verbatim|native-fixed|opencl] [--level N] [--threads N] [--frame-samples N] [--lpc-order N] [--stats] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng verify [--source ORIGINAL.lds] INPUT\n"
        << "  ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng bench [--threads 1,4,8] [--frame-samples N[,N...]] [--lpc-order N[,N...]] INPUT\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng --help\n";
    std::exit(exit_code);
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
        backend == ldcompress::CompressionBackend::OpenClNativeFlac;
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
            } else {
                throw std::runtime_error("unknown backend: " + std::string(backend));
            }
        } else if (arg == "--level") {
            if (++i >= argc) {
                throw std::runtime_error("--level requires a value");
            }
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
            options.native_frame_samples = parse_bounded_unsigned(
                argv[i], "native FLAC frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-order requires a value");
            }
            options.native_max_lpc_order = parse_bounded_unsigned(
                argv[i], "native FLAC max LPC order", 0, 12);
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

    if (options.threads != 1 &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--threads is currently supported only by native FLAC backends");
    }
    if (options.show_stats &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--stats is currently supported only by native FLAC backends");
    }
    if (options.backend == ldcompress::CompressionBackend::CpuLibFlac &&
        (options.native_frame_samples != 4608 || options.native_max_lpc_order != 8)) {
        throw std::runtime_error("--frame-samples and --lpc-order are supported only by native FLAC backends");
    }

    if ((options.backend == ldcompress::CompressionBackend::NativeVerbatimFlac ||
            options.backend == ldcompress::CompressionBackend::NativeFixedFlac) &&
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
        .native_stats = options.show_stats ? &native_stats : nullptr,
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
    bool show_frame_samples;
    bool show_lpc_order;
};

struct BenchResult {
    const char* backend;
    unsigned threads;
    unsigned native_frame_samples;
    unsigned native_max_lpc_order;
    bool show_frame_samples;
    bool show_lpc_order;
    ldcompress::ConversionStats stats;
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

    const ldcompress::CompressionOptions compress_options {
        .backend = bench_case.backend,
        .container = bench_case.container,
        .compression_level = 11,
        .sample_rate = 40000,
        .thread_count = bench_case.threads,
        .native_frame_samples = bench_case.native_frame_samples,
        .native_max_lpc_order = bench_case.native_max_lpc_order,
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
        .show_frame_samples = bench_case.show_frame_samples,
        .show_lpc_order = bench_case.show_lpc_order,
        .stats = stats,
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
    std::cout << std::setw(14) << result.stats.input_bytes
              << std::setw(15) << result.stats.output_bytes
              << std::setw(12) << result.stats.samples
              << std::setw(10) << std::fixed << std::setprecision(4) << ratio
              << std::setw(11) << std::fixed << std::setprecision(3) << result.elapsed_seconds
              << std::setw(11) << std::fixed << std::setprecision(2) << mib_per_second
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
            .native_frame_samples = 4608,
            .native_max_lpc_order = 8,
            .show_frame_samples = false,
            .show_lpc_order = false,
        },
    };

    for (const unsigned frame_samples : options.bench_frame_samples) {
        cases.push_back(BenchCase {
            .backend = ldcompress::CompressionBackend::NativeVerbatimFlac,
            .container = ldcompress::FlacContainer::Native,
            .threads = 1,
            .native_frame_samples = frame_samples,
            .native_max_lpc_order = 0,
            .show_frame_samples = true,
            .show_lpc_order = false,
        });
    }

    for (const unsigned frame_samples : options.bench_frame_samples) {
        for (const unsigned lpc_order : options.bench_lpc_orders) {
            for (const unsigned threads : options.bench_threads) {
                cases.push_back(BenchCase {
                    .backend = ldcompress::CompressionBackend::NativeFixedFlac,
                    .container = ldcompress::FlacContainer::Native,
                    .threads = threads,
                    .native_frame_samples = frame_samples,
                    .native_max_lpc_order = lpc_order,
                    .show_frame_samples = true,
                    .show_lpc_order = true,
                });
            }
        }
    }

    std::cout << std::left << std::setw(17) << "backend"
              << std::right << std::setw(9) << "threads"
              << std::setw(15) << "frame_samples"
              << std::setw(10) << "lpc_order"
              << std::setw(14) << "input_bytes"
              << std::setw(15) << "output_bytes"
              << std::setw(12) << "samples"
              << std::setw(10) << "ratio"
              << std::setw(11) << "elapsed_s"
              << std::setw(11) << "mib_per_s"
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
