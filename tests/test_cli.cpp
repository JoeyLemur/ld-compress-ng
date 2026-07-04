#include "lds_codec.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string shell_quote(const std::filesystem::path& path)
{
    std::string quoted = "'";
    for (const char ch : path.string()) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

std::string shell_quote(const std::string& value)
{
    return shell_quote(std::filesystem::path(value));
}

void run_ok(const std::string& command)
{
    const int rc = std::system(command.c_str());
    if (rc != 0) {
        throw std::runtime_error("command failed: " + command);
    }
}

void run_fails(const std::string& command)
{
    const int rc = std::system(command.c_str());
    if (rc == 0) {
        throw std::runtime_error("command unexpectedly succeeded: " + command);
    }
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void write_file(const std::filesystem::path& path, std::string_view data)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string make_lds_fixture()
{
    std::string data;
    for (int group = 0; group < 2305; ++group) {
        const ldcompress::SampleGroup samples {
            static_cast<std::int16_t>((((group * 7) + 0) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 7) + 1) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 7) + 2) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 7) + 3) % 1024 - 512) * 64),
        };
        const auto packed = ldcompress::pack_group(samples);
        data.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    }
    return data;
}

void test_cli(const std::filesystem::path& exe)
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-cli-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto lds = temp_dir / "fixture.lds";
    const auto default_lds = temp_dir / "default-name.lds";
    const auto alias_lds = temp_dir / "alias-name.lds";
    const auto pcm = temp_dir / "fixture.s16";
    const auto repacked = temp_dir / "fixture.repacked.lds";
    const auto compressed = temp_dir / "fixture.ldf";
    const auto decompressed = temp_dir / "fixture.out.lds";
    const auto native = temp_dir / "fixture.flac.ldf";
    const auto native_verbatim = temp_dir / "fixture.native-verbatim.flac.ldf";
    const auto native_verbatim_out = temp_dir / "fixture.native-verbatim.out.lds";
    const auto bad_native_verbatim = temp_dir / "fixture.bad-native-verbatim.ldf";
    const auto bad_native_verbatim_reversed = temp_dir / "fixture.bad-native-verbatim-reversed.ldf";
    const auto native_fixed = temp_dir / "fixture.native-fixed.flac.ldf";
    const auto native_fixed_out = temp_dir / "fixture.native-fixed.out.lds";
    const auto native_fixed_threads = temp_dir / "fixture.native-fixed-threads.flac.ldf";
    const auto native_fixed_threads_out = temp_dir / "fixture.native-fixed-threads.out.lds";
    const auto native_fixed_stats = temp_dir / "fixture.native-fixed-stats.flac.ldf";
    const auto native_fixed_stats_out = temp_dir / "fixture.native-fixed-stats.out.lds";
    const auto native_fixed_tuned = temp_dir / "fixture.native-fixed-tuned.flac.ldf";
    const auto native_fixed_tuned_out = temp_dir / "fixture.native-fixed-tuned.out.lds";
    const auto native_fixed_tuned_threads = temp_dir / "fixture.native-fixed-tuned-threads.flac.ldf";
    const auto native_fixed_tuned_threads_out = temp_dir / "fixture.native-fixed-tuned-threads.out.lds";
    const auto default_native_fixed = temp_dir / "default-name.flac.ldf";
    const auto alias_native_fixed = temp_dir / "alias-name.flac.ldf";
    const auto backend_order_cpu = temp_dir / "backend-order-cpu.ldf";
    const auto backend_order_native = temp_dir / "backend-order-native.flac.ldf";
    const auto bad_native_fixed = temp_dir / "fixture.bad-native-fixed.ldf";
    const auto bad_native_fixed_reversed = temp_dir / "fixture.bad-native-fixed-reversed.ldf";
    const auto bad_native_fixed_threads = temp_dir / "fixture.bad-native-fixed-threads.ldf";
    const auto bad_native_frame_samples = temp_dir / "fixture.bad-native-frame-samples.ldf";
    const auto bad_native_frame_samples_large = temp_dir / "fixture.bad-native-frame-samples-large.ldf";
    const auto bad_native_lpc_order = temp_dir / "fixture.bad-native-lpc-order.ldf";
    const auto bad_native_lpc_order_large = temp_dir / "fixture.bad-native-lpc-order-large.ldf";
    const auto bad_native_lpc_precision = temp_dir / "fixture.bad-native-lpc-precision.ldf";
    const auto bad_native_lpc_precision_large = temp_dir / "fixture.bad-native-lpc-precision-large.ldf";
    const auto bad_native_rice_partition_order = temp_dir / "fixture.bad-native-rice-partition-order.ldf";
    const auto bad_native_rice_partition_order_large = temp_dir / "fixture.bad-native-rice-partition-order-large.ldf";
    const auto bad_cpu_stats = temp_dir / "fixture.bad-cpu-stats.ldf";
    const auto bad_cpu_frame_samples = temp_dir / "fixture.bad-cpu-frame-samples.ldf";
    const auto bad_cpu_lpc_precision = temp_dir / "fixture.bad-cpu-lpc-precision.ldf";
    const auto bad_cpu_rice_partition_order = temp_dir / "fixture.bad-cpu-rice-partition-order.ldf";
    const auto bad_cpu_threads = temp_dir / "fixture.bad-cpu-threads.ldf";
    const auto bad_cpu_device = temp_dir / "fixture.bad-cpu-device.ldf";
    const auto bad_native_device = temp_dir / "fixture.bad-native-device.ldf";
    const auto bad_native_opencl_device = temp_dir / "fixture.bad-native-opencl-device.ldf";
    const auto empty_lds = temp_dir / "empty.lds";
    const auto empty_native_verbatim = temp_dir / "empty.native-verbatim.flac.ldf";
    const auto empty_native_verbatim_out = temp_dir / "empty.native-verbatim.out.lds";
    const auto empty_native_fixed = temp_dir / "empty.native-fixed.flac.ldf";
    const auto empty_native_fixed_out = temp_dir / "empty.native-fixed.out.lds";
    const auto opencl_output = temp_dir / "fixture.opencl.flac.ldf";
    const auto bad_opencl_device = temp_dir / "fixture.bad-opencl-device.flac.ldf";
    const auto bad_opencl_container = temp_dir / "fixture.bad-opencl-container.ldf";

    const std::string fixture = make_lds_fixture();
    write_file(lds, fixture);
    write_file(default_lds, fixture);
    write_file(alias_lds, fixture);
    write_file(empty_lds, "");

    run_ok(shell_quote(exe) + " convert --unpack " + shell_quote(lds) + " " + shell_quote(pcm));
    run_ok(shell_quote(exe) + " convert --pack " + shell_quote(pcm) + " " + shell_quote(repacked));
    require(read_file(repacked) == fixture, "convert round trip changed LDS bytes");
    run_fails(shell_quote(exe) + " convert --unpack --overwrite " + shell_quote(lds) + " " + shell_quote(lds));
    require(read_file(lds) == fixture, "same-path convert guard changed LDS bytes");

    run_ok(shell_quote(exe) + " compress " + shell_quote(lds) + " " + shell_quote(compressed));
    run_fails(shell_quote(exe) + " compress " + shell_quote(lds) + " " + shell_quote(compressed));
    run_fails(shell_quote(exe) + " compress --overwrite " + shell_quote(lds) + " " + shell_quote(lds));
    require(read_file(lds) == fixture, "same-path compress guard changed LDS bytes");
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(compressed));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(compressed) + " " + shell_quote(decompressed));
    run_fails(shell_quote(exe) + " decompress --overwrite " + shell_quote(compressed) + " " + shell_quote(compressed));
    require(read_file(decompressed) == fixture, "Ogg FLAC round trip changed LDS bytes");

    run_ok(shell_quote(exe) + " compress --backend cpu --container flac --overwrite " + shell_quote(lds) + " " + shell_quote(native));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(native));
    run_ok(shell_quote(exe) + " compress --backend native-verbatim " + shell_quote(lds) + " " + shell_quote(native_verbatim));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(native_verbatim));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_verbatim) + " " + shell_quote(native_verbatim_out));
    require(read_file(native_verbatim_out) == fixture, "native-verbatim FLAC round trip changed LDS bytes");
    run_fails(shell_quote(exe) + " compress --backend native-verbatim --container ogg " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim));
    require(!std::filesystem::exists(bad_native_verbatim), "native-verbatim Ogg rejection wrote output");
    run_fails(shell_quote(exe) + " compress --container ogg --backend native-verbatim " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim_reversed));
    require(!std::filesystem::exists(bad_native_verbatim_reversed), "reversed native-verbatim Ogg rejection wrote output");
    run_ok(shell_quote(exe) + " compress --backend native-fixed " + shell_quote(lds) + " " + shell_quote(native_fixed));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(native_fixed));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed) + " " + shell_quote(native_fixed_out));
    require(read_file(native_fixed_out) == fixture, "native-fixed FLAC round trip changed LDS bytes");
    require(std::filesystem::file_size(native_fixed) < std::filesystem::file_size(native_verbatim),
        "native-fixed fixture was not smaller than native-verbatim fixture");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --threads 2 " + shell_quote(lds) + " " + shell_quote(native_fixed_threads));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed_threads) + " " + shell_quote(native_fixed_threads_out));
    require(read_file(native_fixed_threads_out) == fixture, "threaded native-fixed FLAC round trip changed LDS bytes");
    require(read_file(native_fixed_threads) == read_file(native_fixed),
        "threaded native-fixed output differed from single-threaded output");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --stats " + shell_quote(lds) + " " + shell_quote(native_fixed_stats));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed_stats) + " " + shell_quote(native_fixed_stats_out));
    require(read_file(native_fixed_stats_out) == fixture, "native-fixed --stats round trip changed LDS bytes");
    require(read_file(native_fixed_stats) == read_file(native_fixed),
        "native-fixed --stats output differed from normal output");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --frame-samples 2048 --lpc-order 12 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds) + " " + shell_quote(native_fixed_tuned));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed_tuned) + " " + shell_quote(native_fixed_tuned_out));
    require(read_file(native_fixed_tuned_out) == fixture, "tuned native-fixed FLAC round trip changed LDS bytes");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --threads 2 --frame-samples 2048 --lpc-order 12 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds) + " " + shell_quote(native_fixed_tuned_threads));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed_tuned_threads) + " " + shell_quote(native_fixed_tuned_threads_out));
    require(read_file(native_fixed_tuned_threads_out) == fixture, "threaded tuned native-fixed FLAC round trip changed LDS bytes");
    require(read_file(native_fixed_tuned_threads) == read_file(native_fixed_tuned),
        "threaded tuned native-fixed output differed from single-threaded output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --threads 0 " + shell_quote(lds) + " " + shell_quote(bad_native_fixed_threads));
    require(!std::filesystem::exists(bad_native_fixed_threads), "invalid native-fixed thread count wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --frame-samples 15 " + shell_quote(lds) + " " + shell_quote(bad_native_frame_samples));
    require(!std::filesystem::exists(bad_native_frame_samples), "invalid native frame size wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --frame-samples 99999999999999999999 " + shell_quote(lds) + " " + shell_quote(bad_native_frame_samples_large));
    require(!std::filesystem::exists(bad_native_frame_samples_large), "oversized native frame size wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --lpc-order 13 " + shell_quote(lds) + " " + shell_quote(bad_native_lpc_order));
    require(!std::filesystem::exists(bad_native_lpc_order), "invalid native LPC order wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --lpc-order 99999999999999999999 " + shell_quote(lds) + " " + shell_quote(bad_native_lpc_order_large));
    require(!std::filesystem::exists(bad_native_lpc_order_large), "oversized native LPC order wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --lpc-precision 0 " + shell_quote(lds) + " " + shell_quote(bad_native_lpc_precision));
    require(!std::filesystem::exists(bad_native_lpc_precision), "invalid native LPC precision wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --lpc-precision 99999999999999999999 " + shell_quote(lds) + " " + shell_quote(bad_native_lpc_precision_large));
    require(!std::filesystem::exists(bad_native_lpc_precision_large), "oversized native LPC precision wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --rice-partition-order 9 " + shell_quote(lds) + " " + shell_quote(bad_native_rice_partition_order));
    require(!std::filesystem::exists(bad_native_rice_partition_order), "invalid native Rice partition order wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --rice-partition-order 99999999999999999999 " + shell_quote(lds) + " " + shell_quote(bad_native_rice_partition_order_large));
    require(!std::filesystem::exists(bad_native_rice_partition_order_large), "oversized native Rice partition order wrote output");
    run_fails(shell_quote(exe) + " compress --stats " + shell_quote(lds) + " " + shell_quote(bad_cpu_stats));
    require(!std::filesystem::exists(bad_cpu_stats), "CPU --stats rejection wrote output");
    run_fails(shell_quote(exe) + " compress --frame-samples 2048 " + shell_quote(lds) + " " + shell_quote(bad_cpu_frame_samples));
    require(!std::filesystem::exists(bad_cpu_frame_samples), "CPU --frame-samples rejection wrote output");
    run_fails(shell_quote(exe) + " compress --lpc-precision 10 " + shell_quote(lds) + " " + shell_quote(bad_cpu_lpc_precision));
    require(!std::filesystem::exists(bad_cpu_lpc_precision), "CPU --lpc-precision rejection wrote output");
    run_fails(shell_quote(exe) + " compress --rice-partition-order 4 " + shell_quote(lds) + " " + shell_quote(bad_cpu_rice_partition_order));
    require(!std::filesystem::exists(bad_cpu_rice_partition_order), "CPU --rice-partition-order rejection wrote output");
    run_fails(shell_quote(exe) + " compress --threads 2 " + shell_quote(lds) + " " + shell_quote(bad_cpu_threads));
    require(!std::filesystem::exists(bad_cpu_threads), "CPU --threads rejection wrote output");
    run_fails(shell_quote(exe) + " compress --device 0 " + shell_quote(lds) + " " + shell_quote(bad_cpu_device));
    require(!std::filesystem::exists(bad_cpu_device), "CPU --device rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --device 0 " + shell_quote(lds) + " " + shell_quote(bad_native_device));
    require(!std::filesystem::exists(bad_native_device), "native --device rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --opencl-device 0 " + shell_quote(lds) + " " + shell_quote(bad_native_opencl_device));
    require(!std::filesystem::exists(bad_native_opencl_device), "native --opencl-device rejection wrote output");
    run_ok("cd " + shell_quote(temp_dir) + " && " + shell_quote(exe) + " compress --backend native-fixed default-name.lds");
    require(std::filesystem::exists(default_native_fixed), "native-fixed default output name was not .flac.ldf");
    run_ok("cd " + shell_quote(temp_dir) + " && " + shell_quote(exe) + " compress --backend fixed-rice alias-name.lds");
    require(std::filesystem::exists(alias_native_fixed), "fixed-rice alias default output name was not .flac.ldf");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --backend cpu " + shell_quote(lds) + " " + shell_quote(backend_order_cpu));
    require(read_file(backend_order_cpu).substr(0, 4) == "OggS",
        "final cpu backend did not restore implicit Ogg container");
    run_ok(shell_quote(exe) + " compress --backend cpu --backend native-fixed " + shell_quote(lds) + " " + shell_quote(backend_order_native));
    require(read_file(backend_order_native).substr(0, 4) == "fLaC",
        "final native-fixed backend did not select implicit native FLAC container");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --container ogg " + shell_quote(lds) + " " + shell_quote(bad_native_fixed));
    require(!std::filesystem::exists(bad_native_fixed), "native-fixed Ogg rejection wrote output");
    run_fails(shell_quote(exe) + " compress --container ogg --backend native-fixed " + shell_quote(lds) + " " + shell_quote(bad_native_fixed_reversed));
    require(!std::filesystem::exists(bad_native_fixed_reversed), "reversed native-fixed Ogg rejection wrote output");
    run_ok(shell_quote(exe) + " compress --backend native-verbatim " + shell_quote(empty_lds) + " " + shell_quote(empty_native_verbatim));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(empty_lds) + " " + shell_quote(empty_native_verbatim));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(empty_native_verbatim) + " " + shell_quote(empty_native_verbatim_out));
    require(read_file(empty_native_verbatim_out).empty(), "empty native-verbatim FLAC produced decoded LDS bytes");
    run_ok(shell_quote(exe) + " compress --backend native-fixed " + shell_quote(empty_lds) + " " + shell_quote(empty_native_fixed));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(empty_lds) + " " + shell_quote(empty_native_fixed));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(empty_native_fixed) + " " + shell_quote(empty_native_fixed_out));
    require(read_file(empty_native_fixed_out).empty(), "empty native-fixed FLAC produced decoded LDS bytes");
    run_fails(shell_quote(exe) + " compress --backend opencl " + shell_quote(lds) + " " + shell_quote(opencl_output));
    require(!std::filesystem::exists(opencl_output), "unimplemented OpenCL backend wrote output");
    run_fails(shell_quote(exe) + " compress --backend opencl --device nope " + shell_quote(lds) + " " + shell_quote(bad_opencl_device));
    require(!std::filesystem::exists(bad_opencl_device), "invalid OpenCL device index wrote output");
    run_fails(shell_quote(exe) + " compress --backend opencl --container ogg " + shell_quote(lds) + " " + shell_quote(bad_opencl_container));
    require(!std::filesystem::exists(bad_opencl_container), "OpenCL Ogg rejection wrote output");
    run_ok(shell_quote(exe) + " bench --threads 1,2 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " bench --threads 1 --frame-samples 2048 --lpc-order 12 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " bench --threads 1,2 --frame-samples 1024,2048 --lpc-order 0,8 --lpc-precision 10,12 --rice-partition-order 0,4 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --threads 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --threads 1,,2 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --frame-samples 1024,15 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --lpc-order 0,13 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --lpc-precision 0,16 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --rice-partition-order 0,9 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " devices");

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("expected path to ld-compress-ng executable");
        }
        test_cli(argv[1]);
    } catch (const std::exception& ex) {
        std::cerr << "test_cli: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
