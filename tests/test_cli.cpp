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
    for (int group = 0; group < 64; ++group) {
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
    const auto pcm = temp_dir / "fixture.s16";
    const auto repacked = temp_dir / "fixture.repacked.lds";
    const auto compressed = temp_dir / "fixture.ldf";
    const auto decompressed = temp_dir / "fixture.out.lds";
    const auto native = temp_dir / "fixture.flac.ldf";

    const std::string fixture = make_lds_fixture();
    write_file(lds, fixture);

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

    run_ok(shell_quote(exe) + " compress --container flac --overwrite " + shell_quote(lds) + " " + shell_quote(native));
    run_ok(shell_quote(exe) + " verify --source " + shell_quote(lds) + " " + shell_quote(native));
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
