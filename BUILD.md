# Build And Dependency Notes

`ld-compress-ng` is a C++20/CMake project targeting Linux and macOS on arm64 and
amd64/x86_64. The normal CPU compressor is intentionally small: it does not
depend on Qt, ffmpeg, `.NET`, Mono, FlaLDF, OpenSSL, or `ld-lds-converter` at
build time or runtime.

## Dependency Matrix

| Component | Required | Purpose | Notes |
| --- | --- | --- | --- |
| C++20 compiler | Yes | Build all targets | AppleClang, Clang, or GCC with C++20 support. |
| CMake 3.20+ | Yes | Configure/build | Matches the project minimum in `CMakeLists.txt`. |
| `pkg-config` / `pkgconf` | Yes | Locate FLAC/Ogg | CMake uses imported pkg-config targets. |
| `libFLAC` development files | Yes | CPU FLAC encode/decode | Requires pkg-config module `flac`. |
| `libogg` development files | Yes | Ogg FLAC container support | Requires pkg-config module `ogg`. |
| OpenCL headers + loader/framework | Optional | `devices` enumeration, future GPU backend | Disable with `-DLDCOMPRESS_ENABLE_OPENCL=OFF`. |
| `ffmpeg` | No | Legacy fixture regeneration only | Not used by normal builds/tests/runtime. |
| `ld-lds-converter` | No | Legacy fixture regeneration only | Reference binary may live under `build-ld-lds-converter/`. |
| Qt | No | Reference converter build only | `ld-compress-ng` itself does not link Qt. |

## macOS

Install the baseline CPU build dependencies with Homebrew:

```sh
brew install cmake pkg-config flac libogg
```

Xcode Command Line Tools provide AppleClang and the macOS SDK. The SDK also
provides the deprecated but still linkable OpenCL framework used by the optional
`devices` command. If OpenCL is unavailable or intentionally unwanted, configure
with `-DLDCOMPRESS_ENABLE_OPENCL=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CPU-only configure:

```sh
cmake -S . -B build-noopencl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=OFF
cmake --build build-noopencl --parallel
ctest --test-dir build-noopencl --output-on-failure
```

## Linux

Debian/Ubuntu baseline CPU build:

```sh
sudo apt install build-essential cmake pkg-config libflac-dev libogg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Fedora/RHEL-family baseline CPU build:

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config flac-devel libogg-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Arch Linux baseline CPU build:

```sh
sudo pacman -S base-devel cmake pkgconf flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For OpenCL enumeration on Linux, install OpenCL headers plus the ICD loader
development files, and make sure a vendor ICD/runtime is installed for the
actual hardware. Examples include Mesa/Rusticl, Intel NEO, NVIDIA, or ROCm.
OpenCL headers/linker support alone is enough to build, but it does not
guarantee that `ld-compress-ng devices` will find usable hardware.

Common optional package names:

| Distribution | OpenCL development packages |
| --- | --- |
| Debian/Ubuntu | `ocl-icd-opencl-dev`, `opencl-headers`, optional `clinfo` |
| Fedora/RHEL-family | `ocl-icd-devel`, `opencl-headers`, optional `clinfo` |
| Arch Linux | `ocl-icd`, `opencl-headers`, optional `clinfo` |

CPU-only configure on Linux is the same as macOS:

```sh
cmake -S . -B build-noopencl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=OFF
cmake --build build-noopencl --parallel
ctest --test-dir build-noopencl --output-on-failure
```

## Verifying The Built Binary

The binary should show Qt-free runtime linkage. On macOS:

```sh
otool -L build/ld-compress-ng
```

On Linux:

```sh
ldd build/ld-compress-ng
```

Expected required runtime libraries for the CPU build are the C++ runtime,
system C library, `libFLAC`, and `libogg`. OpenCL appears only when the optional
OpenCL path is enabled and found by CMake.

Useful smoke commands:

```sh
build/ld-compress-ng --help
build/ld-compress-ng devices
ctest --test-dir build --output-on-failure
```

## Opt-In Real Fixture Regression

Normal tests use generated or embedded fixtures and do not require real RF
captures. If ignored reference data is available locally, configure a separate
build with the real-fixture suite enabled:

```sh
cmake -S . -B build-real-fixtures -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=ON \
    -DLDCOMPRESS_REAL_FIXTURE_DIR=reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a
cmake --build build-real-fixtures --parallel
ctest --test-dir build-real-fixtures -L real-fixtures --output-on-failure
```

The real-fixture test recursively finds `.lds` files under the configured
directory, verifies matching legacy `.ldf` files when present, and round-trips
each fixture through the CPU/libFLAC and threaded native-fixed backends. It
prints ratio, timing, and compact native decision-stat columns as a local
regression scoreboard. The fixture tree remains ignored by Git.

## Opt-In FLAC Decoder Testbench

The local `reference/flac-test-files/` tree can be used for targeted FLAC
compatibility checks. These are not general audio playback tests; they verify
that `ld-compress-ng` accepts only RF-shaped FLAC input for decompression and
rejects non-40 kHz, non-mono, non-16-bit, missing-STREAMINFO, and raw-frame
testbench cases cleanly.

```sh
cmake -S . -B build-flac-test-files -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=ON \
    -DLDCOMPRESS_FLAC_TEST_FILE_DIR=reference/flac-test-files
cmake --build build-flac-test-files --parallel
ctest --test-dir build-flac-test-files -L flac-test-files --output-on-failure
```

The FLAC testbench tree remains ignored by Git and is used only when the opt-in
CMake option is enabled.

## Real Fixture Tuning Sweep

For native encoder tuning work, use the helper script to run `bench` across all
ignored real fixtures and save CSV plus Markdown summaries under the ignored
`build/` tree:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build/ld-compress-ng \
    --fixtures reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a
```

The default sweep is intentionally focused: frame size `4608`, LPC orders
`10,12`, LPC coefficient precisions `10,12`, Rice partition order `5`, and one
thread. Expand the grid explicitly when doing a broader local tuning pass:

```sh
python3 tools/sweep_real_fixtures.py \
    --threads 1,8 \
    --frame-samples 2304,4608 \
    --lpc-order 8,10,12 \
    --lpc-precision 8,10,12,14 \
    --rice-partition-order 4,5,6
```

Use `--dry-run` to print the generated `bench` commands and `--limit N` for a
quick subset check. The helper depends only on Python 3 stdlib, the built
`ld-compress-ng` binary, and local ignored fixture files.

After adding Tukey-windowed LPC analysis candidates and retuning over the six
current real fixtures, frame size `4608`, LPC order `12`, LPC coefficient
precision `12`, and Rice partition order `5` are the current default
native-fixed settings. That configuration produces `79,920,941` bytes across
the current real fixtures, about `-0.21%` smaller than CPU/libFLAC. Rice
partition order `6` squeezed the aggregate to `79,914,216` bytes, but the byte
gain was small enough that `5` remains the default speed/size tradeoff.

## Legacy Fixture Regeneration

Committed tests do not require `ffmpeg` or `ld-lds-converter`; the legacy Ogg
FLAC fixture is embedded as bytes. To regenerate that fixture intentionally, use
the reference converter and ffmpeg outside the normal build path:

```sh
build-ld-lds-converter/ld-lds-converter -q -u -i reference.lds -o reference.s16
ffmpeg -hide_banner -loglevel error -y -f s16le -ar 40k -ac 1 \
    -i reference.s16 -acodec flac -compression_level 11 -f ogg reference.ldf
```

The reference converter may itself link Qt; that dependency stays isolated to
the ignored reference/helper build and is not part of `ld-compress-ng`.
