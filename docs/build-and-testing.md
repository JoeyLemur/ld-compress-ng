# Detailed Build, Testing, And Development Notes

`ld-compress-ng` is a C++20/CMake project targeting Linux and macOS on arm64 and
amd64/x86_64. The normal CPU compressor is intentionally small: it does not
depend on Qt, ffmpeg, `.NET`, Mono, FlaLDF, OpenSSL, or `ld-lds-converter` at
build time or runtime. Some optional compatibility tests use local reference
tools when they are available and skip cleanly when they are not.

## Dependency Matrix

| Component | Required | Purpose | Notes |
| --- | --- | --- | --- |
| C++20 compiler | Yes | Build all targets | AppleClang, Clang, or GCC with C++20 support. |
| CMake 3.20+ | Yes | Configure/build | Matches the project minimum in `CMakeLists.txt`. |
| `pkg-config` / `pkgconf` | Yes | Locate FLAC/Ogg | CMake uses imported pkg-config targets. |
| `libFLAC` development files | Yes | CPU FLAC encode/decode | Requires pkg-config module `flac`. |
| `libogg` development files | Yes | Ogg FLAC container support | Requires pkg-config module `ogg`. |
| OpenCL headers + loader/framework | Optional | `devices` enumeration and OpenCL compression backend | Disable with `-DLDCOMPRESS_ENABLE_OPENCL=OFF`. |
| Python 3 interpreter | Optional | Skip-safe external decode compatibility CTests and helper scripts | CMake adds Python-based tests only when an interpreter is found. |
| `ffmpeg`/`ffprobe` | Optional | External native-FLAC decode compatibility CTest and legacy fixture regeneration | The compatibility test skips if `ffmpeg` is unavailable. |
| PyAV and reference `ld-decode` dependencies | Optional | External `ld-decode` loader compatibility CTests | Tests skip if the local reference tree or imports are unavailable. |
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

Apple has deprecated OpenCL and current macOS systems may build the OpenCL path
without exposing any usable OpenCL devices. Treat OpenCL runtime validation as a
Linux-first task; macOS GPU acceleration should be a later Metal backend.

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

### Linux OpenCL Validation Record

The current OpenCL analysis smoke tests were validated on `smaug`:

- OS/kernel: Debian amd64, `Linux smaug 6.12.94+deb13-amd64`.
- OpenCL platform: NVIDIA CUDA, OpenCL 3.0 CUDA 13.3.44.
- Driver: NVIDIA `610.43.02`.
- Devices reported by `ld-compress-ng devices`:
  - `[0] NVIDIA GeForce RTX 4070 SUPER`, 56 compute units,
    12,481,134,592 bytes global memory.
  - `[1] NVIDIA GeForce RTX 5070 Ti`, 70 compute units,
    16,652,042,240 bytes global memory.

Validation commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=ON
cmake --build build --parallel
build/ld-compress-ng devices
build/test_opencl_analysis
ctest --test-dir build --output-on-failure
```

Observed result: `build/test_opencl_analysis` exited successfully without skip
messages, and the non-real-fixture CTest suite passed. The test currently
selects the first available OpenCL device, so this validates device `[0]` unless
future tests add explicit multi-device coverage.

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

`ld-compress-ng devices` prints flattened OpenCL device indexes for
`compress --backend opencl --device INDEX` or `--opencl-device INDEX`, plus
platform-local `platform/device` coordinates. The OpenCL compression backend
writes native FLAC and requires an available OpenCL device at runtime.

## Compatibility And Tuning CLI Notes

The README covers the normal compression and decompression commands. These
commands are useful when checking compatibility or doing local size/speed
tuning.

Write native FLAC through the CPU/libFLAC backend instead of the default Ogg
container:

```sh
build/ld-compress-ng compress --backend cpu --container flac capture.lds capture.flac.ldf
```

The scalar native and OpenCL backends use native FLAC tuning controls:

```sh
build/ld-compress-ng compress --backend native-fixed \
    --threads 8 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5 \
    --stats \
    capture.lds
```

The current recommended native defaults are frame size `4608`, maximum LPC
order `12`, LPC coefficient precision `12`, maximum Rice partition order `5`,
and one thread unless `--threads` is specified. OpenCL uses the same native
tuning options, but currently requires `--threads 1`.

## Local Validation Matrix

Use the local matrix helper when you want the normal configure/build/test checks
without remembering the exact command pile:

```sh
python3 tools/check_local_matrix.py
```

By default, the helper configures isolated `build/local-check/default` and
`build/local-check/no-opencl` trees, builds both, runs their CTest suites, and
runs `ld-compress-ng devices` from each tree. The default lane keeps opt-in
fixture suites disabled so an existing CMake cache cannot accidentally turn a
quick check into a real-fixture run.

Add local ignored fixture suites explicitly:

```sh
python3 tools/check_local_matrix.py --include-flac-test-files
python3 tools/check_local_matrix.py --include-real-fixtures
python3 tools/check_local_matrix.py --include-real-fixtures --skip-default --skip-no-opencl
python3 tools/check_local_matrix.py --all-local
```

The FLAC testbench lane uses `reference/flac-test-files/` by default and runs
only the `flac-test-files` CTest label. The real-fixture lane uses the current
local `reference/testdata/ld-decode-testdata-ci/...` fixture root by default and
runs `real-fixtures` while excluding the OpenCL-labelled real-fixture test.
`--all-local` follows that same scalar-only real-fixture behavior. Pass
`--include-opencl-real-fixture` when you want the runtime OpenCL real-fixture
check; it implies the real-fixture lane and uses the first available OpenCL
device unless `--opencl-device INDEX` is provided. Run that GPU lane from a
context that can see the OpenCL runtime and devices; sandboxed executions may
skip or report no available devices even when the system build can see them.
Use `--dry-run` to inspect the generated commands, and `--strict-optional` to
fail instead of skipping when a requested local fixture directory is missing.

If the reference `ld-decode` dependencies live in a non-default Python
environment, pass that interpreter through to CMake:

```sh
python3 tools/check_local_matrix.py --include-real-fixtures \
    --python-executable /path/to/ld-decode-env/bin/python
```

The core OpenCL smoke tests are labelled `opencl`, so a configured OpenCL build
can also run them directly:

```sh
ctest --test-dir build -L opencl --output-on-failure
```

In a real-fixture-enabled build, that label also includes the OpenCL
real-fixture loader compatibility test.

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

The C++ real-fixture test recursively finds `.lds` files under the configured
directory, verifies matching legacy `.ldf` files when present, and round-trips
each fixture through the CPU/libFLAC and threaded native-fixed backends. It
prints ratio, timing, and compact native decision-stat columns as a local
regression scoreboard.

When Python and the local `reference/ld-decode/` loader dependencies are
available, the same CMake option also adds skip-safe external compatibility
tests. Those compress the first fixture to native `.flac.ldf` output and verify
the reference `ld-decode` loader can read both `.flac.ldf` and `.flac` suffixes.
An OpenCL real-fixture loader test is added too; it skips cleanly when OpenCL
support, a runtime device, or the reference loader dependencies are unavailable.
Use `ctest --test-dir build-real-fixtures -L real-fixtures -LE opencl` when you
want the scalar real-fixture suite without the OpenCL runtime check. The fixture
tree remains ignored by Git.

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
thread. Add `--include-opencl` and optionally `--opencl-device INDEX` to include
OpenCL backend rows in the CSV/Markdown output when an OpenCL device is
available. Expand the grid explicitly when doing a broader local
tuning pass:

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

After adding Tukey-windowed LPC analysis candidates plus high-order
Welch-windowed candidates and retuning over the six current real fixtures, frame
size `4608`, LPC order `12`, LPC coefficient precision `12`, and Rice partition
order `5` are the current default native-fixed settings. In the latest pinned
sweep, raw LDS inputs total `149,954,560` bytes, CPU/libFLAC outputs total
`80,086,984` bytes, scalar native-fixed outputs total `79,867,690` bytes, and
OpenCL outputs total `79,952,087` bytes. That leaves scalar native-fixed about
`-0.27%` smaller than CPU/libFLAC and OpenCL about `-0.17%` smaller than
CPU/libFLAC on the current fixture set; keep Rice partition order `5` as the
default speed/size tradeoff unless a future sweep justifies changing it.

## Legacy Fixture Regeneration

Committed generated-fixture tests do not require `ffmpeg` or `ld-lds-converter`;
the legacy Ogg FLAC fixture is embedded as bytes. To regenerate that fixture
intentionally, use the reference converter and ffmpeg outside the normal build
path:

```sh
build-ld-lds-converter/ld-lds-converter -q -u -i reference.lds -o reference.s16
ffmpeg -hide_banner -loglevel error -y -f s16le -ar 40k -ac 1 \
    -i reference.s16 -acodec flac -compression_level 11 -f ogg reference.ldf
```

The reference converter may itself link Qt; that dependency stays isolated to
the ignored reference/helper build and is not part of `ld-compress-ng`.
