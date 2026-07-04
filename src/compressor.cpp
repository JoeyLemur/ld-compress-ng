#include "compressor.h"

#include "native_flac_encoder.h"

#include <stdexcept>

namespace ldcompress {

const char* backend_name(CompressionBackend backend)
{
    switch (backend) {
    case CompressionBackend::CpuLibFlac:
        return "cpu";
    case CompressionBackend::NativeVerbatimFlac:
        return "native-verbatim";
    case CompressionBackend::NativeFixedFlac:
        return "native-fixed";
    case CompressionBackend::OpenClNativeFlac:
        return "opencl";
    }
    return "unknown";
}

ConversionStats compress_lds(
    std::istream& lds_input,
    const std::string& output_path,
    const CompressionOptions& options)
{
    switch (options.backend) {
    case CompressionBackend::CpuLibFlac: {
        const FlacEncodeOptions flac_options {
            .container = options.container,
            .compression_level = options.compression_level,
            .sample_rate = options.sample_rate,
        };
        return compress_lds_to_flac(lds_input, output_path, flac_options);
    }
    case CompressionBackend::NativeVerbatimFlac:
        if (options.container != FlacContainer::Native) {
            throw std::runtime_error("native-verbatim backend writes native FLAC only");
        }
        return compress_lds_to_native_verbatim_flac(
            lds_input, output_path, options.sample_rate, options.thread_count,
            options.native_frame_samples, options.native_max_lpc_order,
            options.native_max_rice_partition_order,
            options.native_stats);
    case CompressionBackend::NativeFixedFlac:
        if (options.container != FlacContainer::Native) {
            throw std::runtime_error("native-fixed backend writes native FLAC only");
        }
        return compress_lds_to_native_fixed_flac(
            lds_input, output_path, options.sample_rate, options.thread_count,
            options.native_frame_samples, options.native_max_lpc_order,
            options.native_max_rice_partition_order,
            options.native_stats);
    case CompressionBackend::OpenClNativeFlac:
        throw std::runtime_error("OpenCL compression backend is not implemented yet");
    }

    throw std::runtime_error("unknown compression backend");
}

}  // namespace ldcompress
