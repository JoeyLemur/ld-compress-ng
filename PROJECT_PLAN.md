# LaserDisc RF Compression Project Plan

## Summary

This project will replace the existing `ld-compress` shell pipeline with a targeted,
self-contained compression tool for LaserDisc RF captures. The new project should
focus only on compression, decompression, verification, and the LDS sample-format
conversion needed for those operations.

The preferred implementation is a C++20/CMake command-line tool that targets Linux
and macOS on both arm64 and amd64/x86_64 CPUs. It should avoid depending at
runtime on `ffmpeg`, `.NET`, Mono, `flaldf`, `pv`, `openssl`, `xxd`, or an
external `ld-lds-converter` command. System libraries such as `libFLAC`,
`libogg`, and OpenCL are acceptable.

## Current State

- `ld-compress` is a shell script that orchestrates several external tools.
- CPU compression currently streams LDS input through `ld-lds-converter -u`, then
  through `ffmpeg` as signed 16-bit mono PCM at 40 kHz, producing Ogg FLAC output.
- GPU compression currently streams LDS input through `ld-lds-converter -r -u`,
  then through `flaldf`, producing native FLAC-style `.flac.ldf` output.
- Decompression streams compressed input through `ffmpeg`, then repacks the
  resulting signed 16-bit PCM with `ld-lds-converter -p`.
- Verification computes hashes over both the compressed file and the decompressed,
  repacked LDS data.
- `FlaLDF/` contains the C# and OpenCL GPU encoder implementation that should be
  treated first as a reference implementation, then as a source for a later native
  OpenCL backend.
- `ld-decode-tools/` contains the LDS sample converter logic. The relevant
  conversion itself is small and should be lifted into this project without
  bringing along the full Qt-based `ld-decode-tools` build.

## Recommendation

Build a single native CLI, tentatively named `ld-compress-ng`, using C++20 and
CMake.

C++ is the best fit for this targeted project because:

- The LDS 10-bit packed to 16-bit signed conversion is simple bit-level streaming
  code and already exists in C++ form.
- CPU compression can use `libFLAC` and `libogg` directly instead of shelling out
  to `ffmpeg`.
- The FlaLDF GPU path is OpenCL plus substantial host-side FLAC framing, Rice, and
  LPC code. Porting that host code is more direct in C++ than in Python or Rust.
- A C++/CMake project maps cleanly to both Linux and macOS packaging.

Python remains useful for tests or helper scripts, but not for the main compressor
or the future GPU backend.

## Proposed CLI

The CLI should use explicit subcommands rather than mirroring the old option soup.

```text
ld-compress-ng compress INPUT [OUTPUT]
ld-compress-ng decompress INPUT [OUTPUT]
ld-compress-ng verify INPUT [--source ORIGINAL.lds]
ld-compress-ng convert --pack|--unpack INPUT [OUTPUT]
ld-compress-ng devices
```

Initial behavior:

- `compress` defaults to CPU compression using Ogg FLAC-compatible `.ldf` output.
- `compress --backend cpu|opencl` should select between the implemented CPU path
  and the later OpenCL-native FLAC encoder. Until the GPU encoder exists,
  `opencl` is a reserved backend name that must fail before writing output.
- `decompress` accepts existing `.ldf`, `.raw.oga`, and `.flac.ldf` inputs where
  supported by the implemented decoder path.
- `verify` reports hashes for the compressed input and the decompressed/repacked
  LDS stream, and can compare against an original `.lds` file when provided.
- `convert` exposes the LDS packing/unpacking logic directly for diagnostics and
  test fixtures.
- `devices` lists available OpenCL platforms/devices when OpenCL support is
  built, and remains an enumeration scaffold until GPU compression exists.

Default output naming should preserve existing conventions unless explicitly
overridden:

- CPU compressed output: `INPUT_BASENAME.ldf`
- GPU compressed output, later phase: `INPUT_BASENAME.flac.ldf`
- Decompressed output: `INPUT_BASENAME.lds`

The tool should refuse to overwrite existing outputs unless `--overwrite` is
provided.

## Implementation Phases

### Phase 1: CPU Path and Test Harness

- Create a focused C++20/CMake project at the repository root.
- Implement native LDS packing and unpacking from the `ld-decode-tools`
  converter algorithm, without Qt.
- Implement CPU Ogg FLAC compression via `libFLAC` and `libogg`.
- Implement decompression for the formats needed to round-trip current CPU
  output.
- Implement `compress`, `decompress`, `verify`, and `convert`.
- Add generated test fixtures so tests do not require real RF captures.
- Keep the baseline CPU implementation portable and scalar until correctness and
  compatibility are established on both arm64 and amd64/x86_64.

### Phase 2: GPU Backend

- Keep the public compression boundary at packed LDS input stream to compressed
  output file plus conversion stats. Do not expose OpenCL buffers, kernel task
  structs, or FlaLDF subframe internals through the public API.
- Build native FLAC writer primitives before porting the OpenCL encoder,
  starting with bit writing, CRC helpers, STREAMINFO, and verbatim frame output.
- Port FlaLDF host-side encoder logic to native C++.
- Reuse or adapt the existing OpenCL kernel from `FlaLDF/`.
- Extend the initial OpenCL platform/device enumeration into explicit device
  selection for GPU compression.
- Add the `devices` subcommand.
- Preserve current GPU-style native FLAC `.flac.ldf` output unless a deliberate
  format migration is chosen later.
- Treat Metal support on macOS as a later optional backend after the OpenCL path
  and CPU compatibility suite are working.

### Phase 3: Hardening and Packaging

- Maintain Linux and macOS build documentation, including the required CPU
  dependency set and optional OpenCL packages.
- Add CI or a local equivalent for generated fixtures.
- Add performance checks against the old shell pipeline.
- Document compatibility with historical `.ldf`, `.raw.oga`, and `.flac.ldf`
  files.
- Consider CPU-specific optimizations such as SIMD or tuned block processing only
  after the portable CPU path has compatibility coverage on arm64 and
  amd64/x86_64.

## Test Plan

- Unit-test LDS unpacking with fixed 5-byte to 4-sample vectors.
- Unit-test LDS packing with fixed 4-sample to 5-byte vectors.
- Add randomized pack/unpack round-trip tests.
- Test empty input and malformed/truncated trailing groups.
- Generate synthetic LDS data, compress it, decompress it, and verify the
  repacked LDS bytes match the original.
- Verify Ogg FLAC `.ldf` decode parity for CPU output.
- Verify native FLAC `.flac.ldf` decode parity once GPU/native-FLAC support is
  implemented.
- For the OpenCL phase, test device enumeration, explicit device selection, CPU
  fallback behavior, and decompressed-output parity with the CPU backend.

## Constraints

- Stay inside `/Users/epowell/Development/laserdisc/compress` unless explicit
  permission is granted first.
- Do not edit vendored or cloned source trees (`FlaLDF/`, `ld-decode-tools/`)
  unless the change is intentional and documented.
- Preserve original license notices when lifting code or porting implementation
  details.
- Keep Linux and macOS compatibility as a first-order constraint, including both
  arm64 and amd64/x86_64 CPU targets.
- Treat `ld-compress`, `FlaLDF/`, and `ld-decode-tools/` as references until the
  replacement has tests proving compatible behavior.
- Keep `AGENTS.md` reserved for contributor or agent operating rules, not the
  full project design.

## Optional Follow-Up

Add a short top-level `AGENTS.md` later with only operational instructions:

- Stay within this project tree unless permission is requested first.
- Do not edit vendored source except intentionally.
- Read `PROJECT_PLAN.md` before implementation work.
