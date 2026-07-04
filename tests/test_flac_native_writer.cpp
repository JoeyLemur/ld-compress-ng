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

}  // namespace

int main()
{
    try {
        test_native_verbatim_round_trip();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_native_writer: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
