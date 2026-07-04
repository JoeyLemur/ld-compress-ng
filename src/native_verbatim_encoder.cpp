#include "native_verbatim_encoder.h"

#include "flac_native_writer.h"
#include "hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr std::size_t kFrameSamples = 4608;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;

void update_md5_s16le(Md5& md5, std::int16_t sample)
{
    const auto value = static_cast<std::uint16_t>(sample);
    const std::array<std::uint8_t, 2> bytes {
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
    };
    md5.update(bytes.data(), bytes.size());
}

template <typename OnSample>
ConversionStats process_lds_samples(std::istream& input, OnSample&& on_sample)
{
    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);

    while (input) {
        input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        for (std::size_t i = 0; i < groups; ++i) {
            PackedLdsGroup packed;
            std::memcpy(packed.data(), input_buffer.data() + (i * 5), packed.size());
            const SampleGroup samples = unpack_group(packed);
            for (const auto sample : samples) {
                on_sample(sample);
            }
        }

        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    return stats;
}

void rewind_to(std::istream& input, std::streampos position)
{
    input.clear();
    input.seekg(position);
    if (!input) {
        throw std::runtime_error("failed to rewind LDS input for native verbatim encoding");
    }
}

FlacStreamInfo make_streaminfo(
    const ConversionStats& stats,
    unsigned sample_rate,
    const std::array<std::uint8_t, 16>& md5)
{
    std::uint64_t min_block = kMinimumStreamInfoBlockSize;
    std::uint64_t max_block = kMinimumStreamInfoBlockSize;
    if (stats.samples >= kMinimumStreamInfoBlockSize && stats.samples <= kFrameSamples) {
        min_block = stats.samples;
        max_block = stats.samples;
    } else if (stats.samples > kFrameSamples) {
        min_block = kFrameSamples;
        max_block = kFrameSamples;
    }

    return FlacStreamInfo {
        .min_block_size = static_cast<unsigned>(min_block),
        .max_block_size = static_cast<unsigned>(max_block),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = sample_rate,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = stats.samples,
        .md5 = md5,
    };
}

void write_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    std::uint64_t frame_number,
    unsigned sample_rate)
{
    const FlacFrameInfo frame_info {
        .frame_number = frame_number,
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
    };
    write_mono_verbatim_frame(output, samples, frame_info);
}

}  // namespace

ConversionStats compress_lds_to_native_verbatim_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate)
{
    const auto start = lds_input.tellg();
    if (start == std::streampos(-1)) {
        throw std::runtime_error("native-verbatim backend requires a seekable LDS input");
    }

    Md5 pcm_md5;
    auto stats = process_lds_samples(lds_input, [&pcm_md5](std::int16_t sample) {
        update_md5_s16le(pcm_md5, sample);
    });
    const auto streaminfo = make_streaminfo(stats, sample_rate, pcm_md5.digest());

    rewind_to(lds_input, start);

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(output, streaminfo);

    std::vector<std::int32_t> frame_samples;
    frame_samples.reserve(kFrameSamples);
    std::uint64_t frame_number = 0;
    const auto encoded_stats = process_lds_samples(lds_input, [&](std::int16_t sample) {
        frame_samples.push_back(sample);
        if (frame_samples.size() == kFrameSamples) {
            write_frame(output, frame_samples, frame_number, sample_rate);
            ++frame_number;
            frame_samples.clear();
        }
    });
    if (!frame_samples.empty()) {
        write_frame(output, frame_samples, frame_number, sample_rate);
    }

    if (encoded_stats.input_bytes != stats.input_bytes ||
        encoded_stats.samples != stats.samples) {
        throw std::runtime_error("LDS input changed while native-verbatim backend was encoding");
    }

    output.close();
    if (!output) {
        throw std::runtime_error("failed to finish native FLAC output: " + output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

}  // namespace ldcompress
