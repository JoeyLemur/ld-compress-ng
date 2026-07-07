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
| Vulkan headers + loader + `glslangValidator` | Optional | Vulkan `devices` enumeration and in-development Vulkan backend | Disable with `-DLDCOMPRESS_ENABLE_VULKAN=OFF`; Vulkan compression currently requires `--threads 1`. |
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

Install from a configured build:

```sh
cmake --install build --prefix /usr/local
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

For Vulkan 1.1 development, install Vulkan headers, the loader development
package, `glslangValidator`, and a vendor runtime. Common package names:

| Distribution | Vulkan development packages |
| --- | --- |
| Debian/Ubuntu | `libvulkan-dev`, `glslang-tools`, optional `vulkan-tools` |
| Fedora/RHEL-family | `vulkan-headers`, `vulkan-loader-devel`, `glslang`, optional `vulkan-tools` |
| Arch Linux | `vulkan-headers`, `vulkan-icd-loader`, `glslang`, optional `vulkan-tools` |

`LDCOMPRESS_ENABLE_VULKAN=ON` is opportunistic: CMake enables Vulkan only when
both Vulkan development files and `glslangValidator` are found. Without them,
`ld-compress-ng devices` reports `Vulkan support: not built`.

When Vulkan is enabled, CMake compiles the checked-in GLSL shaders to SPIR-V
with `glslangValidator`. The smoke shader is passed to `vulkan_smoke`; the
fixed/constant/LPC exact-analysis shader is embedded into the binary for
`--backend vulkan`. Set `LDCOMPRESS_VULKAN_TEST_DEVICE` at configure time to
force a specific backend-local Vulkan device index for the Vulkan CTests.

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

### Linux Vulkan Development Validation Record

The current Vulkan device enumeration and smoke shader were validated on
`smaug` with Vulkan development files and `glslangValidator` installed. Devices
reported by `ld-compress-ng devices` from a GPU-visible context:

- `[0] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)`, integrated GPU.
- `[1] NVIDIA GeForce RTX 5070 Ti`, discrete GPU.
- `[2] NVIDIA GeForce RTX 4070 SUPER`, discrete GPU.
- `[3] llvmpipe (LLVM 19.1.7, 256 bits)`, CPU Vulkan implementation.

Validation commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_VULKAN=ON
cmake --build build --parallel
build/ld-compress-ng devices
ctest --test-dir build -L vulkan --output-on-failure
build/test_vulkan_smoke build/shaders/vulkan_smoke.comp.spv --device 1
build/test_vulkan_analysis --device 1
build/ld-compress-ng compress --backend vulkan --device 1 capture.lds
```

Observed result: the Vulkan-labelled CTests passed, direct fixed/Rice and LPC
analyzer parity passed on NVIDIA device `[1]`, and direct Vulkan CLI compression
on that NVIDIA device verified back to the source LDS bytes. Direct smoke runs
also passed on the AMD integrated GPU plus both NVIDIA GPUs. Sandboxed runs may
expose only llvmpipe; use a GPU-visible context for NVIDIA/AMD runtime
validation.

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
system C library, `libFLAC`, and `libogg`. OpenCL and Vulkan appear only when
the corresponding optional paths are enabled and found by CMake.

Useful smoke commands:

```sh
build/ld-compress-ng --help
build/ld-compress-ng --version
build/ld-compress-ng devices
ctest --test-dir build --output-on-failure
```

`ld-compress-ng devices` prints grouped OpenCL and Vulkan device indexes.
OpenCL indexes are used by `compress --backend opencl --device INDEX` or
`--opencl-device INDEX`, plus platform-local `platform/device` coordinates. The
OpenCL compression backend writes native FLAC and requires an available OpenCL
device at runtime. Vulkan indexes are backend-local and used by
`compress --backend vulkan --device INDEX`; the current Vulkan compression path
is a 1.1 development path that requires `--threads 1` and a compute-capable
device with `shaderInt64`.

## Install Layout

The CMake install target installs:

- `ld-compress-ng` under `${CMAKE_INSTALL_BINDIR}`.
- `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `CHANGELOG.md` under
  `${CMAKE_INSTALL_DOCDIR}`.
- Markdown files from `docs/` under `${CMAKE_INSTALL_DOCDIR}/docs`.

Use a temporary prefix for local install validation:

```sh
cmake --install build --prefix /tmp/ld-compress-ng-install
/tmp/ld-compress-ng-install/bin/ld-compress-ng --version
```

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
order `12`, LPC coefficient precision `12`, and maximum Rice partition order
`5`. Compression still defaults to one thread unless `--threads` is specified;
use `--threads 8` for routine native benchmark comparisons. OpenCL and Vulkan
use the same native tuning options, but currently require `--threads 1`.
Vulkan exact-costs fixed/Rice and GPU-generated LPC candidates. Use `--stats`
on native/OpenCL/Vulkan compression when investigating backend behavior;
accelerated backends also print coarse timing splits for scan, analyzer,
selected-frame writing, and accelerator plan/exact-analysis stages. Vulkan
additionally prints GPU queue timestamp splits when the selected compute queue
supports timestamp queries, which helps separate transfer/readback cost from
generated-LPC and exact residual/Rice shader work.

## Local Validation Matrix

Use the local matrix helper when you want the normal configure/build/test checks
without remembering the exact command pile:

```sh
python3 tools/check_local_matrix.py
```

By default, the helper configures isolated `build/local-check/default`,
`build/local-check/no-opencl`, and `build/local-check/no-vulkan` trees, builds
them, runs their CTest suites, and runs `ld-compress-ng devices` from each tree.
The default lane keeps opt-in
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
device unless `--opencl-device INDEX` is provided. Pass
`--include-vulkan-real-fixture` for the Vulkan-labelled compatibility test; it
uses the first backend-usable Vulkan device (`available` plus `shaderInt64`)
unless `--vulkan-device INDEX` is provided. Run GPU lanes from a context that
can see the accelerator runtime and devices; sandboxed executions may skip or
report no available devices even when the system build can see them. For Vulkan
performance tests on mixed-GPU hosts, use an explicit discrete GPU index from
`ld-compress-ng devices`; the integrated AMD device is suitable for functional
smoke testing but should not be used for NVIDIA performance numbers.
For `compress`, `--device INDEX` is backend-local shorthand for
`--opencl-device INDEX` or `--vulkan-device INDEX` after `--backend` selects
OpenCL or Vulkan. For `bench --include-opencl --include-vulkan`, use
`--opencl-device INDEX` and `--vulkan-device INDEX`; the bare `--device` form
is rejected because it is ambiguous. Optional accelerator `bench` rows are
omitted when no suitable device is visible, while direct `compress --backend
vulkan --device INDEX` fails if the selected device is unavailable or lacks
backend-required features such as `shaderInt64`.
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
Use `ctest --test-dir build-real-fixtures -L real-fixtures -LE "opencl|vulkan"`
when you want the scalar real-fixture suite without accelerator runtime checks.
The fixture tree remains ignored by Git.

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
`10,12`, LPC coefficient precisions `10,12`, Rice partition order `5`, and `8`
native threads. Add `--include-opencl` and optionally `--opencl-device INDEX` to include
OpenCL backend rows in the CSV/Markdown output when an OpenCL device is
available. Add `--include-vulkan` and optionally `--vulkan-device INDEX` to
include Vulkan backend rows; on mixed-GPU hosts, pass the discrete GPU index so
the run does not land on an integrated GPU. Expand the grid explicitly when
doing a broader local tuning pass:

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
OpenCL outputs total `79,952,087` bytes. The latest Vulkan sweep on NVIDIA
device `1` outputs `79,892,217` bytes. That leaves scalar native-fixed about
`-0.27%` smaller than CPU/libFLAC, OpenCL about `-0.17%` smaller than
CPU/libFLAC, and Vulkan about `-0.24%` smaller than CPU/libFLAC on the current
fixture set; keep Rice partition order `5` as the default speed/size tradeoff
unless a future sweep justifies changing it.

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
