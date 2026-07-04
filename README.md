# ld-compress-ng

Targeted LaserDisc RF capture compression tooling.

This repository is replacing the historical `ld-compress` shell pipeline with a
native C++20/CMake CLI. Reference material from `FlaLDF/`, `ld-decode-tools/`,
and the original `ld-compress` script is intentionally ignored by Git.

See `PROJECT_PLAN.md` for the implementation plan.

The baseline CPU path is intended to stay portable across Linux and macOS on
both arm64 and amd64/x86_64. CPU-specific optimizations and macOS Metal GPU
support are later-phase work after compatibility is nailed down.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Required build dependencies are `pkg-config`, `libFLAC`, and `libogg`. OpenCL is
optional; when CMake cannot find it, the CPU compressor still builds and
`devices` reports that OpenCL support was not built.

## Current Status

The current implementation provides:

- Native LDS 10-bit pack/unpack conversion.
- CPU compression to Ogg FLAC `.ldf` using `libFLAC`/`libogg`.
- Decompression from Ogg FLAC and native FLAC to packed `.lds`.
- MD5-based verification, optionally against an original `.lds`.
- A backend selection facade for CPU now and OpenCL later.
- Optional OpenCL device enumeration.

The OpenCL/FlaLDF-derived GPU compression backend is not implemented yet.

## Usage

```sh
ld-compress-ng compress [--backend cpu|opencl] [--level N] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]
ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]
ld-compress-ng verify [--source ORIGINAL.lds] INPUT
ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]
ld-compress-ng devices
```

Defaults:

- `compress` writes Ogg FLAC `.ldf` output.
- `--backend cpu` is the current default and only implemented compression
  backend.
- `--backend opencl` is reserved for the future FlaLDF-derived native FLAC path
  and currently fails before writing output.
- `--container flac` writes native FLAC, useful for compatibility testing with
  the future `.flac.ldf` GPU lane.
- Compression levels accept the legacy CPU range `1..12`; values above libFLAC's
  preset range currently map to libFLAC level 8.
