# ld-compress-ng

`ld-compress-ng` is a focused command-line compressor for LaserDisc RF capture
files. It compresses packed `.lds` captures to FLAC-backed `.ldf` files,
decompresses them back to `.lds`, and verifies round trips without requiring the
old shell pipeline or helper tools.

The default CPU backend writes Ogg FLAC `.ldf` files with system `libFLAC` and
`libogg`. The native scalar and OpenCL backends write native FLAC `.flac.ldf`
files. CPU, scalar native, and OpenCL compression paths are ready for normal
use; OpenCL just requires a build and runtime environment with a usable OpenCL
device. The Vulkan backend is in 1.1 development; it can write compatible
native FLAC using Vulkan exact costing for fixed/Rice and scalar-generated LPC
candidates, but it is not performance-tuned yet.

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
  enumeration and the in-development Vulkan backend.

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

For the in-development Vulkan backend, install Vulkan development headers, the
loader, `glslangValidator`, and a vendor Vulkan runtime. Current 1.1 work uses
Vulkan for Linux-first compute acceleration; the current backend supports
`--backend vulkan` and requires a compute-capable device with `shaderInt64`.

### macOS

Install the baseline CPU build dependencies with Homebrew:

```sh
brew install cmake pkg-config flac libogg
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Xcode Command Line Tools provide AppleClang and the macOS SDK. The SDK may also
provide the deprecated OpenCL framework, but Apple platform OpenCL availability
varies by OS and hardware. If you want a CPU-only build, use:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=OFF
cmake --build build --parallel
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
| `opencl` | Native FLAC `.flac.ldf` | GPU-assisted native encoder; list devices with `devices`, select one with `--device INDEX`. |
| `vulkan` | Native FLAC `.flac.ldf` | 1.1 development backend with Vulkan exact costing for fixed/Rice and scalar-generated LPC candidates. |
| `native-verbatim` | Native FLAC `.flac.ldf` | Compatibility/debug path using verbatim FLAC frames. |

Use the scalar native backend:

```sh
build/ld-compress-ng compress --backend native-fixed capture.lds
```

Use OpenCL:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend opencl --device 0 capture.lds
```

Use the current Vulkan path:

```sh
build/ld-compress-ng devices
build/ld-compress-ng compress --backend vulkan --device 0 capture.lds
```

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
build/ld-compress-ng bench --threads 8 --include-opencl --device 0 capture.lds
build/ld-compress-ng bench --threads 8 --include-vulkan --vulkan-device 1 capture.lds
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
- Development host sync notes:
  [`docs/remote-sync.md`](docs/remote-sync.md)
- Third-party license notices:
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
