# ld-compress-ng

`ld-compress-ng` is a focused command-line compressor for LaserDisc RF capture
files. It compresses packed `.lds` captures to FLAC-backed `.ldf` files,
decompresses them back to `.lds`, and verifies round trips without requiring the
old shell pipeline or helper tools.

The default CPU backend writes Ogg FLAC `.ldf` files with system `libFLAC` and
`libogg`. The native scalar and OpenCL backends write native FLAC `.flac.ldf`
files. CPU, scalar native, and OpenCL compression paths are ready for normal
use; OpenCL just requires a build and runtime environment with a usable OpenCL
device.

`ld-compress-ng` does not depend at runtime on Qt, ffmpeg, `.NET`, Mono, FlaLDF,
OpenSSL, or `ld-lds-converter`.

## Build

Requirements:

- CMake 3.20 or newer.
- A C++20 compiler.
- `pkg-config` or `pkgconf`.
- `libFLAC` development files.
- `libogg` development files.
- Optional OpenCL headers and loader/framework for `devices` and
  `--backend opencl`.

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

## Advanced Usage

Show the command summary:

```sh
build/ld-compress-ng --help
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
defaults are the recommended settings for normal use. The OpenCL backend uses
the same native FLAC tuning controls, but currently requires `--threads 1`.

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
build/ld-compress-ng bench capture.lds
build/ld-compress-ng bench --include-opencl --device 0 capture.lds
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
- Development host sync notes:
  [`docs/remote-sync.md`](docs/remote-sync.md)
- Third-party license notices:
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
