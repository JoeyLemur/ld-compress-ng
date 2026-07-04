# ld-compress-ng

Targeted LaserDisc RF capture compression tooling.

This repository is replacing the historical `ld-compress` shell pipeline with a
native C++20/CMake CLI. Reference material from `FlaLDF/`, `ld-decode-tools/`,
and the original `ld-compress` script is intentionally ignored by Git.

See `PROJECT_PLAN.md` for the implementation plan.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

## Current Status

The current implementation provides:

- Native LDS 10-bit pack/unpack conversion.
- CPU compression to Ogg FLAC `.ldf` using `libFLAC`/`libogg`.
- Decompression from Ogg FLAC and native FLAC to packed `.lds`.
- CRC32-based verification, optionally against an original `.lds`.

The OpenCL/FlaLDF-derived GPU backend is not implemented yet.

## Usage

```sh
ld-compress-ng compress [--level N] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]
ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]
ld-compress-ng verify [--source ORIGINAL.lds] INPUT
ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]
```

Defaults:

- `compress` writes Ogg FLAC `.ldf` output.
- `--container flac` writes native FLAC, useful for compatibility testing with
  the future `.flac.ldf` GPU lane.
- Compression levels accept the legacy CPU range `1..12`; values above libFLAC's
  preset range currently map to libFLAC level 8.
