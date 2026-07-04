# ld-compress-ng

Targeted LaserDisc RF capture compression tooling.

This repository is replacing the historical `ld-compress` shell pipeline with a
native C++20/CMake CLI. Reference material from `FlaLDF/`, `ld-decode-tools/`,
and the original `ld-compress` script is intentionally ignored by Git.

See `PROJECT_PLAN.md` for the implementation plan and `BUILD.md` for platform
build notes.

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

`ld-compress-ng` itself does not depend on Qt, ffmpeg, `.NET`, Mono, FlaLDF,
OpenSSL, or `ld-lds-converter`. Some of those tools remain useful for regenerating
legacy reference fixtures, but they are not normal build or runtime dependencies.
Ignored real RF fixtures can be exercised with the opt-in CMake real-fixture
suite documented in `BUILD.md`.

## Current Status

The current implementation provides:

- Native LDS 10-bit pack/unpack conversion.
- CPU compression to Ogg FLAC `.ldf` using `libFLAC`/`libogg`.
- Decompression from Ogg FLAC and native FLAC to packed `.lds`.
- MD5-based verification, optionally against an original `.lds`.
- A backend selection facade for CPU now and OpenCL later.
- Native FLAC bitstream primitives and an experimental `native-verbatim` backend
  that writes `.flac.ldf` streams with verbatim frames.
- An experimental scalar `native-fixed` backend that selects native FLAC
  constant, verbatim, fixed predictor/Rice-coded, or LPC/Rice-coded frames.
- Optional frame-level threading for the native FLAC backends using a bounded
  worker pool with ordered output.
- Optional OpenCL device enumeration.

The OpenCL/FlaLDF-derived GPU compression backend is not implemented yet.

## Usage

```sh
ld-compress-ng compress [--backend cpu|native-verbatim|native-fixed|opencl] [--level N] [--threads N] [--frame-samples N] [--lpc-order N] [--lpc-precision N] [--rice-partition-order N] [--stats] [--container ogg|flac] [--overwrite] INPUT [OUTPUT]
ld-compress-ng decompress [--overwrite] INPUT [OUTPUT]
ld-compress-ng verify [--source ORIGINAL.lds] INPUT
ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]
ld-compress-ng bench [--threads 1,4,8] [--frame-samples N[,N...]] [--lpc-order N[,N...]] [--lpc-precision N[,N...]] [--rice-partition-order N[,N...]] INPUT
ld-compress-ng devices
```

Defaults:

- `compress` writes Ogg FLAC `.ldf` output.
- `--backend cpu` is the current default production compression backend.
- `--backend native-verbatim` writes native FLAC `.flac.ldf` output using
  uncompressed verbatim FLAC frames. This is mainly a compatibility stepping
  stone for the future native/GPU encoder, not the final compressed path.
- `--backend native-fixed` writes native FLAC `.flac.ldf` output using scalar
  subframe selection: constant for flat frames, fixed prediction/Rice residuals,
  LPC/Rice residuals up to order 12, partition-order search `0..4` by default
  and up to `0..8` when requested,
  wasted-bits handling for the low zero bits in LDS-derived PCM, and verbatim
  fallback when predictive coding would be larger. It is a correctness milestone
  for the native/GPU path, not tuned compression yet.
- `--threads N` is currently supported for native FLAC backends and parallelizes
  frame encoding while preserving output order. It defaults to `1`.
- `--frame-samples N` is currently supported for native FLAC backends and sets
  the FLAC block size used by the native encoder. It defaults to `4608` and is
  constrained to `16..4608` for 40 kHz subset compatibility.
- `--lpc-order N` is currently supported for native FLAC backends and sets the
  maximum scalar LPC order considered by `native-fixed`. It defaults to `12`,
  matching the 40 kHz FLAC subset cap; `0` disables LPC.
- `--lpc-precision N` is currently supported for native FLAC backends and sets
  the LPC coefficient precision considered by `native-fixed`. It defaults to
  `12`; FLAC subset-compatible values `1..15` are accepted.
- `--rice-partition-order N` is currently supported for native FLAC backends and
  sets the maximum Rice partition order considered by `native-fixed`. It
  defaults to `4`; values `0..8` are accepted for FLAC subset compatibility.
- `--stats` is currently supported for native FLAC backends and prints per-frame
  subframe counts, fixed/LPC predictor order counts, Rice partition order
  counts, and wasted-bits counts.
- `--backend opencl` is reserved for the future FlaLDF-derived native FLAC path
  and currently fails before writing output.
- `--container flac` writes native FLAC, useful for compatibility testing with
  the future `.flac.ldf` GPU lane.
- Compression levels accept the legacy CPU range `1..12`; values above libFLAC's
  preset range currently map to libFLAC level 8.

Benchmarking:

- `bench` runs the CPU/libFLAC Ogg path, the native-verbatim path, and the
  native-fixed path for each requested thread count, then prints bytes, ratio,
  elapsed seconds, MiB/s, and compact native decision stats for native backends.
  For native backend tuning, `bench` accepts comma-separated `--frame-samples`,
  `--lpc-order`, `--lpc-precision`, and
  `--rice-partition-order` lists and runs the native-fixed cross product.
- Benchmark output files are temporary and removed after each run; use
  `compress` when you want to keep the encoded result.
- `tools/sweep_real_fixtures.py` wraps `bench` across the ignored real-fixture
  tree and writes CSV/Markdown summaries for native tuning work. See `BUILD.md`
  for the command and default sweep grid.
