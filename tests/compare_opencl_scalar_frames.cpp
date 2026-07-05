#include "flac_native_writer.h"
#include "lds_codec.h"
#include "opencl_analysis.h"
#include "opencl_devices.h"

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kOpenClBatchFrames = 32;

struct Options {
    std::filesystem::path input;
    std::optional<std::size_t> device_index;
    unsigned frame_samples = 4608;
    unsigned max_lpc_order = 12;
    unsigned lpc_precision = 12;
    unsigned max_rice_partition_order = 5;
    std::size_t start_frame = 0;
    std::optional<std::size_t> max_frames;
    std::size_t mismatch_limit = 20;
    bool show_coefficients = false;
};

struct DecisionView {
    std::string kind;
    unsigned order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned coefficient_precision = 0;
    int quantization_shift = 0;
    std::uint64_t estimated_bits = 0;
    std::vector<std::int32_t> coefficients;
};

void usage()
{
    std::cerr
        << "usage: compare_opencl_scalar_frames [options] INPUT.lds\n"
        << "options:\n"
        << "  --device INDEX\n"
        << "  --frame-samples N\n"
        << "  --lpc-order N\n"
        << "  --lpc-precision N\n"
        << "  --rice-partition-order N\n"
        << "  --start-frame N\n"
        << "  --max-frames N\n"
        << "  --mismatch-limit N\n"
        << "  --show-coefficients\n";
}

std::uint64_t parse_u64(std::string_view text, std::string_view name)
{
    if (text.empty()) {
        throw std::runtime_error(std::string(name) + " cannot be empty");
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error(std::string(name) + " must be an unsigned integer: " +
                std::string(text));
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > ((~std::uint64_t {0}) - digit) / 10U) {
            throw std::runtime_error(std::string(name) + " is too large: " + std::string(text));
        }
        value = (value * 10U) + digit;
    }
    return value;
}

unsigned parse_bounded_unsigned(
    std::string_view text,
    std::string_view name,
    unsigned minimum,
    unsigned maximum)
{
    const auto value = parse_u64(text, name);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(name) + " must be " +
            std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return static_cast<unsigned>(value);
}

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto require_value = [&](std::string_view option) -> std::string_view {
            if (++i >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            return argv[i];
        };

        if (arg == "--device" || arg == "--opencl-device") {
            options.device_index = static_cast<std::size_t>(
                parse_u64(require_value(arg), "OpenCL device index"));
        } else if (arg == "--frame-samples") {
            options.frame_samples = parse_bounded_unsigned(
                require_value(arg), "frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            options.max_lpc_order = parse_bounded_unsigned(
                require_value(arg), "LPC order", 0, 12);
        } else if (arg == "--lpc-precision") {
            options.lpc_precision = parse_bounded_unsigned(
                require_value(arg), "LPC precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            options.max_rice_partition_order = parse_bounded_unsigned(
                require_value(arg), "Rice partition order", 0, 8);
        } else if (arg == "--start-frame") {
            options.start_frame = static_cast<std::size_t>(
                parse_u64(require_value(arg), "start frame"));
        } else if (arg == "--max-frames") {
            options.max_frames = static_cast<std::size_t>(
                parse_u64(require_value(arg), "max frames"));
        } else if (arg == "--mismatch-limit") {
            options.mismatch_limit = static_cast<std::size_t>(
                parse_u64(require_value(arg), "mismatch limit"));
        } else if (arg == "--show-coefficients") {
            options.show_coefficients = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else if (options.input.empty()) {
            options.input = std::filesystem::path(arg);
        } else {
            throw std::runtime_error("expected only one input path");
        }
    }

    if (options.input.empty()) {
        throw std::runtime_error("missing input path");
    }
    if (options.frame_samples == 0) {
        throw std::runtime_error("frame sample count must be positive");
    }
    if (options.max_frames.has_value() && *options.max_frames == 0) {
        throw std::runtime_error("max frames must be positive");
    }
    if (options.max_lpc_order == 0) {
        throw std::runtime_error("this diagnostic currently targets the generated-LPC OpenCL path");
    }
    return options;
}

std::vector<std::int32_t> read_lds_samples(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + path.string());
    }

    std::vector<std::int32_t> samples;
    std::vector<std::uint8_t> buffer(5 * 8192);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        samples.reserve(samples.size() + (groups * 4));
        for (std::size_t group = 0; group < groups; ++group) {
            ldcompress::PackedLdsGroup packed;
            std::memcpy(packed.data(), buffer.data() + (group * 5), packed.size());
            const auto unpacked = ldcompress::unpack_group(packed);
            for (const auto sample : unpacked) {
                samples.push_back(sample);
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read input: " + path.string());
    }
    return samples;
}

const char* kind_name(ldcompress::FlacSubframeKind kind)
{
    switch (kind) {
    case ldcompress::FlacSubframeKind::Constant:
        return "constant";
    case ldcompress::FlacSubframeKind::Verbatim:
        return "verbatim";
    case ldcompress::FlacSubframeKind::FixedRice:
        return "fixed";
    case ldcompress::FlacSubframeKind::LpcRice:
        return "lpc";
    }
    return "unknown";
}

unsigned decision_order(const ldcompress::FlacSubframeDecision& decision)
{
    switch (decision.kind) {
    case ldcompress::FlacSubframeKind::FixedRice:
        return decision.fixed_order;
    case ldcompress::FlacSubframeKind::LpcRice:
        return decision.lpc_order;
    case ldcompress::FlacSubframeKind::Constant:
    case ldcompress::FlacSubframeKind::Verbatim:
        return 0;
    }
    return 0;
}

DecisionView make_scalar_view(
    const ldcompress::FlacSubframeDecision& decision,
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info)
{
    DecisionView view {
        .kind = kind_name(decision.kind),
        .order = decision_order(decision),
        .rice_partition_order = decision.rice_partition_order,
        .wasted_bits = decision.wasted_bits,
        .coefficient_precision = 0,
        .quantization_shift = 0,
        .estimated_bits = decision.estimated_bits,
        .coefficients = {},
    };

    if (decision.kind == ldcompress::FlacSubframeKind::LpcRice) {
        const auto lpc = ldcompress::analyze_mono_lpc_order(samples, info, decision.lpc_order);
        if (lpc.has_value()) {
            view.coefficient_precision = lpc->coefficient_precision;
            view.quantization_shift = lpc->quantization_shift;
            view.coefficients = lpc->coefficients;
        }
    }
    return view;
}

DecisionView make_opencl_view(
    const ldcompress::FlacSubframeDecision& decision,
    const ldcompress::FlacSelectedSubframe& selected)
{
    return DecisionView {
        .kind = kind_name(decision.kind),
        .order = decision_order(decision),
        .rice_partition_order = decision.rice_partition_order,
        .wasted_bits = decision.wasted_bits,
        .coefficient_precision = selected.coefficient_precision,
        .quantization_shift = selected.quantization_shift,
        .estimated_bits = decision.estimated_bits,
        .coefficients = selected.coefficients,
    };
}

bool same_decision_shape(const DecisionView& scalar, const DecisionView& opencl)
{
    return scalar.kind == opencl.kind &&
        scalar.order == opencl.order &&
        scalar.rice_partition_order == opencl.rice_partition_order &&
        scalar.wasted_bits == opencl.wasted_bits &&
        scalar.coefficient_precision == opencl.coefficient_precision &&
        scalar.quantization_shift == opencl.quantization_shift;
}

std::string format_coefficients(const std::vector<std::int32_t>& coefficients)
{
    std::string formatted;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        if (i != 0) {
            formatted += ';';
        }
        formatted += std::to_string(coefficients[i]);
    }
    return formatted;
}

std::size_t first_coefficient_mismatch(
    const std::vector<std::int32_t>& scalar,
    const std::vector<std::int32_t>& opencl)
{
    const auto common = std::min(scalar.size(), opencl.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (scalar[i] != opencl[i]) {
            return i;
        }
    }
    return common;
}

std::uint64_t max_abs_coefficient_delta(
    const std::vector<std::int32_t>& scalar,
    const std::vector<std::int32_t>& opencl)
{
    const auto common = std::min(scalar.size(), opencl.size());
    std::uint64_t max_delta = 0;
    for (std::size_t i = 0; i < common; ++i) {
        const auto left = static_cast<std::int64_t>(scalar[i]);
        const auto right = static_cast<std::int64_t>(opencl[i]);
        const auto delta = left > right
            ? static_cast<std::uint64_t>(left - right)
            : static_cast<std::uint64_t>(right - left);
        max_delta = std::max(max_delta, delta);
    }
    return max_delta;
}

std::uint64_t recost_selected_subframe_bits(
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info,
    const ldcompress::FlacSelectedSubframe& selected)
{
    std::ostringstream sink(std::ios::binary);
    const auto recost = ldcompress::write_mono_selected_frame(sink, samples, info, selected);
    return recost.estimated_bits;
}

void print_row(
    std::size_t frame,
    unsigned frame_samples,
    const DecisionView& scalar,
    const DecisionView& opencl,
    bool show_coefficients,
    std::uint64_t opencl_recost_bits)
{
    const auto delta = static_cast<std::int64_t>(opencl.estimated_bits) -
        static_cast<std::int64_t>(scalar.estimated_bits);
    const auto recost_delta = static_cast<std::int64_t>(opencl_recost_bits) -
        static_cast<std::int64_t>(opencl.estimated_bits);
    const auto first_mismatch = first_coefficient_mismatch(
        scalar.coefficients, opencl.coefficients);
    std::cout << frame
              << ',' << (frame * static_cast<std::size_t>(frame_samples))
              << ',' << frame_samples
              << ',' << scalar.kind
              << ',' << scalar.order
              << ',' << scalar.rice_partition_order
              << ',' << scalar.wasted_bits
              << ',' << scalar.coefficient_precision
              << ',' << scalar.quantization_shift
              << ',' << scalar.estimated_bits
              << ',' << opencl.kind
              << ',' << opencl.order
              << ',' << opencl.rice_partition_order
              << ',' << opencl.wasted_bits
              << ',' << opencl.coefficient_precision
              << ',' << opencl.quantization_shift
              << ',' << opencl.estimated_bits
              << ',' << delta
              << ',' << opencl_recost_bits
              << ',' << recost_delta
              << ',' << (scalar.coefficients == opencl.coefficients ? "yes" : "no")
              << ',' << first_mismatch
              << ',' << max_abs_coefficient_delta(scalar.coefficients, opencl.coefficients);
    if (show_coefficients) {
        std::cout << ',' << format_coefficients(scalar.coefficients)
                  << ',' << format_coefficients(opencl.coefficients);
    }
    std::cout << '\n';
}

int run(const Options& options)
{
    const auto selected_device = ldcompress::select_opencl_device(options.device_index);
    std::cerr << "using OpenCL device " << selected_device.flat_index << ": "
              << selected_device.device_name << '\n';

    auto all_samples = read_lds_samples(options.input);
    const auto full_frame_count = all_samples.size() / options.frame_samples;
    if (options.start_frame >= full_frame_count) {
        throw std::runtime_error("start frame is beyond full-frame sample range");
    }

    const auto available_frames = full_frame_count - options.start_frame;
    const auto frame_count = std::min(
        available_frames,
        options.max_frames.value_or(available_frames));
    const auto sample_begin = options.start_frame * static_cast<std::size_t>(options.frame_samples);
    const auto sample_end = sample_begin + (frame_count * static_cast<std::size_t>(options.frame_samples));
    std::vector<std::int32_t> samples(
        all_samples.begin() + static_cast<std::ptrdiff_t>(sample_begin),
        all_samples.begin() + static_cast<std::ptrdiff_t>(sample_end));

    const auto tail_samples = all_samples.size() - (full_frame_count * options.frame_samples);
    std::cerr << "loaded " << all_samples.size() << " samples, comparing "
              << frame_count << " full frames starting at " << options.start_frame;
    if (tail_samples != 0) {
        std::cerr << " (" << tail_samples << " tail samples omitted)";
    }
    std::cerr << '\n';

    ldcompress::FlacFrameInfo info {
        .frame_number = options.start_frame,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
    };

    std::vector<ldcompress::FlacSubframeDecision> opencl_decisions;
    std::vector<ldcompress::FlacSelectedSubframe> opencl_selected;
    opencl_decisions.reserve(frame_count);
    opencl_selected.reserve(frame_count);
    for (std::size_t batch_frame = 0; batch_frame < frame_count;
         batch_frame += kOpenClBatchFrames) {
        const auto batch_count = std::min(kOpenClBatchFrames, frame_count - batch_frame);
        const auto batch_sample_begin = batch_frame * static_cast<std::size_t>(options.frame_samples);
        const auto batch_sample_end = batch_sample_begin +
            (batch_count * static_cast<std::size_t>(options.frame_samples));
        const std::vector<std::int32_t> batch_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_begin),
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_end));

        info.frame_number = options.start_frame + batch_frame;
        auto opencl_result = ldcompress::opencl_detail::analyze_opencl_mono_generated_frames(
            batch_samples, info, options.frame_samples, selected_device.flat_index);
        if (opencl_result.decisions.size() != batch_count ||
            opencl_result.selected_subframes.size() != batch_count) {
            throw std::runtime_error("OpenCL result frame count did not match input batch");
        }
        opencl_decisions.insert(
            opencl_decisions.end(),
            opencl_result.decisions.begin(),
            opencl_result.decisions.end());
        opencl_selected.insert(
            opencl_selected.end(),
            opencl_result.selected_subframes.begin(),
            opencl_result.selected_subframes.end());
    }

    std::cout
        << "frame,sample_offset,sample_count,"
        << "scalar_kind,scalar_order,scalar_partition,scalar_wasted,"
        << "scalar_precision,scalar_shift,scalar_bits,"
        << "opencl_kind,opencl_order,opencl_partition,opencl_wasted,"
        << "opencl_precision,opencl_shift,opencl_bits,delta_bits,"
        << "opencl_recost_bits,opencl_recost_delta_bits,"
        << "coefficients_match,first_coefficient_mismatch,max_abs_coefficient_delta";
    if (options.show_coefficients) {
        std::cout << ",scalar_coefficients,opencl_coefficients";
    }
    std::cout << '\n';

    std::uint64_t scalar_bits = 0;
    std::uint64_t opencl_bits = 0;
    std::uint64_t opencl_recost_bits_total = 0;
    std::size_t shape_mismatches = 0;
    std::size_t bit_mismatches = 0;
    std::size_t opencl_recost_mismatches = 0;
    std::size_t opencl_larger = 0;
    std::size_t opencl_smaller = 0;
    std::size_t coefficient_mismatches = 0;
    std::size_t printed = 0;
    std::optional<std::size_t> first_opencl_larger;

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
        info.frame_number = options.start_frame + frame;

        const auto scalar_decision = ldcompress::analyze_mono_best_frame(frame_samples, info);
        const auto scalar = make_scalar_view(scalar_decision, frame_samples, info);
        const auto opencl = make_opencl_view(
            opencl_decisions[frame],
            opencl_selected[frame]);
        const auto opencl_recost_bits = recost_selected_subframe_bits(
            frame_samples, info, opencl_selected[frame]);

        scalar_bits += scalar.estimated_bits;
        opencl_bits += opencl.estimated_bits;
        opencl_recost_bits_total += opencl_recost_bits;

        const bool shape_differs = !same_decision_shape(scalar, opencl);
        const bool bits_differ = scalar.estimated_bits != opencl.estimated_bits;
        const bool recost_differs = opencl_recost_bits != opencl.estimated_bits;
        if (shape_differs) {
            ++shape_mismatches;
        }
        if (bits_differ) {
            ++bit_mismatches;
        }
        if (recost_differs) {
            ++opencl_recost_mismatches;
        }
        if (scalar.coefficients != opencl.coefficients) {
            ++coefficient_mismatches;
        }
        if (opencl.estimated_bits > scalar.estimated_bits) {
            ++opencl_larger;
            if (!first_opencl_larger.has_value()) {
                first_opencl_larger = options.start_frame + frame;
            }
        } else if (opencl.estimated_bits < scalar.estimated_bits) {
            ++opencl_smaller;
        }

        if ((shape_differs || bits_differ) && printed < options.mismatch_limit) {
            print_row(
                options.start_frame + frame,
                options.frame_samples,
                scalar,
                opencl,
                options.show_coefficients,
                opencl_recost_bits);
            ++printed;
        }
    }

    const auto bit_delta = static_cast<std::int64_t>(opencl_bits) -
        static_cast<std::int64_t>(scalar_bits);
    const auto recost_delta = static_cast<std::int64_t>(opencl_recost_bits_total) -
        static_cast<std::int64_t>(opencl_bits);
    std::cerr << "summary frames=" << frame_count
              << " scalar_bits=" << scalar_bits
              << " opencl_bits=" << opencl_bits
              << " delta_bits=" << bit_delta
              << " opencl_recost_bits=" << opencl_recost_bits_total
              << " opencl_recost_delta_bits=" << recost_delta
              << " shape_mismatches=" << shape_mismatches
              << " bit_mismatches=" << bit_mismatches
              << " opencl_recost_mismatches=" << opencl_recost_mismatches
              << " coefficient_mismatches=" << coefficient_mismatches
              << " opencl_larger=" << opencl_larger
              << " opencl_smaller=" << opencl_smaller;
    if (first_opencl_larger.has_value()) {
        std::cerr << " first_opencl_larger=" << *first_opencl_larger;
    }
    std::cerr << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << "compare_opencl_scalar_frames: " << ex.what() << '\n';
        return 1;
    }
}
