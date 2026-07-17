#include "lds_codec.h"
#include "metal_devices.h"
#include "opencl_devices.h"
#include "vulkan_devices.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

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

std::string read_file(const std::filesystem::path& path);

std::string run_ok_with_stderr(
    const std::string& command,
    const std::filesystem::path& stderr_path)
{
    std::filesystem::remove(stderr_path);
    const int rc = std::system((command + " 2> " + shell_quote(stderr_path)).c_str());
    if (rc != 0) {
        throw std::runtime_error("command failed: " + command);
    }
    return read_file(stderr_path);
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

void run_fails_with_stderr(
    const std::string& command,
    const std::filesystem::path& stderr_path,
    std::string_view expected)
{
    std::filesystem::remove(stderr_path);
    const int rc = std::system((command + " 2> " + shell_quote(stderr_path)).c_str());
    if (rc == 0) {
        throw std::runtime_error("command unexpectedly succeeded: " + command);
    }
    const auto stderr_text = read_file(stderr_path);
    if (stderr_text.find(expected) == std::string::npos) {
        throw std::runtime_error("command stderr did not contain expected text '" +
            std::string(expected) + "': " + stderr_text);
    }
}

void write_file(const std::filesystem::path& path, std::string_view data)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool has_temporary_output_sibling(const std::filesystem::path& output_path)
{
    const auto directory = output_path.parent_path().empty()
        ? std::filesystem::current_path()
        : output_path.parent_path();
    const auto prefix = "." + output_path.filename().string() + ".tmp-";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

int create_held_open_fifo(const std::filesystem::path& path)
{
    if (::mkfifo(path.c_str(), 0600) != 0) {
        throw std::runtime_error("could not create test FIFO: " + path.string());
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        throw std::runtime_error("could not hold test FIFO open: " + path.string());
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        (void)::close(fd);
        throw std::runtime_error("could not make test FIFO close on exec: " + path.string());
    }
    return fd;
}

pid_t start_blocked_compress(
    const std::filesystem::path& exe,
    const std::filesystem::path& input,
    const std::filesystem::path& output)
{
    const auto exe_text = exe.string();
    const auto input_text = input.string();
    const auto output_text = output.string();
    const auto child = ::fork();
    if (child < 0) {
        throw std::runtime_error("could not fork blocked compression test");
    }
    if (child == 0) {
        ::execl(exe_text.c_str(), exe_text.c_str(), "compress", input_text.c_str(),
            output_text.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return child;
}

void wait_for_temporary_output_sibling(const std::filesystem::path& output_path, pid_t child)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (has_temporary_output_sibling(output_path)) {
            return;
        }

        int status = 0;
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            throw std::runtime_error("blocked compression exited before creating staging output");
        }
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error("could not poll blocked compression test");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("blocked compression did not create staging output");
}

int wait_for_child(pid_t child)
{
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("could not wait for blocked compression test");
        }
    }
    return status;
}

void test_atomic_no_overwrite_publish(
    const std::filesystem::path& exe,
    const std::filesystem::path& temp_dir)
{
    const auto input = temp_dir / "publish-race-input.lds";
    const auto output = temp_dir / "publish-race-output.ldf";
    const int fifo_fd = create_held_open_fifo(input);
    const auto child = start_blocked_compress(exe, input, output);
    wait_for_temporary_output_sibling(output, child);

    write_file(output, "concurrent destination");
    (void)::close(fifo_fd);
    const int status = wait_for_child(child);
    require(WIFEXITED(status) && WEXITSTATUS(status) != 0,
        "no-overwrite compression accepted a concurrently created destination");
    require(read_file(output) == "concurrent destination",
        "no-overwrite compression replaced a concurrently created destination");
    require(!has_temporary_output_sibling(output),
        "no-overwrite publish race left a staging directory behind");
    std::filesystem::remove(input);
}

void test_signal_cleanup_of_staging_output(
    const std::filesystem::path& exe,
    const std::filesystem::path& temp_dir)
{
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        const auto suffix = std::to_string(signal_number);
        const auto input = temp_dir / ("signal-cleanup-input-" + suffix + ".lds");
        const auto output = temp_dir / ("signal-cleanup-output-" + suffix + ".ldf");
        const int fifo_fd = create_held_open_fifo(input);
        const auto child = start_blocked_compress(exe, input, output);
        wait_for_temporary_output_sibling(output, child);

        if (::kill(child, signal_number) != 0) {
            (void)::close(fifo_fd);
            throw std::runtime_error("could not interrupt blocked compression test");
        }
        const int status = wait_for_child(child);
        (void)::close(fifo_fd);
        require(WIFSIGNALED(status) && WTERMSIG(status) == signal_number,
            "interrupted compression did not preserve termination status");
        require(!std::filesystem::exists(output),
            "interrupted compression published an output");
        require(!has_temporary_output_sibling(output),
            "interrupted compression left a staging directory behind");
        std::filesystem::remove(input);
    }
}

std::string corrupt_native_streaminfo_md5(std::string data)
{
    require(data.size() > 42, "native FLAC test file is too small to corrupt STREAMINFO MD5");
    require(data.substr(0, 4) == "fLaC", "native FLAC test file did not have fLaC marker");
    data[26] = static_cast<char>(static_cast<unsigned char>(data[26]) ^ 0xffU);
    return data;
}

std::string corrupt_native_streaminfo_sample_count(std::string data)
{
    require(data.size() > 42, "native FLAC test file is too small to corrupt STREAMINFO sample count");
    require(data.substr(0, 4) == "fLaC", "native FLAC test file did not have fLaC marker");
    data[25] = static_cast<char>(static_cast<unsigned char>(data[25]) ^ 0x01U);
    return data;
}

std::string clear_native_streaminfo_sample_count(std::string data)
{
    require(data.size() > 42, "native FLAC test file is too small to clear STREAMINFO sample count");
    require(data.substr(0, 4) == "fLaC", "native FLAC test file did not have fLaC marker");
    data[21] = static_cast<char>(static_cast<unsigned char>(data[21]) & 0xf0U);
    data[22] = 0;
    data[23] = 0;
    data[24] = 0;
    data[25] = 0;
    return data;
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

std::optional<std::size_t> first_available_opencl_device_index()
{
    if (!ldcompress::opencl_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_opencl_devices()) {
        if (device.available) {
            return device.flat_index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> first_available_vulkan_analysis_device_index()
{
    if (!ldcompress::vulkan_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_vulkan_devices()) {
        if (device.available && device.shader_int64) {
            return device.index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> first_available_metal_device_index()
{
    if (!ldcompress::metal_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_metal_devices()) {
        if (device.available && !device.low_power) {
            return device.index;
        }
    }
    for (const auto& device : ldcompress::list_metal_devices()) {
        if (device.available) {
            return device.index;
        }
    }

    return std::nullopt;
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
    const auto opencl_default_lds = temp_dir / "opencl-default.lds";
    const auto truncated_lds = temp_dir / "truncated.lds";
    const auto pcm = temp_dir / "fixture.s16";
    const auto repacked = temp_dir / "fixture.repacked.lds";
    const auto compressed = temp_dir / "fixture.ldf";
    const auto protected_cpu_compress_output = temp_dir / "protected-cpu.ldf";
    const auto protected_native_compress_output = temp_dir / "protected-native.flac.ldf";
    const auto cpu_level = temp_dir / "fixture.cpu-level.ldf";
    const auto decompressed = temp_dir / "fixture.out.lds";
    const auto decompressed_progress = temp_dir / "fixture.progress.out.lds";
    const auto progress_stderr = temp_dir / "decompress-progress.stderr";
    const auto native = temp_dir / "fixture.flac.ldf";
    const auto native_verbatim = temp_dir / "fixture.native-verbatim.flac.ldf";
    const auto native_verbatim_out = temp_dir / "fixture.native-verbatim.out.lds";
    const auto bad_native_verbatim = temp_dir / "fixture.bad-native-verbatim.ldf";
    const auto bad_native_verbatim_reversed = temp_dir / "fixture.bad-native-verbatim-reversed.ldf";
    const auto bad_native_verbatim_level = temp_dir / "fixture.bad-native-verbatim-level.flac.ldf";
    const auto bad_native_verbatim_lpc_order = temp_dir / "fixture.bad-native-verbatim-lpc-order.flac.ldf";
    const auto bad_native_verbatim_lpc_precision = temp_dir / "fixture.bad-native-verbatim-lpc-precision.flac.ldf";
    const auto bad_native_verbatim_rice_partition_order = temp_dir / "fixture.bad-native-verbatim-rice-partition-order.flac.ldf";
    const auto native_fixed = temp_dir / "fixture.native-fixed.flac.ldf";
    const auto native_fixed_out = temp_dir / "fixture.native-fixed.out.lds";
    const auto native_fixed_unknown_total = temp_dir / "fixture.native-fixed.unknown-total.flac.ldf";
    const auto native_fixed_unknown_total_out = temp_dir / "fixture.native-fixed.unknown-total.out.lds";
    const auto bad_native_fixed_md5 = temp_dir / "fixture.native-fixed.bad-md5.flac.ldf";
    const auto bad_native_fixed_md5_out = temp_dir / "fixture.native-fixed.bad-md5.out.lds";
    const auto bad_native_fixed_count = temp_dir / "fixture.native-fixed.bad-count.flac.ldf";
    const auto protected_decompress_output = temp_dir / "fixture.protected-output.lds";
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
    const auto bad_native_fixed_level = temp_dir / "fixture.bad-native-fixed-level.flac.ldf";
    const auto bad_cpu_stats = temp_dir / "fixture.bad-cpu-stats.ldf";
    const auto bad_cpu_frame_samples = temp_dir / "fixture.bad-cpu-frame-samples.ldf";
    const auto bad_cpu_frame_samples_default = temp_dir / "fixture.bad-cpu-frame-samples-default.ldf";
    const auto bad_cpu_lpc_order_default = temp_dir / "fixture.bad-cpu-lpc-order-default.ldf";
    const auto bad_cpu_lpc_precision = temp_dir / "fixture.bad-cpu-lpc-precision.ldf";
    const auto bad_cpu_rice_partition_order = temp_dir / "fixture.bad-cpu-rice-partition-order.ldf";
    const auto bad_cpu_threads = temp_dir / "fixture.bad-cpu-threads.ldf";
    const auto bad_cpu_device = temp_dir / "fixture.bad-cpu-device.ldf";
    const auto bad_native_device = temp_dir / "fixture.bad-native-device.ldf";
    const auto bad_native_opencl_device = temp_dir / "fixture.bad-native-opencl-device.ldf";
    const auto bad_native_vulkan_device = temp_dir / "fixture.bad-native-vulkan-device.ldf";
    const auto bad_native_metal_device = temp_dir / "fixture.bad-native-metal-device.ldf";
    const auto bad_opencl_level = temp_dir / "fixture.bad-opencl-level.flac.ldf";
    const auto empty_lds = temp_dir / "empty.lds";
    const auto empty_native_verbatim = temp_dir / "empty.native-verbatim.flac.ldf";
    const auto empty_native_verbatim_out = temp_dir / "empty.native-verbatim.out.lds";
    const auto empty_native_fixed = temp_dir / "empty.native-fixed.flac.ldf";
    const auto empty_native_fixed_out = temp_dir / "empty.native-fixed.out.lds";
    const auto opencl_output = temp_dir / "fixture.opencl.flac.ldf";
    const auto opencl_output_out = temp_dir / "fixture.opencl.out.lds";
    const auto opencl_stats = temp_dir / "fixture.opencl-stats.flac.ldf";
    const auto opencl_stats_out = temp_dir / "fixture.opencl-stats.out.lds";
    const auto opencl_container = temp_dir / "fixture.opencl-container.flac.ldf";
    const auto opencl_container_out = temp_dir / "fixture.opencl-container.out.lds";
    const auto opencl_fixed_only = temp_dir / "fixture.opencl-fixed-only.flac.ldf";
    const auto opencl_fixed_only_out = temp_dir / "fixture.opencl-fixed-only.out.lds";
    const auto opencl_threads = temp_dir / "fixture.opencl-threads.flac.ldf";
    const auto opencl_threads_out = temp_dir / "fixture.opencl-threads.out.lds";
    const auto opencl_default = temp_dir / "opencl-default.flac.ldf";
    const auto opencl_default_out = temp_dir / "opencl-default.out.lds";
    const auto empty_opencl = temp_dir / "empty.opencl.flac.ldf";
    const auto empty_opencl_out = temp_dir / "empty.opencl.out.lds";
    const auto bad_opencl_device = temp_dir / "fixture.bad-opencl-device.flac.ldf";
    const auto bad_opencl_container = temp_dir / "fixture.bad-opencl-container.ldf";
    const auto vulkan_output = temp_dir / "fixture.vulkan.flac.ldf";
    const auto vulkan_output_out = temp_dir / "fixture.vulkan.out.lds";
    const auto vulkan_fixed_only = temp_dir / "fixture.vulkan-fixed-only.flac.ldf";
    const auto vulkan_fixed_only_out = temp_dir / "fixture.vulkan-fixed-only.out.lds";
    const auto empty_vulkan = temp_dir / "empty.vulkan.flac.ldf";
    const auto empty_vulkan_out = temp_dir / "empty.vulkan.out.lds";
    const auto vulkan_threads = temp_dir / "fixture.vulkan-threads.flac.ldf";
    const auto vulkan_threads_out = temp_dir / "fixture.vulkan-threads.out.lds";
    const auto bad_vulkan_opencl_device = temp_dir / "fixture.bad-vulkan-opencl-device.flac.ldf";
    const auto bad_vulkan_device = temp_dir / "fixture.bad-vulkan-device.flac.ldf";
    const auto bad_vulkan_device_out_of_range =
        temp_dir / "fixture.bad-vulkan-device-out-of-range.flac.ldf";
    const auto bad_vulkan_container = temp_dir / "fixture.bad-vulkan-container.ldf";
    const auto metal_output = temp_dir / "fixture.metal.flac.ldf";
    const auto metal_output_out = temp_dir / "fixture.metal.out.lds";
    const auto metal_stats = temp_dir / "fixture.metal-stats.flac.ldf";
    const auto metal_stats_out = temp_dir / "fixture.metal-stats.out.lds";
    const auto metal_container = temp_dir / "fixture.metal-container.flac.ldf";
    const auto metal_container_out = temp_dir / "fixture.metal-container.out.lds";
    const auto metal_fixed_only = temp_dir / "fixture.metal-fixed-only.flac.ldf";
    const auto metal_fixed_only_out = temp_dir / "fixture.metal-fixed-only.out.lds";
    const auto metal_threads = temp_dir / "fixture.metal-threads.flac.ldf";
    const auto metal_threads_out = temp_dir / "fixture.metal-threads.out.lds";
    const auto empty_metal = temp_dir / "empty.metal.flac.ldf";
    const auto empty_metal_out = temp_dir / "empty.metal.out.lds";
    const auto bad_metal_opencl_device = temp_dir / "fixture.bad-metal-opencl-device.flac.ldf";
    const auto bad_metal_vulkan_device = temp_dir / "fixture.bad-metal-vulkan-device.flac.ldf";
    const auto bad_metal_device = temp_dir / "fixture.bad-metal-device.flac.ldf";
    const auto bad_metal_level = temp_dir / "fixture.bad-metal-level.flac.ldf";
    const auto bad_metal_container = temp_dir / "fixture.bad-metal-container.ldf";
    const auto help_output = temp_dir / "help.txt";
    const auto version_output = temp_dir / "version.txt";
    const auto devices_output = temp_dir / "devices.txt";
    const auto command_stderr = temp_dir / "command.stderr";

    const std::string fixture = make_lds_fixture();
    std::string truncated_fixture;
    while (truncated_fixture.size() <= (5U * 8192U)) {
        truncated_fixture += fixture;
    }
    truncated_fixture.push_back('\0');
    write_file(lds, fixture);
    write_file(default_lds, fixture);
    write_file(alias_lds, fixture);
    write_file(opencl_default_lds, fixture);
    write_file(truncated_lds, truncated_fixture);
    write_file(empty_lds, "");

    test_atomic_no_overwrite_publish(exe, temp_dir);
    test_signal_cleanup_of_staging_output(exe, temp_dir);

    run_ok(shell_quote(exe) + " --help > " + shell_quote(help_output));
    const auto help_text = read_file(help_output);
    require(help_text.find("Common examples:") != std::string::npos,
        "help output did not include examples");
    require(help_text.find("Compression backends:") != std::string::npos,
        "help output did not describe backends");
    require(help_text.find("Reference/debug scalar native FLAC backend") != std::string::npos,
        "help output did not label native-fixed as reference/debug");
    require(help_text.find("vulkan") != std::string::npos,
        "help output did not mention the Vulkan backend");
    require(help_text.find("native-fixed/opencl/vulkan/metal/native-verbatim") != std::string::npos,
        "help output did not include Metal in native default output description");
    require(help_text.find("native/opencl/vulkan/metal write flac") != std::string::npos,
        "help output did not include Metal in native container description");
    require(help_text.find("More details: README.md and docs/build-and-testing.md") != std::string::npos,
        "help output did not point to documentation");
    require(help_text.find("order-guess-mean-estimate-rice") != std::string::npos,
        "help output did not list benchmark analysis profiles");
    require(help_text.find("ld-compress-ng --version") != std::string::npos,
        "help output did not mention --version");

    run_ok(shell_quote(exe) + " --version > " + shell_quote(version_output));
    require(read_file(version_output) == "ld-compress-ng 1.2.0\n",
        "version output did not match project version");

    run_ok(shell_quote(exe) + " convert --unpack " + shell_quote(lds) + " " + shell_quote(pcm));
    run_ok(shell_quote(exe) + " convert --pack " + shell_quote(pcm) + " " + shell_quote(repacked));
    require(read_file(repacked) == fixture, "convert round trip changed LDS bytes");
    run_fails(shell_quote(exe) + " convert --unpack --overwrite " + shell_quote(lds) + " " + shell_quote(lds));
    require(read_file(lds) == fixture, "same-path convert guard changed LDS bytes");

    run_ok(shell_quote(exe) + " compress " + shell_quote(lds) + " " + shell_quote(compressed));
    run_fails(shell_quote(exe) + " compress " + shell_quote(lds) + " " + shell_quote(compressed));
    run_fails(shell_quote(exe) + " compress --overwrite " + shell_quote(lds) + " " + shell_quote(lds));
    require(read_file(lds) == fixture, "same-path compress guard changed LDS bytes");

    write_file(protected_cpu_compress_output, "keep this CPU output");
    run_fails(shell_quote(exe) + " compress --backend cpu --overwrite " +
        shell_quote(truncated_lds) + " " + shell_quote(protected_cpu_compress_output));
    require(read_file(protected_cpu_compress_output) == "keep this CPU output",
        "failed CPU compression replaced existing output");
    require(!has_temporary_output_sibling(protected_cpu_compress_output),
        "failed CPU compression left a temporary output behind");
    run_ok(shell_quote(exe) + " compress --backend cpu --overwrite " + shell_quote(lds) +
        " " + shell_quote(protected_cpu_compress_output));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " +
        shell_quote(protected_cpu_compress_output));
    require(!has_temporary_output_sibling(protected_cpu_compress_output),
        "successful CPU compression left a temporary output behind");

    write_file(protected_native_compress_output, "keep this native output");
    run_fails(shell_quote(exe) + " compress --backend native-fixed --overwrite " +
        shell_quote(truncated_lds) + " " + shell_quote(protected_native_compress_output));
    require(read_file(protected_native_compress_output) == "keep this native output",
        "failed native compression replaced existing output");
    require(!has_temporary_output_sibling(protected_native_compress_output),
        "failed native compression left a temporary output behind");
    run_ok(shell_quote(exe) + " compress --backend native-fixed --overwrite " +
        shell_quote(lds) + " " + shell_quote(protected_native_compress_output));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " +
        shell_quote(protected_native_compress_output));
    require(!has_temporary_output_sibling(protected_native_compress_output),
        "successful native compression left a temporary output behind");

    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(compressed));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(compressed) + " " + shell_quote(decompressed));
    require(!has_temporary_output_sibling(decompressed),
        "successful Ogg decompression left a staging directory behind");
    const auto progress_text = run_ok_with_stderr(
        shell_quote(exe) + " decompress --progress " + shell_quote(compressed) + " " +
            shell_quote(decompressed_progress),
        progress_stderr);
    run_fails(shell_quote(exe) + " decompress --overwrite " + shell_quote(compressed) + " " + shell_quote(compressed));
    require(read_file(decompressed) == fixture, "Ogg FLAC round trip changed LDS bytes");
    require(read_file(decompressed_progress) == fixture,
        "progress-enabled Ogg FLAC round trip changed LDS bytes");
    require(progress_text.find("decompressing: 0%") != std::string::npos,
        "decompress --progress did not report initial progress");
    require(progress_text.find("100%") != std::string::npos,
        "decompress --progress did not report completion");
    run_ok(shell_quote(exe) + " compress --level 8 " + shell_quote(lds) + " " + shell_quote(cpu_level));
    require(read_file(cpu_level).substr(0, 4) == "OggS", "CPU --level output was not Ogg FLAC");

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
    run_fails(shell_quote(exe) + " compress --backend native-verbatim --level 8 " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim_level));
    require(!std::filesystem::exists(bad_native_verbatim_level), "native-verbatim --level rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-verbatim --lpc-order 12 " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim_lpc_order));
    require(!std::filesystem::exists(bad_native_verbatim_lpc_order), "native-verbatim --lpc-order rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-verbatim --lpc-precision 12 " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim_lpc_precision));
    require(!std::filesystem::exists(bad_native_verbatim_lpc_precision), "native-verbatim --lpc-precision rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend native-verbatim --rice-partition-order 5 " + shell_quote(lds) + " " + shell_quote(bad_native_verbatim_rice_partition_order));
    require(!std::filesystem::exists(bad_native_verbatim_rice_partition_order), "native-verbatim --rice-partition-order rejection wrote output");
    run_ok(shell_quote(exe) + " compress --backend native-fixed " + shell_quote(lds) + " " + shell_quote(native_fixed));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(native_fixed));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(native_fixed) + " " + shell_quote(native_fixed_out));
    require(read_file(native_fixed_out) == fixture, "native-fixed FLAC round trip changed LDS bytes");
    write_file(native_fixed_unknown_total,
        clear_native_streaminfo_sample_count(read_file(native_fixed)));
    const auto unknown_total_progress_text = run_ok_with_stderr(
        shell_quote(exe) + " decompress --progress " + shell_quote(native_fixed_unknown_total) +
            " " + shell_quote(native_fixed_unknown_total_out),
        progress_stderr);
    require(read_file(native_fixed_unknown_total_out) == fixture,
        "unknown-total native FLAC round trip changed LDS bytes");
    require(unknown_total_progress_text.find("samples decoded") != std::string::npos,
        "decompress --progress did not report decoded samples for an unknown total");
    require(std::filesystem::file_size(native_fixed) < std::filesystem::file_size(native_verbatim),
        "native-fixed fixture was not smaller than native-verbatim fixture");
    write_file(bad_native_fixed_md5, corrupt_native_streaminfo_md5(read_file(native_fixed)));
    run_ok(shell_quote(exe) + " decompress " + shell_quote(bad_native_fixed_md5) + " " + shell_quote(bad_native_fixed_md5_out));
    require(read_file(bad_native_fixed_md5_out) == fixture,
        "STREAMINFO MD5 mismatch changed decompressed LDS output");
    require(!has_temporary_output_sibling(bad_native_fixed_md5_out),
        "STREAMINFO MD5 mismatch left a staging directory behind");
    write_file(bad_native_fixed_count, corrupt_native_streaminfo_sample_count(read_file(native_fixed)));
    write_file(protected_decompress_output, "keep this LDS output");
    run_fails(shell_quote(exe) + " decompress --overwrite " + shell_quote(bad_native_fixed_count) + " " + shell_quote(protected_decompress_output));
    require(read_file(protected_decompress_output) == "keep this LDS output",
        "failed decompress replaced existing output");
    require(!has_temporary_output_sibling(protected_decompress_output),
        "failed overwrite decompression left a staging directory behind");
    run_ok(shell_quote(exe) + " decompress --overwrite " + shell_quote(native_fixed) + " " + shell_quote(protected_decompress_output));
    require(read_file(protected_decompress_output) == fixture,
        "successful overwrite decompress did not replace existing output");
    require(!has_temporary_output_sibling(protected_decompress_output),
        "successful overwrite decompression left a staging directory behind");
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
    run_fails(shell_quote(exe) + " compress --backend native-fixed --level 8 " + shell_quote(lds) + " " + shell_quote(bad_native_fixed_level));
    require(!std::filesystem::exists(bad_native_fixed_level), "native-fixed --level rejection wrote output");
    run_fails(shell_quote(exe) + " compress --stats " + shell_quote(lds) + " " + shell_quote(bad_cpu_stats));
    require(!std::filesystem::exists(bad_cpu_stats), "CPU --stats rejection wrote output");
    run_fails(shell_quote(exe) + " compress --frame-samples 2048 " + shell_quote(lds) + " " + shell_quote(bad_cpu_frame_samples));
    require(!std::filesystem::exists(bad_cpu_frame_samples), "CPU --frame-samples rejection wrote output");
    run_fails(shell_quote(exe) + " compress --frame-samples 4608 " + shell_quote(lds) + " " + shell_quote(bad_cpu_frame_samples_default));
    require(!std::filesystem::exists(bad_cpu_frame_samples_default), "CPU default --frame-samples rejection wrote output");
    run_fails(shell_quote(exe) + " compress --lpc-order 12 " + shell_quote(lds) + " " + shell_quote(bad_cpu_lpc_order_default));
    require(!std::filesystem::exists(bad_cpu_lpc_order_default), "CPU default --lpc-order rejection wrote output");
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
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend native-fixed --vulkan-device 0 " +
            shell_quote(lds) + " " + shell_quote(bad_native_vulkan_device),
        command_stderr,
        "--vulkan-device is currently supported only by the vulkan backend");
    require(!std::filesystem::exists(bad_native_vulkan_device), "native --vulkan-device rejection wrote output");
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend native-fixed --metal-device 0 " +
            shell_quote(lds) + " " + shell_quote(bad_native_metal_device),
        command_stderr,
        "--metal-device is currently supported only by the metal backend");
    require(!std::filesystem::exists(bad_native_metal_device), "native --metal-device rejection wrote output");
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
    const auto opencl_device_index = first_available_opencl_device_index();
    if (opencl_device_index.has_value()) {
        const auto opencl_device_arg = " --device " + std::to_string(*opencl_device_index);
        run_ok(shell_quote(exe) + " compress --backend opencl" + opencl_device_arg + " " + shell_quote(lds) + " " + shell_quote(opencl_output));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(opencl_output));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_output) + " " + shell_quote(opencl_output_out));
        require(read_file(opencl_output_out) == fixture, "OpenCL FLAC round trip changed LDS bytes");
        const auto preserved_opencl_output = read_file(opencl_output);
        run_fails(shell_quote(exe) + " compress --backend opencl --overwrite" +
            opencl_device_arg + " " + shell_quote(truncated_lds) + " " +
            shell_quote(opencl_output));
        require(read_file(opencl_output) == preserved_opencl_output,
            "failed OpenCL compression replaced existing output");
        require(!has_temporary_output_sibling(opencl_output),
            "failed OpenCL compression left a temporary output behind");
        run_ok(shell_quote(exe) + " compress --backend opencl --stats" + opencl_device_arg + " " + shell_quote(lds) + " " + shell_quote(opencl_stats));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_stats) + " " + shell_quote(opencl_stats_out));
        require(read_file(opencl_stats_out) == fixture, "OpenCL --stats round trip changed LDS bytes");
        require(read_file(opencl_stats) == read_file(opencl_output),
            "OpenCL --stats output differed from normal output");
        run_ok(shell_quote(exe) + " compress --backend opencl --container flac" + opencl_device_arg + " " + shell_quote(lds) + " " + shell_quote(opencl_container));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_container) + " " + shell_quote(opencl_container_out));
        require(read_file(opencl_container_out) == fixture,
            "OpenCL explicit native container round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend opencl --lpc-order 0" + opencl_device_arg + " " + shell_quote(lds) + " " + shell_quote(opencl_fixed_only));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_fixed_only) + " " + shell_quote(opencl_fixed_only_out));
        require(read_file(opencl_fixed_only_out) == fixture,
            "OpenCL fixed-only round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend opencl --threads 2" + opencl_device_arg + " " + shell_quote(lds) + " " + shell_quote(opencl_threads));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_threads) + " " + shell_quote(opencl_threads_out));
        require(read_file(opencl_threads_out) == fixture,
            "OpenCL threaded writer round trip changed LDS bytes");
        run_ok("cd " + shell_quote(temp_dir) + " && " + shell_quote(exe) + " compress --backend opencl" + opencl_device_arg + " opencl-default.lds");
        require(std::filesystem::exists(opencl_default), "OpenCL default output name was not .flac.ldf");
        run_ok(shell_quote(exe) + " decompress " + shell_quote(opencl_default) + " " + shell_quote(opencl_default_out));
        require(read_file(opencl_default_out) == fixture,
            "OpenCL default-name round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend opencl" + opencl_device_arg + " " + shell_quote(empty_lds) + " " + shell_quote(empty_opencl));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(empty_lds) + " " + shell_quote(empty_opencl));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(empty_opencl) + " " + shell_quote(empty_opencl_out));
        require(read_file(empty_opencl_out).empty(), "empty OpenCL FLAC produced decoded LDS bytes");
    } else {
        run_fails(shell_quote(exe) + " compress --backend opencl " + shell_quote(lds) + " " + shell_quote(opencl_output));
        require(!std::filesystem::exists(opencl_output), "unavailable OpenCL backend wrote output");
    }
    run_fails(shell_quote(exe) + " compress --backend opencl --level 8 " + shell_quote(lds) + " " + shell_quote(bad_opencl_level));
    require(!std::filesystem::exists(bad_opencl_level), "OpenCL --level rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend opencl --device nope " + shell_quote(lds) + " " + shell_quote(bad_opencl_device));
    require(!std::filesystem::exists(bad_opencl_device), "invalid OpenCL device index wrote output");
    run_fails(shell_quote(exe) + " compress --backend opencl --container ogg " + shell_quote(lds) + " " + shell_quote(bad_opencl_container));
    require(!std::filesystem::exists(bad_opencl_container), "OpenCL Ogg rejection wrote output");
    const auto vulkan_device_index = first_available_vulkan_analysis_device_index();
    if (vulkan_device_index.has_value()) {
        const auto vulkan_device_arg = " --device " + std::to_string(*vulkan_device_index);
        run_ok(shell_quote(exe) + " compress --backend vulkan" + vulkan_device_arg + " " + shell_quote(lds) + " " + shell_quote(vulkan_output));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(vulkan_output));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(vulkan_output) + " " + shell_quote(vulkan_output_out));
        require(read_file(vulkan_output_out) == fixture, "Vulkan FLAC round trip changed LDS bytes");
        const auto preserved_vulkan_output = read_file(vulkan_output);
        run_fails(shell_quote(exe) + " compress --backend vulkan --overwrite --lpc-order 0" +
            vulkan_device_arg + " " + shell_quote(truncated_lds) + " " +
            shell_quote(vulkan_output));
        require(read_file(vulkan_output) == preserved_vulkan_output,
            "failed Vulkan compression replaced existing output");
        require(!has_temporary_output_sibling(vulkan_output),
            "failed Vulkan compression left a temporary output behind");
        run_ok(shell_quote(exe) + " compress --backend vulkan --lpc-order 0" + vulkan_device_arg + " " + shell_quote(lds) + " " + shell_quote(vulkan_fixed_only));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(vulkan_fixed_only) + " " + shell_quote(vulkan_fixed_only_out));
        require(read_file(vulkan_fixed_only_out) == fixture,
            "Vulkan fixed-only FLAC round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend vulkan --threads 2 --lpc-order 0" + vulkan_device_arg + " " + shell_quote(lds) + " " + shell_quote(vulkan_threads));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(vulkan_threads) + " " + shell_quote(vulkan_threads_out));
        require(read_file(vulkan_threads_out) == fixture,
            "Vulkan threaded writer round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend vulkan --lpc-order 0" + vulkan_device_arg + " " + shell_quote(empty_lds) + " " + shell_quote(empty_vulkan));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(empty_lds) + " " + shell_quote(empty_vulkan));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(empty_vulkan) + " " + shell_quote(empty_vulkan_out));
        require(read_file(empty_vulkan_out).empty(), "empty Vulkan FLAC produced decoded LDS bytes");
    } else {
        run_fails(shell_quote(exe) + " compress --backend vulkan --lpc-order 0 " + shell_quote(lds) + " " + shell_quote(vulkan_output));
        require(!std::filesystem::exists(vulkan_output), "unavailable Vulkan backend wrote output");
    }
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend vulkan --opencl-device 0 " +
            shell_quote(lds) + " " + shell_quote(bad_vulkan_opencl_device),
        command_stderr,
        "--opencl-device and --metal-device cannot be used with the vulkan backend");
    require(!std::filesystem::exists(bad_vulkan_opencl_device), "Vulkan --opencl-device rejection wrote output");
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend vulkan --vulkan-device nope " +
            shell_quote(lds) + " " + shell_quote(bad_vulkan_device),
        command_stderr,
        "invalid device index: nope");
    require(!std::filesystem::exists(bad_vulkan_device), "invalid Vulkan device index wrote output");
    if (ldcompress::vulkan_support_built()) {
        const auto vulkan_devices = ldcompress::list_vulkan_devices();
        if (!vulkan_devices.empty()) {
            const auto out_of_range_index = vulkan_devices.size();
            run_fails_with_stderr(
                shell_quote(exe) + " compress --backend vulkan --vulkan-device " +
                    std::to_string(out_of_range_index) + " " + shell_quote(lds) + " " +
                    shell_quote(bad_vulkan_device_out_of_range),
                command_stderr,
                "visible devices: " + std::to_string(vulkan_devices.size()));
            const auto stderr_text = read_file(command_stderr);
            require(stderr_text.find("ld-compress-ng devices") != std::string::npos,
                "out-of-range Vulkan CLI error did not point to devices command");
            require(!std::filesystem::exists(bad_vulkan_device_out_of_range),
                "out-of-range Vulkan device index wrote output");
        }
    }
    run_fails(shell_quote(exe) + " compress --backend vulkan --container ogg " + shell_quote(lds) + " " + shell_quote(bad_vulkan_container));
    require(!std::filesystem::exists(bad_vulkan_container), "Vulkan Ogg rejection wrote output");
    const auto metal_device_index = first_available_metal_device_index();
    if (metal_device_index.has_value()) {
        const auto metal_device_arg = " --device " + std::to_string(*metal_device_index);
        run_ok(shell_quote(exe) + " compress --backend metal" + metal_device_arg + " " + shell_quote(lds) + " " + shell_quote(metal_output));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(metal_output));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(metal_output) + " " + shell_quote(metal_output_out));
        require(read_file(metal_output_out) == fixture, "Metal FLAC round trip changed LDS bytes");
        const auto preserved_metal_output = read_file(metal_output);
        run_fails(shell_quote(exe) + " compress --backend metal --overwrite" +
            metal_device_arg + " " + shell_quote(truncated_lds) + " " +
            shell_quote(metal_output));
        require(read_file(metal_output) == preserved_metal_output,
            "failed Metal compression replaced existing output");
        require(!has_temporary_output_sibling(metal_output),
            "failed Metal compression left a temporary output behind");
        run_ok(shell_quote(exe) + " compress --backend metal --stats" + metal_device_arg + " " + shell_quote(lds) + " " + shell_quote(metal_stats));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(metal_stats) + " " + shell_quote(metal_stats_out));
        require(read_file(metal_stats_out) == fixture, "Metal --stats round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend metal --container flac" + metal_device_arg + " " + shell_quote(lds) + " " + shell_quote(metal_container));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(metal_container) + " " + shell_quote(metal_container_out));
        require(read_file(metal_container_out) == fixture,
            "Metal explicit native container round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend metal --lpc-order 0" + metal_device_arg + " " + shell_quote(lds) + " " + shell_quote(metal_fixed_only));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(metal_fixed_only) + " " + shell_quote(metal_fixed_only_out));
        require(read_file(metal_fixed_only_out) == fixture,
            "Metal fixed-only FLAC round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend metal --threads 2" + metal_device_arg + " " + shell_quote(lds) + " " + shell_quote(metal_threads));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(metal_threads) + " " + shell_quote(metal_threads_out));
        require(read_file(metal_threads_out) == fixture,
            "Metal threaded writer round trip changed LDS bytes");
        run_ok(shell_quote(exe) + " compress --backend metal" + metal_device_arg + " " + shell_quote(empty_lds) + " " + shell_quote(empty_metal));
        run_ok(shell_quote(exe) + " verify --source " + shell_quote(empty_lds) + " " + shell_quote(empty_metal));
        run_ok(shell_quote(exe) + " decompress " + shell_quote(empty_metal) + " " + shell_quote(empty_metal_out));
        require(read_file(empty_metal_out).empty(), "empty Metal FLAC produced decoded LDS bytes");
    } else {
        run_fails(shell_quote(exe) + " compress --backend metal --lpc-order 0 " + shell_quote(lds) + " " + shell_quote(metal_output));
        require(!std::filesystem::exists(metal_output), "unavailable Metal backend wrote output");
    }
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend metal --opencl-device 0 " +
            shell_quote(lds) + " " + shell_quote(bad_metal_opencl_device),
        command_stderr,
        "--opencl-device and --vulkan-device cannot be used with the metal backend");
    require(!std::filesystem::exists(bad_metal_opencl_device), "Metal --opencl-device rejection wrote output");
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend metal --vulkan-device 0 " +
            shell_quote(lds) + " " + shell_quote(bad_metal_vulkan_device),
        command_stderr,
        "--opencl-device and --vulkan-device cannot be used with the metal backend");
    require(!std::filesystem::exists(bad_metal_vulkan_device), "Metal --vulkan-device rejection wrote output");
    run_fails_with_stderr(
        shell_quote(exe) + " compress --backend metal --metal-device nope " +
            shell_quote(lds) + " " + shell_quote(bad_metal_device),
        command_stderr,
        "invalid device index: nope");
    require(!std::filesystem::exists(bad_metal_device), "invalid Metal device index wrote output");
    run_fails(shell_quote(exe) + " compress --backend metal --level 8 " + shell_quote(lds) + " " + shell_quote(bad_metal_level));
    require(!std::filesystem::exists(bad_metal_level), "Metal --level rejection wrote output");
    run_fails(shell_quote(exe) + " compress --backend metal --container ogg " + shell_quote(lds) + " " + shell_quote(bad_metal_container));
    require(!std::filesystem::exists(bad_metal_container), "Metal Ogg rejection wrote output");
    run_ok(shell_quote(exe) + " bench --threads 1,2 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " bench --threads 1 --frame-samples 2048 --lpc-order 12 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " bench --threads 1,2 --frame-samples 1024,2048 --lpc-order 0,8 --lpc-precision 10,12 --rice-partition-order 0,4 " + shell_quote(lds));
    run_ok(shell_quote(exe) + " bench --threads 1 --frame-samples 2048 --lpc-order 8 --lpc-precision 12 --rice-partition-order 5 --analysis-profile exact,order-guess-exact-rice,order-guess-mean-rice,order-guess-mean-estimate-rice,subdivide-tukey3-mean-rice,subdivide-tukey3-mean-estimate-rice " + shell_quote(lds));
    if (opencl_device_index.has_value()) {
        run_ok(shell_quote(exe) + " bench --include-opencl --device " +
            std::to_string(*opencl_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0,12 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-opencl --reuse-opencl-session --device " +
            std::to_string(*opencl_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
    } else {
        run_ok(shell_quote(exe) + " bench --include-opencl --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-opencl --reuse-opencl-session --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    }
    run_fails(shell_quote(exe) + " bench --include-opencl --device 999999 --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    if (vulkan_device_index.has_value()) {
        run_ok(shell_quote(exe) + " bench --include-vulkan --device " +
            std::to_string(*vulkan_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-vulkan --reuse-vulkan-session --device " +
            std::to_string(*vulkan_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
    } else {
        run_ok(shell_quote(exe) + " bench --include-vulkan --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-vulkan --reuse-vulkan-session --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    }
    run_fails(shell_quote(exe) + " bench --include-vulkan --vulkan-device 999999 --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    if (metal_device_index.has_value()) {
        run_ok(shell_quote(exe) + " bench --include-metal --device " +
            std::to_string(*metal_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-metal --reuse-metal-session --device " +
            std::to_string(*metal_device_index) +
            " --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " +
            shell_quote(lds));
    } else {
        run_ok(shell_quote(exe) + " bench --include-metal --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
        run_ok(shell_quote(exe) + " bench --include-metal --reuse-metal-session --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    }
    run_fails(shell_quote(exe) + " bench --include-metal --metal-device 999999 --threads 1 --frame-samples 2048 --lpc-order 0 --lpc-precision 12 --rice-partition-order 5 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --include-opencl --include-vulkan --device 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --include-opencl --include-metal --device 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --device 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --vulkan-device 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --metal-device 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --reuse-opencl-session " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --reuse-vulkan-session " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --reuse-metal-session " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --threads 0 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --threads 1,,2 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --frame-samples 1024,15 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --lpc-order 0,13 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --lpc-precision 0,16 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --rice-partition-order 0,9 " + shell_quote(lds));
    run_fails(shell_quote(exe) + " bench --analysis-profile nope " + shell_quote(lds));
    run_ok(shell_quote(exe) + " devices > " + shell_quote(devices_output));
    const auto devices_text = read_file(devices_output);
    require(devices_text.find("OpenCL support:") != std::string::npos,
        "devices output did not include OpenCL support status");
    require(devices_text.find("Vulkan support:") != std::string::npos,
        "devices output did not include Vulkan support status");
    require(devices_text.find("Metal support:") != std::string::npos,
        "devices output did not include Metal support status");
    if (ldcompress::vulkan_support_built()) {
        const auto vulkan_devices = ldcompress::list_vulkan_devices();
        if (!vulkan_devices.empty()) {
            require(devices_text.find("shaderInt64:") != std::string::npos,
                "devices output did not include shaderInt64 status");
            require(devices_text.find("vulkan backend usable:") != std::string::npos,
                "devices output did not include Vulkan backend usability status");
        }
    }

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
