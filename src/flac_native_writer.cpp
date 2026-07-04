#include "flac_native_writer.h"

#include "flac_primitives.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace ldcompress {
namespace {

constexpr unsigned kMaxRicePartitionOrder = 4;

void write_byte(std::ostream& output, std::uint8_t byte)
{
    output.put(static_cast<char>(byte));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC output");
    }
}

void write_bytes(std::ostream& output, const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC output");
    }
}

void write_u16be(std::ostream& output, unsigned value)
{
    write_byte(output, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>(value & 0xffU));
}

void write_u24be(std::ostream& output, unsigned value)
{
    write_byte(output, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>(value & 0xffU));
}

void validate_streaminfo(const FlacStreamInfo& info)
{
    if (info.min_block_size == 0 || info.min_block_size > 65535 ||
        info.max_block_size == 0 || info.max_block_size > 65535 ||
        info.min_block_size > info.max_block_size) {
        throw std::runtime_error("invalid FLAC STREAMINFO block size");
    }
    if (info.min_frame_size > 0xffffff || info.max_frame_size > 0xffffff) {
        throw std::runtime_error("invalid FLAC STREAMINFO frame size");
    }
    if (info.sample_rate > 0xfffff) {
        throw std::runtime_error("invalid FLAC STREAMINFO sample rate");
    }
    if (info.channels == 0 || info.channels > 8) {
        throw std::runtime_error("invalid FLAC STREAMINFO channel count");
    }
    if (info.bits_per_sample < 4 || info.bits_per_sample > 32) {
        throw std::runtime_error("invalid FLAC STREAMINFO bits per sample");
    }
    if (info.total_samples > 0xfffffffffULL) {
        throw std::runtime_error("invalid FLAC STREAMINFO total sample count");
    }
}

void write_utf8_uint(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    if (value < 0x80) {
        output.push_back(static_cast<std::uint8_t>(value));
        return;
    }
    if (value < 0x800) {
        output.push_back(static_cast<std::uint8_t>(0xc0U | (value >> 6U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x10000) {
        output.push_back(static_cast<std::uint8_t>(0xe0U | (value >> 12U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x200000) {
        output.push_back(static_cast<std::uint8_t>(0xf0U | (value >> 18U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x4000000) {
        output.push_back(static_cast<std::uint8_t>(0xf8U | (value >> 24U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x80000000ULL) {
        output.push_back(static_cast<std::uint8_t>(0xfcU | (value >> 30U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 24U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x1000000000ULL) {
        output.push_back(static_cast<std::uint8_t>(0xfeU));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 30U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 24U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    throw std::runtime_error("FLAC UTF-8 coded number is too large");
}

std::uint8_t block_size_code(std::size_t block_size)
{
    if (block_size == 0 || block_size > 65536) {
        throw std::runtime_error("invalid FLAC frame block size");
    }
    if (block_size <= 256) {
        return 6;
    }
    return 7;
}

std::uint8_t bits_per_sample_code(unsigned bits_per_sample)
{
    switch (bits_per_sample) {
    case 8:
        return 1;
    case 12:
        return 2;
    case 16:
        return 4;
    case 20:
        return 5;
    case 24:
        return 6;
    case 32:
        return 7;
    default:
        throw std::runtime_error("unsupported FLAC frame bits per sample");
    }
}

void validate_sample(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("sample is outside the selected FLAC bit depth");
    }
}

std::int64_t fixed_prediction_residual(
    const std::vector<std::int32_t>& samples,
    std::size_t index,
    unsigned order)
{
    switch (order) {
    case 0:
        return samples[index];
    case 1:
        return static_cast<std::int64_t>(samples[index]) - samples[index - 1];
    case 2:
        return static_cast<std::int64_t>(samples[index]) -
            (2LL * samples[index - 1]) + samples[index - 2];
    case 3:
        return static_cast<std::int64_t>(samples[index]) -
            (3LL * samples[index - 1]) + (3LL * samples[index - 2]) -
            samples[index - 3];
    case 4:
        return static_cast<std::int64_t>(samples[index]) -
            (4LL * samples[index - 1]) + (6LL * samples[index - 2]) -
            (4LL * samples[index - 3]) + samples[index - 4];
    default:
        throw std::runtime_error("unsupported FLAC fixed predictor order");
    }
}

std::uint64_t fold_signed_residual(std::int64_t residual)
{
    if (residual >= 0) {
        return static_cast<std::uint64_t>(residual) << 1U;
    }
    return (static_cast<std::uint64_t>(-(residual + 1)) << 1U) + 1U;
}

std::vector<std::int64_t> fixed_residuals(
    const std::vector<std::int32_t>& samples,
    unsigned order)
{
    std::vector<std::int64_t> residuals;
    residuals.reserve(samples.size() - order);
    for (std::size_t i = order; i < samples.size(); ++i) {
        residuals.push_back(fixed_prediction_residual(samples, i, order));
    }
    return residuals;
}

std::uint64_t rice_bits(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count,
    unsigned parameter)
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto residual = residuals.at(offset + i);
        const auto folded = fold_signed_residual(residual);
        bits += (folded >> parameter) + 1U + parameter;
    }
    return bits;
}

unsigned choose_rice_parameter(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count)
{
    unsigned best_parameter = 0;
    auto best_bits = std::numeric_limits<std::uint64_t>::max();
    for (unsigned parameter = 0; parameter <= 14; ++parameter) {
        const auto bits = rice_bits(residuals, offset, count, parameter);
        if (bits < best_bits) {
            best_bits = bits;
            best_parameter = parameter;
        }
    }
    return best_parameter;
}

struct FixedRiceSubframe {
    unsigned order = 0;
    unsigned partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned effective_bits_per_sample = 0;
    std::vector<std::int32_t> shifted_samples;
    std::vector<unsigned> rice_parameters;
    std::vector<std::int64_t> residuals;
    std::uint64_t bits = 0;
};

bool all_samples_equal(const std::vector<std::int32_t>& samples)
{
    return std::all_of(samples.begin(), samples.end(), [&](std::int32_t sample) {
        return sample == samples.front();
    });
}

std::uint64_t verbatim_subframe_bits(std::size_t block_size, unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + (static_cast<std::uint64_t>(block_size) * bits_per_sample);
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + bits_per_sample;
}

std::uint64_t subframe_wasted_bits_overhead(unsigned wasted_bits)
{
    return wasted_bits == 0 ? 0 : wasted_bits;
}

std::uint64_t verbatim_subframe_bits(
    std::size_t block_size,
    unsigned bits_per_sample,
    unsigned wasted_bits)
{
    return verbatim_subframe_bits(block_size, bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample, unsigned wasted_bits)
{
    return constant_subframe_bits(bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

unsigned sample_trailing_zero_bits(std::int32_t sample, unsigned bits_per_sample)
{
    if (sample == 0) {
        return bits_per_sample;
    }

    const auto mask = bits_per_sample == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : ((std::uint64_t {1} << bits_per_sample) - 1U);
    auto raw = static_cast<std::uint64_t>(sample) & mask;

    unsigned zeros = 0;
    while ((raw & 1U) == 0U && zeros < bits_per_sample) {
        raw >>= 1U;
        ++zeros;
    }
    return zeros;
}

unsigned common_wasted_bits(
    const std::vector<std::int32_t>& samples,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (const auto sample : samples) {
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
        if (wasted_bits == 0) {
            return 0;
        }
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

std::vector<std::int32_t> shift_samples(
    const std::vector<std::int32_t>& samples,
    unsigned wasted_bits)
{
    std::vector<std::int32_t> shifted;
    shifted.reserve(samples.size());
    if (wasted_bits == 0) {
        shifted = samples;
        return shifted;
    }

    const auto divisor = std::int64_t {1} << wasted_bits;
    for (const auto sample : samples) {
        shifted.push_back(static_cast<std::int32_t>(
            static_cast<std::int64_t>(sample) / divisor));
    }
    return shifted;
}

bool valid_partition_order(std::size_t block_size, unsigned predictor_order, unsigned partition_order)
{
    const auto partition_count = std::size_t {1} << partition_order;
    if ((block_size % partition_count) != 0) {
        return false;
    }
    const auto partition_samples = block_size / partition_count;
    return partition_samples > predictor_order;
}

std::size_t partition_residual_count(
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order,
    std::size_t partition)
{
    const auto partition_samples = block_size >> partition_order;
    return partition == 0
        ? partition_samples - predictor_order
        : partition_samples;
}

std::uint64_t fixed_rice_subframe_bits(
    unsigned order,
    unsigned partition_order,
    unsigned wasted_bits,
    const std::vector<unsigned>& rice_parameters,
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    constexpr unsigned kRiceMethodBits = 2;
    constexpr unsigned kPartitionOrderBits = 4;
    constexpr unsigned kRiceParameterBits = 4;

    std::uint64_t bits =
        kSubframeHeaderBits + (static_cast<std::uint64_t>(order) * bits_per_sample) +
        kRiceMethodBits + kPartitionOrderBits + subframe_wasted_bits_overhead(wasted_bits);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            block_size, order, partition_order, partition);
        bits += kRiceParameterBits;
        bits += rice_bits(residuals, residual_offset, residual_count,
            rice_parameters.at(partition));
        residual_offset += residual_count;
    }
    if (residual_offset != residuals.size()) {
        throw std::runtime_error("internal FLAC residual partition accounting error");
    }
    return bits;
}

FixedRiceSubframe choose_fixed_rice_subframe(
    const std::vector<std::int32_t>& samples,
    unsigned bits_per_sample)
{
    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;

    const auto max_order = shifted_samples.size() > 1
        ? std::min<std::size_t>(4, shifted_samples.size() - 1)
        : 0;

    FixedRiceSubframe best;
    auto best_bits = std::numeric_limits<std::uint64_t>::max();
    for (unsigned order = 0; order <= max_order; ++order) {
        auto residuals = fixed_residuals(shifted_samples, order);
        for (unsigned partition_order = 0; partition_order <= kMaxRicePartitionOrder; ++partition_order) {
            if (!valid_partition_order(shifted_samples.size(), order, partition_order)) {
                continue;
            }

            const auto partition_count = std::size_t {1} << partition_order;
            std::vector<unsigned> rice_parameters;
            rice_parameters.reserve(partition_count);
            std::size_t residual_offset = 0;
            for (std::size_t partition = 0; partition < partition_count; ++partition) {
                const auto residual_count = partition_residual_count(
                    shifted_samples.size(), order, partition_order, partition);
                rice_parameters.push_back(choose_rice_parameter(
                    residuals, residual_offset, residual_count));
                residual_offset += residual_count;
            }
            if (residual_offset != residuals.size()) {
                throw std::runtime_error("internal FLAC residual partition accounting error");
            }

            const auto bits = fixed_rice_subframe_bits(
                order, partition_order, wasted_bits, rice_parameters, residuals,
                shifted_samples.size(), effective_bits_per_sample);
            if (bits < best_bits) {
                best_bits = bits;
                best.order = order;
                best.partition_order = partition_order;
                best.wasted_bits = wasted_bits;
                best.effective_bits_per_sample = effective_bits_per_sample;
                best.shifted_samples = shifted_samples;
                best.rice_parameters = std::move(rice_parameters);
                best.residuals = residuals;
                best.bits = bits;
            }
        }
    }
    return best;
}

void write_subframe_header(BitWriter& output, unsigned type, unsigned wasted_bits)
{
    output.write_bits(0, 1);
    output.write_bits(type, 6);
    if (wasted_bits == 0) {
        output.write_bits(0, 1);
        return;
    }

    output.write_bits(1, 1);
    output.write_unary(wasted_bits - 1U);
}

void write_rice_signed(BitWriter& output, std::int64_t residual, unsigned parameter)
{
    const auto folded = fold_signed_residual(residual);
    const auto quotient = folded >> parameter;
    if (quotient > std::numeric_limits<unsigned>::max()) {
        throw std::runtime_error("Rice-coded residual quotient is too large");
    }
    output.write_unary(static_cast<unsigned>(quotient));
    if (parameter != 0) {
        output.write_bits(folded, parameter);
    }
}

void write_frame_with_body(
    std::ostream& output,
    std::size_t block_size,
    const FlacFrameInfo& info,
    BitWriter& frame_body)
{
    if (block_size == 0) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    if (info.sample_rate > 0xfffff) {
        throw std::runtime_error("invalid FLAC frame sample rate");
    }
    const auto bps_code = bits_per_sample_code(info.bits_per_sample);
    const auto block_code = block_size_code(block_size);

    std::vector<std::uint8_t> header;
    BitWriter header_bits;
    header_bits.write_bits(0x3ffe, 14);
    header_bits.write_bits(0, 1);
    header_bits.write_bits(0, 1);
    header_bits.write_bits(block_code, 4);
    // Use the STREAMINFO sample rate; this keeps 40 kHz files out of the
    // sample-rate extension path while the native writer is still minimal.
    header_bits.write_bits(0, 4);
    header_bits.write_bits(0, 4);
    header_bits.write_bits(bps_code, 3);
    header_bits.write_bits(0, 1);
    header_bits.align_zero();
    header = header_bits.bytes();
    write_utf8_uint(header, info.frame_number);
    if (block_code == 6) {
        header.push_back(static_cast<std::uint8_t>(block_size - 1U));
    } else {
        const auto coded_size = static_cast<unsigned>(block_size - 1U);
        header.push_back(static_cast<std::uint8_t>((coded_size >> 8U) & 0xffU));
        header.push_back(static_cast<std::uint8_t>(coded_size & 0xffU));
    }
    header.push_back(flac_crc8(header.data(), header.size()));
    write_bytes(output, header);

    frame_body.align_zero();
    write_bytes(output, frame_body.bytes());

    std::vector<std::uint8_t> frame_without_footer;
    frame_without_footer.reserve(header.size() + frame_body.bytes().size());
    frame_without_footer.insert(frame_without_footer.end(), header.begin(), header.end());
    frame_without_footer.insert(frame_without_footer.end(), frame_body.bytes().begin(), frame_body.bytes().end());
    const auto crc = flac_crc16(frame_without_footer.data(), frame_without_footer.size());
    write_u16be(output, crc);
}

FlacSubframeDecision write_fixed_rice_frame(
    std::ostream& output,
    const FlacFrameInfo& info,
    const FixedRiceSubframe& subframe)
{
    BitWriter frame_body;
    write_subframe_header(frame_body, 0x08U + subframe.order, subframe.wasted_bits);
    for (unsigned i = 0; i < subframe.order; ++i) {
        frame_body.write_signed(subframe.shifted_samples[i],
            subframe.effective_bits_per_sample);
    }
    frame_body.write_bits(0, 2);
    frame_body.write_bits(subframe.partition_order, 4);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << subframe.partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            subframe.shifted_samples.size(), subframe.order,
            subframe.partition_order, partition);
        const auto rice_parameter = subframe.rice_parameters.at(partition);
        frame_body.write_bits(rice_parameter, 4);
        for (std::size_t i = 0; i < residual_count; ++i) {
            write_rice_signed(frame_body, subframe.residuals.at(residual_offset + i),
                rice_parameter);
        }
        residual_offset += residual_count;
    }
    if (residual_offset != subframe.residuals.size()) {
        throw std::runtime_error("internal FLAC residual partition accounting error");
    }
    write_frame_with_body(output, subframe.shifted_samples.size(), info, frame_body);
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::FixedRice,
        .fixed_order = subframe.order,
        .rice_partition_order = subframe.partition_order,
        .wasted_bits = subframe.wasted_bits,
        .estimated_bits = subframe.bits,
    };
}

}  // namespace

void write_native_flac_streaminfo(std::ostream& output, const FlacStreamInfo& info)
{
    validate_streaminfo(info);

    output.write("fLaC", 4);
    if (!output) {
        throw std::runtime_error("failed to write native FLAC marker");
    }

    write_byte(output, 0x80U);
    write_u24be(output, 34);
    write_u16be(output, info.min_block_size);
    write_u16be(output, info.max_block_size);
    write_u24be(output, info.min_frame_size);
    write_u24be(output, info.max_frame_size);

    BitWriter streaminfo_bits;
    streaminfo_bits.write_bits(info.sample_rate, 20);
    streaminfo_bits.write_bits(info.channels - 1U, 3);
    streaminfo_bits.write_bits(info.bits_per_sample - 1U, 5);
    streaminfo_bits.write_bits(info.total_samples, 36);
    streaminfo_bits.align_zero();
    write_bytes(output, streaminfo_bits.bytes());

    output.write(reinterpret_cast<const char*>(info.md5.data()),
        static_cast<std::streamsize>(info.md5.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC STREAMINFO MD5");
    }
}

FlacSubframeDecision write_mono_verbatim_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;

    BitWriter frame_body;
    write_subframe_header(frame_body, 1, wasted_bits);
    for (const auto sample : shifted_samples) {
        validate_sample(sample, effective_bits_per_sample);
        frame_body.write_signed(sample, effective_bits_per_sample);
    }
    write_frame_with_body(output, samples.size(), info, frame_body);
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Verbatim,
        .fixed_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = verbatim_subframe_bits(
            samples.size(), info.bits_per_sample, wasted_bits),
    };
}

FlacSubframeDecision write_mono_constant_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    if (!all_samples_equal(samples)) {
        throw std::runtime_error("constant FLAC subframe received varying samples");
    }
    validate_sample(samples.front(), info.bits_per_sample);

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;

    BitWriter frame_body;
    write_subframe_header(frame_body, 0, wasted_bits);
    frame_body.write_signed(shifted_samples.front(), effective_bits_per_sample);
    write_frame_with_body(output, samples.size(), info, frame_body);
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Constant,
        .fixed_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = constant_subframe_bits(info.bits_per_sample, wasted_bits),
    };
}

FlacSubframeDecision write_mono_fixed_rice_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    const auto subframe = choose_fixed_rice_subframe(samples, info.bits_per_sample);
    return write_fixed_rice_frame(output, info, subframe);
}

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    if (all_samples_equal(samples)) {
        return write_mono_constant_frame(output, samples, info);
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto fixed = choose_fixed_rice_subframe(samples, info.bits_per_sample);
    const auto verbatim_bits = verbatim_subframe_bits(
        samples.size(), info.bits_per_sample, wasted_bits);
    if (fixed.bits < verbatim_bits) {
        return write_fixed_rice_frame(output, info, fixed);
    }
    return write_mono_verbatim_frame(output, samples, info);
}

}  // namespace ldcompress
