#include "flac_codec.h"

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ldcompress {
namespace {

constexpr unsigned kChannels = 1;
constexpr unsigned kBitsPerSample = 16;
constexpr unsigned kSampleRate = 40000;
constexpr std::size_t kGroupsPerChunk = 8192;

struct EncoderDeleter {
    void operator()(FLAC__StreamEncoder* encoder) const
    {
        if (encoder != nullptr) {
            FLAC__stream_encoder_delete(encoder);
        }
    }
};

using EncoderPtr = std::unique_ptr<FLAC__StreamEncoder, EncoderDeleter>;

struct DecoderDeleter {
    void operator()(FLAC__StreamDecoder* decoder) const
    {
        if (decoder != nullptr) {
            FLAC__stream_decoder_delete(decoder);
        }
    }
};

using DecoderPtr = std::unique_ptr<FLAC__StreamDecoder, DecoderDeleter>;

void require_encoder(bool ok, const char* message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

unsigned map_compression_level(unsigned legacy_level)
{
    // The old ffmpeg path accepts 1..12; libFLAC exposes 0..8 presets.
    return std::min(legacy_level, 8U);
}

struct DecoderClient {
    std::ostream& output;
    ConversionStats stats;
    DecompressionProgressCallback progress_callback;
    std::uint64_t expected_total_samples = 0;
    unsigned expected_sample_rate = 0;
    unsigned expected_channels = 0;
    unsigned expected_bits_per_sample = 0;
    bool have_streaminfo = false;
    SampleGroup pending {};
    std::size_t pending_count = 0;
    std::array<std::uint8_t, 5 * kGroupsPerChunk> packed_output {};
    std::size_t packed_output_bytes = 0;
    std::string error;
};

void set_error_once(DecoderClient& client, std::string message)
{
    if (client.error.empty()) {
        client.error = std::move(message);
    }
}

bool report_progress(DecoderClient& client)
{
    if (!client.progress_callback) {
        return true;
    }

    try {
        client.progress_callback(client.stats.samples, client.expected_total_samples);
    } catch (const std::exception& error) {
        set_error_once(client, std::string("decompression progress callback failed: ") +
            error.what());
        return false;
    } catch (...) {
        set_error_once(client, "decompression progress callback failed");
        return false;
    }
    return true;
}

bool flush_packed_output(DecoderClient& client)
{
    if (client.packed_output_bytes == 0) {
        return true;
    }

    client.output.write(reinterpret_cast<const char*>(client.packed_output.data()),
        static_cast<std::streamsize>(client.packed_output_bytes));
    if (!client.output) {
        client.error = "failed to write decompressed LDS output";
        return false;
    }
    client.stats.output_bytes += client.packed_output_bytes;
    client.packed_output_bytes = 0;
    return true;
}

bool write_packed_group(DecoderClient& client)
{
    const auto packed = pack_group(client.pending);
    if (client.packed_output.size() - client.packed_output_bytes < packed.size() &&
        !flush_packed_output(client)) {
        return false;
    }

    std::memcpy(client.packed_output.data() + client.packed_output_bytes,
        packed.data(), packed.size());
    client.packed_output_bytes += packed.size();
    if (client.packed_output_bytes == client.packed_output.size()) {
        return flush_packed_output(client);
    }
    return true;
}

FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder*,
    const FLAC__Frame* frame,
    const FLAC__int32* const buffer[],
    void* client_data)
{
    auto& client = *static_cast<DecoderClient*>(client_data);
    if (!client.error.empty()) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    if (frame->header.channels != kChannels) {
        set_error_once(client, "FLAC stream is not mono");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (frame->header.bits_per_sample != kBitsPerSample) {
        set_error_once(client, "FLAC stream is not 16-bit");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (frame->header.sample_rate != kSampleRate) {
        set_error_once(client, "FLAC stream sample rate is not 40000 Hz");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    for (std::uint32_t i = 0; i < frame->header.blocksize; ++i) {
        const auto sample = buffer[0][i];
        if (sample < -32768 || sample > 32767) {
            set_error_once(client, "decoded FLAC sample is outside int16 range");
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        client.pending[client.pending_count++] = static_cast<std::int16_t>(sample);
        ++client.stats.samples;
        if (client.pending_count == client.pending.size()) {
            if (!write_packed_group(client)) {
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
            client.pending_count = 0;
        }
    }

    if (!report_progress(client)) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_callback(
    const FLAC__StreamDecoder*,
    const FLAC__StreamMetadata* metadata,
    void* client_data)
{
    auto& client = *static_cast<DecoderClient*>(client_data);
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }

    client.have_streaminfo = true;
    client.expected_total_samples = metadata->data.stream_info.total_samples;
    client.expected_sample_rate = metadata->data.stream_info.sample_rate;
    client.expected_channels = metadata->data.stream_info.channels;
    client.expected_bits_per_sample = metadata->data.stream_info.bits_per_sample;

    if (client.expected_channels != kChannels) {
        set_error_once(client, "FLAC STREAMINFO channel count is not mono");
    } else if (client.expected_bits_per_sample != kBitsPerSample) {
        set_error_once(client, "FLAC STREAMINFO bit depth is not 16-bit");
    } else if (client.expected_sample_rate != kSampleRate) {
        set_error_once(client, "FLAC STREAMINFO sample rate is not 40000 Hz");
    } else if (client.expected_total_samples != 0 &&
        (client.expected_total_samples % 4U) != 0) {
        set_error_once(client, "FLAC STREAMINFO total sample count is not divisible by four");
    }

    if (client.error.empty()) {
        (void)report_progress(client);
    }
}

void error_callback(
    const FLAC__StreamDecoder*,
    FLAC__StreamDecoderErrorStatus status,
    void* client_data)
{
    auto& client = *static_cast<DecoderClient*>(client_data);
    set_error_once(client, std::string("FLAC decoder error: ") +
        FLAC__StreamDecoderErrorStatusString[status]);
}

}  // namespace

FlacContainer detect_flac_container(const std::string& input_path)
{
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + input_path);
    }

    std::array<char, 4> magic {};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (input.gcount() != static_cast<std::streamsize>(magic.size())) {
        throw std::runtime_error("input is too small to identify FLAC container: " + input_path);
    }

    if (std::memcmp(magic.data(), "OggS", 4) == 0) {
        return FlacContainer::Ogg;
    }
    if (std::memcmp(magic.data(), "fLaC", 4) == 0) {
        return FlacContainer::Native;
    }

    throw std::runtime_error("unsupported compressed input container: " + input_path);
}

ConversionStats compress_lds_to_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const FlacEncodeOptions& options)
{
    EncoderPtr encoder(FLAC__stream_encoder_new());
    if (!encoder) {
        throw std::runtime_error("could not allocate FLAC encoder");
    }

    require_encoder(FLAC__stream_encoder_set_channels(encoder.get(), kChannels),
        "could not set FLAC channel count");
    require_encoder(FLAC__stream_encoder_set_bits_per_sample(encoder.get(), kBitsPerSample),
        "could not set FLAC bits per sample");
    require_encoder(FLAC__stream_encoder_set_sample_rate(encoder.get(), options.sample_rate),
        "could not set FLAC sample rate");
    require_encoder(FLAC__stream_encoder_set_compression_level(
                        encoder.get(), map_compression_level(options.compression_level)),
        "could not set FLAC compression level");
    require_encoder(FLAC__stream_encoder_set_verify(encoder.get(), true),
        "could not enable FLAC encoder verification");

    const FLAC__StreamEncoderInitStatus init_status =
        options.container == FlacContainer::Ogg
            ? FLAC__stream_encoder_init_ogg_file(encoder.get(), output_path.c_str(), nullptr, nullptr)
            : FLAC__stream_encoder_init_file(encoder.get(), output_path.c_str(), nullptr, nullptr);
    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        throw std::runtime_error(std::string("could not initialize FLAC encoder: ") +
            FLAC__StreamEncoderInitStatusString[init_status]);
    }

    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);
    std::vector<FLAC__int32> samples(4 * kGroupsPerChunk);

    while (lds_input) {
        lds_input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = lds_input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            (void)FLAC__stream_encoder_finish(encoder.get());
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        for (std::size_t i = 0; i < groups; ++i) {
            PackedLdsGroup packed;
            std::memcpy(packed.data(), input_buffer.data() + (i * 5), packed.size());
            const auto unpacked = unpack_group(packed);
            for (std::size_t j = 0; j < unpacked.size(); ++j) {
                samples[(i * 4) + j] = unpacked[j];
            }
        }

        if (!FLAC__stream_encoder_process_interleaved(
                encoder.get(), samples.data(), static_cast<unsigned>(groups * 4))) {
            (void)FLAC__stream_encoder_finish(encoder.get());
            throw std::runtime_error(std::string("FLAC encoder failed: ") +
                FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder.get())]);
        }

        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4;
    }

    if (lds_input.bad()) {
        (void)FLAC__stream_encoder_finish(encoder.get());
        throw std::runtime_error("failed to read LDS input");
    }

    if (!FLAC__stream_encoder_finish(encoder.get())) {
        throw std::runtime_error(std::string("FLAC encoder did not finish cleanly: ") +
            FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder.get())]);
    }

    if (std::filesystem::exists(output_path)) {
        stats.output_bytes = std::filesystem::file_size(output_path);
    }
    return stats;
}

ConversionStats decompress_flac_to_lds(
    const std::string& input_path,
    std::ostream& lds_output,
    DecompressionProgressCallback progress_callback)
{
    DecoderPtr decoder(FLAC__stream_decoder_new());
    if (!decoder) {
        throw std::runtime_error("could not allocate FLAC decoder");
    }

    if (!FLAC__stream_decoder_set_md5_checking(decoder.get(), true)) {
        throw std::runtime_error("could not enable FLAC decoded-PCM MD5 checking");
    }

    DecoderClient client {
        .output = lds_output,
        .stats = {},
        .progress_callback = std::move(progress_callback),
        .expected_total_samples = 0,
        .expected_sample_rate = 0,
        .expected_channels = 0,
        .expected_bits_per_sample = 0,
        .have_streaminfo = false,
        .pending = {},
        .pending_count = 0,
        .packed_output = {},
        .packed_output_bytes = 0,
        .error = {},
    };
    const auto container = detect_flac_container(input_path);
    const FLAC__StreamDecoderInitStatus init_status =
        container == FlacContainer::Ogg
            ? FLAC__stream_decoder_init_ogg_file(
                  decoder.get(), input_path.c_str(), write_callback, metadata_callback,
                  error_callback, &client)
            : FLAC__stream_decoder_init_file(
                  decoder.get(), input_path.c_str(), write_callback, metadata_callback,
                  error_callback, &client);

    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        throw std::runtime_error(std::string("could not initialize FLAC decoder: ") +
            FLAC__StreamDecoderInitStatusString[init_status]);
    }

    if (!FLAC__stream_decoder_process_until_end_of_stream(decoder.get())) {
        if (!client.error.empty()) {
            throw std::runtime_error(client.error);
        }
        throw std::runtime_error(std::string("FLAC decoder failed: ") +
            FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(decoder.get())]);
    }

    if (!client.error.empty()) {
        throw std::runtime_error(client.error);
    }
    if (client.pending_count != 0) {
        throw std::runtime_error("decoded sample count is not divisible by four");
    }
    if (!client.have_streaminfo) {
        throw std::runtime_error("FLAC stream did not provide STREAMINFO metadata");
    }
    if (client.expected_channels != kChannels) {
        throw std::runtime_error("FLAC STREAMINFO channel count is not mono");
    }
    if (client.expected_bits_per_sample != kBitsPerSample) {
        throw std::runtime_error("FLAC STREAMINFO bit depth is not 16-bit");
    }
    if (client.expected_sample_rate != kSampleRate) {
        throw std::runtime_error("FLAC STREAMINFO sample rate is not 40000 Hz");
    }
    if (client.expected_total_samples != 0 &&
        client.stats.samples != client.expected_total_samples) {
        throw std::runtime_error("decoded FLAC sample count did not match STREAMINFO");
    }
    if (!FLAC__stream_decoder_finish(decoder.get())) {
        throw std::runtime_error("decoded FLAC sample MD5 did not match STREAMINFO");
    }
    if (!flush_packed_output(client)) {
        throw std::runtime_error(client.error);
    }

    if (std::filesystem::exists(input_path)) {
        client.stats.input_bytes = std::filesystem::file_size(input_path);
    }
    return client.stats;
}

}  // namespace ldcompress
