#include "flac_codec.h"

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

std::string make_lds_fixture()
{
    std::string data;
    for (int group = 0; group < 512; ++group) {
        const ldcompress::SampleGroup samples {
            static_cast<std::int16_t>((((group * 4) + 0) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 1) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 2) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 3) % 1024 - 512) * 64),
        };
        const auto packed = ldcompress::pack_group(samples);
        data.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    }
    return data;
}

void test_ogg_flac_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(temp_dir);
    const auto output_path = temp_dir / "fixture.ldf";

    const std::string fixture = make_lds_fixture();
    std::stringstream input(fixture);
    const auto encode_stats = ldcompress::compress_lds_to_flac(
        input, output_path.string(), ldcompress::FlacEncodeOptions {});
    require(encode_stats.input_bytes == fixture.size(), "wrong encoded input byte count");
    require(encode_stats.samples == 2048, "wrong encoded sample count");
    require(encode_stats.output_bytes > 0, "compressed file is empty");
    require(ldcompress::detect_flac_container(output_path.string()) == ldcompress::FlacContainer::Ogg,
        "default compression did not write Ogg FLAC");

    std::stringstream decoded;
    const auto decode_stats = ldcompress::decompress_flac_to_lds(output_path.string(), decoded);
    require(decode_stats.samples == 2048, "wrong decoded sample count");
    require(decode_stats.output_bytes == fixture.size(), "wrong decoded output byte count");
    require(decoded.str() == fixture, "FLAC round trip changed LDS bytes");

    std::filesystem::remove(output_path);
    std::filesystem::remove(temp_dir);
}

}  // namespace

int main()
{
    try {
        test_ogg_flac_round_trip();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_roundtrip: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
