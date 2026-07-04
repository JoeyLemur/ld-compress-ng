#include "flac_codec.h"
#include "flac_native_writer.h"
#include "hash.h"
#include "lds_codec.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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

unsigned first_frame_fixed_order(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    constexpr std::size_t kStreamInfoBytes = 42;
    constexpr std::size_t kSmallFirstFrameHeaderBytes = 7;
    const auto subframe_bit_offset =
        (kStreamInfoBytes + kSmallFirstFrameHeaderBytes) * 8U;
    const auto type = read_bits(bytes, subframe_bit_offset + 1U, 6);
    if (type < 8 || type > 12) {
        throw std::runtime_error("first frame did not use a fixed predictor subframe");
    }
    return type - 8;
}

void write_fixed_rice_file(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples)
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
    ldcompress::write_mono_fixed_rice_frame(output, samples, frame_info);
}

void verify_fixed_rice_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned expected_order)
{
    write_fixed_rice_file(flac_path, samples);
    require(first_frame_fixed_order(flac_path) == expected_order,
        "native fixed/Rice writer chose an unexpected fixed predictor order");

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected fixed/Rice decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected fixed/Rice decoded LDS byte count");
    require(decoded.str() == expected, "native fixed/Rice FLAC did not round-trip to expected LDS");
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

    verify_fixed_rice_round_trip(temp_dir / "order0.flac", std::vector<std::int32_t>(16, 0), 0);
    verify_fixed_rice_round_trip(temp_dir / "order1.flac", {
        -2, -1, -1, -2, 0, -2, -2, -4,
        -6, -8, -6, -8, -7, -8, -7, -9,
    }, 1);
    verify_fixed_rice_round_trip(temp_dir / "order2.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    }, 2);

    std::vector<std::int32_t> quadratic;
    std::vector<std::int32_t> cubic;
    for (int i = 0; i < 32; ++i) {
        quadratic.push_back(i * i * 8);
        cubic.push_back(i * i * i);
    }
    verify_fixed_rice_round_trip(temp_dir / "order3.flac", quadratic, 3);
    verify_fixed_rice_round_trip(temp_dir / "order4.flac", cubic, 4);
    verify_fixed_rice_round_trip(temp_dir / "tiny.flac", {0, -1, 1, -2}, 0);
    verify_fixed_rice_round_trip(temp_dir / "wide.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 0);

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main()
{
    try {
        test_native_verbatim_round_trip();
        test_native_fixed_rice_round_trip();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_native_writer: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
