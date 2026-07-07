# ld-compress-ng

`ld-compress-ng` is a focused command-line compressor for LaserDisc RF capture
files. It compresses packed `.lds` captures to FLAC-backed `.ldf` files,
decompresses them back to `.lds`, and verifies round trips without requiring the
old shell pipeline or helper tools.

The default CPU backend writes Ogg FLAC `.ldf` files with system `libFLAC` and
`libogg`. The native scalar, OpenCL, and Vulkan backends write native FLAC
`.flac.ldf` files. CPU, scalar native, OpenCL, and Linux-first Vulkan
compression paths are ready for normal use on validated hosts. Vulkan has been
validated locally on NVIDIA hardware for compatible native FLAC output; AMD is
intended through standard Vulkan compute but not yet hardware-validated.

`ld-compress-ng` does not depend at runtime on Qt, ffmpeg, `.NET`, Mono, FlaLDF,
OpenSSL, or `ld-lds-converter`.

`ld-compress-ng` is licensed under LGPL-2.1-or-later. See
[`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for
the project license and preserved third-party notices.

## Build

Requirements:

- CMake 3.20 or newer.
- A C++20 compiler.
- `pkg-config` or `pkgconf`.
- `libFLAC` development files.
- `libogg` development files.
- Optional OpenCL headers and loader/framework for `devices` and
  `--backend opencl`.
- Optional Vulkan headers, loader, and `glslangValidator` for Vulkan device
  enumeration and the Linux-first Vulkan backend.

### Linux

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libflac-dev libogg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Fedora/RHEL-family:

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config flac-devel libogg-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Arch Linux:

```sh
sudo pacman -S base-devel cmake pkgconf flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

For OpenCL compression on Linux, also install OpenCL headers, the ICD loader
development package, and a vendor runtime for your GPU or accelerator. Common
package names include `ocl-icd-opencl-dev`/`opencl-headers` on Debian/Ubuntu,
`ocl-icd-devel`/`opencl-headers` on Fedora, and `ocl-icd`/`opencl-headers` on
Arch.

For the Vulkan backend, install Vulkan development headers, the loader,
`glslangValidator`, and a vendor Vulkan runtime. Vulkan is Linux-first for 1.1;
the backend supports `--backend vulkan` and requires a compute-capable non-CPU
device with `shaderInt64` unless you explicitly select a CPU Vulkan device for
functional testing.

### macOS

Install the baseline CPU build dependencies with Homebrew:

```sh
brew install cmake pkg-config flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Xcode Command Line Tools provide AppleClang and the macOS SDK. The SDK may also
provide the deprecated OpenCL framework, but Apple platform OpenCL availability
varies by OS and hardware. If you want a CPU-only build, disable both
accelerator backends:

```sh
cmake -S . -B build-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF
cmake --build build-cpu-only --parallel
```

Install from a configured build:

```sh
cmake --install build --prefix /usr/local
```

## Basic Usage

Compress an `.lds` capture with the default CPU/libFLAC backend:

```sh
build/ld-compress-ng compress capture.lds
```

This writes `capture.ldf` unless you provide an explicit output path.

Decompress an `.ldf`, `.raw.oga`, or `.flac.ldf` file back to `.lds`:

```sh
build/ld-compress-ng decompress capture.ldf
```

This writes `capture.lds` by default.

Verify that a compressed file decodes to the original `.lds` data:

```sh
build/ld-compress-ng verify --source capture.lds capture.ldf
```

Overwrite an existing output only when you ask for it:

```sh
build/ld-compress-ng compress --overwrite capture.lds capture.ldf
build/ld-compress-ng decompress --overwrite capture.ldf capture.lds
```

## Backends

| Backend | Output | Notes |
| --- | --- | --- |
| `cpu` | Ogg FLAC `.ldf` | Default, portable, uses system `libFLAC`/`libogg`; supports `--level`. |
| `native-fixed` | Native FLAC `.flac.ldf` | Scalar native encoder with fixed/LPC prediction, Rice coding, threading, and tuning controls. |
| `opencl` | Native FLAC `.flac.ldf` | GPU-assisted native encoder; list devices with `devices`, select one with `--device INDEX` or `--opencl-device INDEX`. |
| `vulkan` | Native FLAC `.flac.ldf` | Linux-first acceleration backend with Vulkan exact costing for fixed/Rice and GPU-generated LPC candidates; validated locally on NVIDIA and intended for standard Vulkan compute devices; select one with `--device INDEX` or `--vulkan-device INDEX`. |
| `native-verbatim` | Native FLAC `.flac.ldf` | Compatibility/debug path using verbatim FLAC frames. |

Use the scalar native backend:

```sh
build/ld-compress-ng compress --backend native-fixed capture.lds
```

Use OpenCL:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend opencl --device INDEX capture.lds
```

Use Vulkan:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend vulkan --device INDEX capture.lds
```

For `compress`, `--device INDEX` is backend-local shorthand for the selected
OpenCL or Vulkan backend. For `bench --include-opencl --include-vulkan`, use
`--opencl-device INDEX` and `--vulkan-device INDEX` because a bare `--device`
would be ambiguous.

## Advanced Usage

Show the command summary:

```sh
build/ld-compress-ng --help
```

Show the release version:

```sh
build/ld-compress-ng --version
```

Tune CPU/libFLAC compression level:

```sh
build/ld-compress-ng compress --backend cpu --level 12 capture.lds
```

Run the scalar native backend with multiple encoding threads and summary stats:

```sh
build/ld-compress-ng compress --backend native-fixed --threads 8 --stats capture.lds
```

Native tuning defaults are `--frame-samples 4608`, `--lpc-order 12`,
`--lpc-precision 12`, `--rice-partition-order 5`, and `--threads 1`; the
defaults are the recommended settings for normal use. The OpenCL and Vulkan
backends use the same native FLAC tuning controls, but currently require
`--threads 1`. Vulkan still supports `--lpc-order 0` for fixed/Rice-only
diagnostics.

Use explicit native FLAC tuning controls when you are comparing size/speed
tradeoffs:

```sh
build/ld-compress-ng compress --backend native-fixed \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5 \
    capture.lds
```

Benchmark available backends on one capture:

```sh
build/ld-compress-ng bench --threads 8 capture.lds
build/ld-compress-ng bench --threads 8 --include-opencl --opencl-device INDEX capture.lds
build/ld-compress-ng bench --threads 8 --include-vulkan --vulkan-device INDEX capture.lds
```

Convert between packed LDS and signed 16-bit little-endian mono PCM:

```sh
build/ld-compress-ng convert --unpack capture.lds capture.s16
build/ld-compress-ng convert --pack capture.s16 capture.lds
```

## Documentation

- Detailed build, validation, fixture, and tuning notes:
  [`docs/build-and-testing.md`](docs/build-and-testing.md)
- Current implementation plan and project history:
  [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md)
- Release checklist:
  [`docs/release-checklist.md`](docs/release-checklist.md)
- Changelog:
  [`CHANGELOG.md`](CHANGELOG.md)
- Third-party license notices:
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
