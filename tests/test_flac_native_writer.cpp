#include "flac_codec.h"
#include "flac_native_writer.h"
#include "hash.h"
#include "lds_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::int32_t> make_samples()
{
    std::vector<std::int32_t> samples;
    for (int i = 0; i < 16; ++i) {
        samples.push_back((((i * 7) % 1024) - 512) * 64);
    }
    return samples;
}

std::vector<std::int32_t> make_lpc_friendly_samples()
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<std::int32_t> samples;
    samples.reserve(512);
    for (int i = 0; i < 512; ++i) {
        const double sample =
            (std::sin((2.0 * kPi * i) / 31.0) * 11000.0) +
            (std::sin((2.0 * kPi * i) / 11.0) * 3500.0);
        auto quantized = static_cast<int>(std::lround(sample / 64.0)) * 64;
        quantized = std::clamp(quantized, -32768, 32704);
        samples.push_back(quantized);
    }
    return samples;
}

std::string pack_expected_lds(const std::vector<std::int32_t>& samples)
{
    std::string packed;
    for (std::size_t i = 0; i < samples.size(); i += 4) {
        const ldcompress::SampleGroup group {
            static_cast<std::int16_t>(samples[i + 0]),
            static_cast<std::int16_t>(samples[i + 1]),
            static_cast<std::int16_t>(samples[i + 2]),
            static_cast<std::int16_t>(samples[i + 3]),
        };
        const auto packed_group = ldcompress::pack_group(group);
        packed.append(reinterpret_cast<const char*>(packed_group.data()), packed_group.size());
    }
    return packed;
}

std::array<std::uint8_t, 16> md5_samples_s16le(const std::vector<std::int32_t>& samples)
{
    ldcompress::Md5 md5;
    for (const auto sample : samples) {
        const auto s16 = static_cast<std::int16_t>(sample);
        const std::array<std::uint8_t, 2> bytes {
            static_cast<std::uint8_t>(s16 & 0xff),
            static_cast<std::uint8_t>((s16 >> 8) & 0xff),
        };
        md5.update(bytes.data(), bytes.size());
    }
    return md5.digest();
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

unsigned read_bits(std::string_view bytes, std::size_t bit_offset, unsigned bit_count)
{
    unsigned value = 0;
    for (unsigned i = 0; i < bit_count; ++i) {
        const auto absolute_bit = bit_offset + i;
        const auto byte = static_cast<unsigned char>(bytes.at(absolute_bit / 8));
        const auto bit = (byte >> (7U - (absolute_bit % 8U))) & 1U;
        value = (value << 1U) | bit;
    }
    return value;
}

struct FirstFrameFixedInfo {
    unsigned order = 0;
    unsigned partition_order = 0;
    unsigned wasted_bits = 0;
};

struct FirstFrameSubframeInfo {
    unsigned type = 0;
    unsigned wasted_bits = 0;
    std::size_t sample_data_bit_offset = 0;
};

std::size_t first_frame_subframe_bit_offset(std::string_view bytes)
{
    constexpr std::size_t kStreamInfoBytes = 42;
    constexpr std::size_t kFrameHeaderBytes = 4;
    constexpr std::size_t kFrameNumberBytes = 1;
    constexpr std::size_t kCrc8Bytes = 1;
    const auto frame_header_bit_offset = kStreamInfoBytes * 8U;
    const auto block_size_code = read_bits(bytes, frame_header_bit_offset + 16U, 4);
    const std::size_t block_size_extension_bytes = block_size_code == 6
        ? 1
        : (block_size_code == 7 ? 2 : 0);
    return (kStreamInfoBytes + kFrameHeaderBytes + kFrameNumberBytes +
        block_size_extension_bytes + kCrc8Bytes) * 8U;
}

unsigned first_frame_subframe_type(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    return read_bits(bytes, first_frame_subframe_bit_offset(bytes) + 1U, 6);
}

FirstFrameSubframeInfo first_frame_subframe_info(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    const auto subframe_bit_offset = first_frame_subframe_bit_offset(bytes);
    FirstFrameSubframeInfo info {
        .type = read_bits(bytes, subframe_bit_offset + 1U, 6),
        .wasted_bits = 0,
        .sample_data_bit_offset = subframe_bit_offset + 8U,
    };

    if (read_bits(bytes, subframe_bit_offset + 7U, 1) != 0) {
        unsigned leading_zeroes = 0;
        while (read_bits(bytes, info.sample_data_bit_offset + leading_zeroes, 1) == 0) {
            ++leading_zeroes;
        }
        info.wasted_bits = leading_zeroes + 1U;
        info.sample_data_bit_offset += leading_zeroes + 1U;
    }

    return info;
}

unsigned first_frame_wasted_bits(const std::filesystem::path& flac_path)
{
    return first_frame_subframe_info(flac_path).wasted_bits;
}

FirstFrameFixedInfo first_frame_fixed_info(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    const auto subframe_info = first_frame_subframe_info(flac_path);
    if (subframe_info.type < 8 || subframe_info.type > 12) {
        throw std::runtime_error("first frame did not use a fixed predictor subframe");
    }
    const auto order = subframe_info.type - 8;
    const auto effective_bits_per_sample = 16U - subframe_info.wasted_bits;
    const auto partition_order_bit_offset =
        subframe_info.sample_data_bit_offset +
        (static_cast<std::size_t>(order) * effective_bits_per_sample) + 2U;
    return FirstFrameFixedInfo {
        .order = order,
        .partition_order = read_bits(bytes, partition_order_bit_offset, 4),
        .wasted_bits = subframe_info.wasted_bits,
    };
}

using FrameWriter = ldcompress::FlacSubframeDecision (*)(
    std::ostream&,
    const std::vector<std::int32_t>&,
    const ldcompress::FlacFrameInfo&);

ldcompress::FlacSubframeDecision write_fixed_rice_file(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    FrameWriter writer = ldcompress::write_mono_fixed_rice_frame)
{
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }

    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    return writer(output, samples, frame_info);
}

void verify_fixed_rice_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned expected_order,
    unsigned expected_partition_order,
    int expected_wasted_bits = -1)
{
    write_fixed_rice_file(flac_path, samples);
    const auto frame_info = first_frame_fixed_info(flac_path);
    require(frame_info.order == expected_order,
        "native fixed/Rice writer chose an unexpected fixed predictor order");
    require(frame_info.partition_order == expected_partition_order,
        "native fixed/Rice writer chose an unexpected Rice partition order");
    if (expected_wasted_bits >= 0) {
        require(frame_info.wasted_bits == static_cast<unsigned>(expected_wasted_bits),
            "native fixed/Rice writer chose an unexpected wasted-bits count");
    }

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected fixed/Rice decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected fixed/Rice decoded LDS byte count");
    require(decoded.str() == expected, "native fixed/Rice FLAC did not round-trip to expected LDS");
}

void verify_selected_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned expected_type,
    int expected_wasted_bits = -1)
{
    write_fixed_rice_file(flac_path, samples, ldcompress::write_mono_best_frame);
    require(first_frame_subframe_type(flac_path) == expected_type,
        "native best-frame writer chose an unexpected subframe type");
    if (expected_wasted_bits >= 0) {
        require(first_frame_wasted_bits(flac_path) == static_cast<unsigned>(expected_wasted_bits),
            "native best-frame writer chose an unexpected wasted-bits count");
    }

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected best-frame decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected best-frame decoded LDS byte count");
    require(decoded.str() == expected, "native best-frame FLAC did not round-trip to expected LDS");
}

void verify_lpc_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples)
{
    const auto decision = write_fixed_rice_file(
        flac_path, samples, ldcompress::write_mono_best_frame);
    require(decision.kind == ldcompress::FlacSubframeKind::LpcRice,
        "native best-frame writer did not choose LPC for LPC-friendly samples");
    require(decision.lpc_order >= 1 && decision.lpc_order <= 12,
        "native best-frame writer chose an unexpected LPC order");
    const auto type = first_frame_subframe_type(flac_path);
    require(type >= 0x20 && type <= 0x2b,
        "native best-frame writer did not write an expected LPC subframe type");

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected LPC decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected LPC decoded LDS byte count");
    require(decoded.str() == expected, "native LPC FLAC did not round-trip to expected LDS");
}

void test_native_verbatim_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-writer-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "verbatim.flac";
    const auto samples = make_samples();

    {
        std::ofstream output(flac_path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not create test FLAC file");
        }

        const ldcompress::FlacStreamInfo stream_info {
            .min_block_size = static_cast<unsigned>(samples.size()),
            .max_block_size = static_cast<unsigned>(samples.size()),
            .min_frame_size = 0,
            .max_frame_size = 0,
            .sample_rate = 40000,
            .channels = 1,
            .bits_per_sample = 16,
            .total_samples = samples.size(),
            .md5 = md5_samples_s16le(samples),
        };
        ldcompress::write_native_flac_streaminfo(output, stream_info);

        const ldcompress::FlacFrameInfo frame_info {
            .frame_number = 0,
            .sample_rate = 40000,
            .bits_per_sample = 16,
        };
        ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    }

    require(ldcompress::detect_flac_container(flac_path.string()) == ldcompress::FlacContainer::Native,
        "native writer output was not detected as native FLAC");
    require(first_frame_wasted_bits(flac_path) == 6,
        "native verbatim writer did not mark LDS low zero bits as wasted");

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected decoded LDS byte count");
    require(decoded.str() == expected, "native verbatim FLAC did not round-trip to expected LDS");

    std::filesystem::remove_all(temp_dir);
}

void test_native_fixed_rice_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-fixed-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_fixed_rice_round_trip(temp_dir / "order0.flac", std::vector<std::int32_t>(16, 0), 0, 0);
    verify_fixed_rice_round_trip(temp_dir / "order1.flac", {
        -2, -1, -1, -2, 0, -2, -2, -4,
        -6, -8, -6, -8, -7, -8, -7, -9,
    }, 1, 0);
    verify_fixed_rice_round_trip(temp_dir / "order2.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    }, 2, 0, 6);

    std::vector<std::int32_t> quadratic;
    std::vector<std::int32_t> cubic;
    for (int i = 0; i < 32; ++i) {
        quadratic.push_back(i * i * 8);
        cubic.push_back(i * i * i);
    }
    verify_fixed_rice_round_trip(temp_dir / "order3.flac", quadratic, 3, 0);
    verify_fixed_rice_round_trip(temp_dir / "order4.flac", cubic, 4, 0);
    verify_fixed_rice_round_trip(temp_dir / "partitioned.flac", {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 0, 1);
    verify_fixed_rice_round_trip(temp_dir / "tiny.flac", {0, -1, 1, -2}, 0, 0);
    verify_fixed_rice_round_trip(temp_dir / "wide.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 0, 0);

    std::filesystem::remove_all(temp_dir);
}

void test_native_best_subframe_selection()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-best-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_selected_round_trip(temp_dir / "constant.flac",
        std::vector<std::int32_t>(16, -4096), 0, 12);
    verify_selected_round_trip(temp_dir / "fixed.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    }, 10, 6);
    verify_selected_round_trip(temp_dir / "verbatim.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 1, 6);
    verify_lpc_round_trip(temp_dir / "lpc.flac", make_lpc_friendly_samples());

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main()
{
    try {
        test_native_verbatim_round_trip();
        test_native_fixed_rice_round_trip();
        test_native_best_subframe_selection();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_native_writer: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
