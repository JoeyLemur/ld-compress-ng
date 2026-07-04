#include "flac_native_writer.h"

#include "flac_primitives.h"

#include <ostream>
#include <stdexcept>

namespace ldcompress {
namespace {

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

void write_mono_verbatim_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    if (info.sample_rate > 0xfffff) {
        throw std::runtime_error("invalid FLAC frame sample rate");
    }
    const auto bps_code = bits_per_sample_code(info.bits_per_sample);
    const auto block_code = block_size_code(samples.size());

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
        header.push_back(static_cast<std::uint8_t>(samples.size() - 1U));
    } else {
        const auto coded_size = static_cast<unsigned>(samples.size() - 1U);
        header.push_back(static_cast<std::uint8_t>((coded_size >> 8U) & 0xffU));
        header.push_back(static_cast<std::uint8_t>(coded_size & 0xffU));
    }
    header.push_back(flac_crc8(header.data(), header.size()));
    write_bytes(output, header);

    BitWriter frame_body;
    frame_body.write_bits(0, 1);
    frame_body.write_bits(1, 6);
    frame_body.write_bits(0, 1);
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
        frame_body.write_signed(sample, info.bits_per_sample);
    }
    frame_body.align_zero();

    write_bytes(output, frame_body.bytes());

    std::vector<std::uint8_t> frame_without_footer;
    frame_without_footer.reserve(header.size() + frame_body.bytes().size());
    frame_without_footer.insert(frame_without_footer.end(), header.begin(), header.end());
    frame_without_footer.insert(frame_without_footer.end(), frame_body.bytes().begin(), frame_body.bytes().end());
    const auto crc = flac_crc16(frame_without_footer.data(), frame_without_footer.size());
    write_u16be(output, crc);
}

}  // namespace ldcompress
