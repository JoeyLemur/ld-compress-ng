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

## Reference And Legacy State

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
- `reference/testdata/` contains ignored real `.lds` fixtures from
  `ld-decode-testdata-ci` for opt-in regression and tuning sweeps.
- `reference/decode-orc/` contains the downstream consumer expected to decode
  compressed files. Treat it as a compatibility reference.
- `reference/flac/` contains the Xiph.org FLAC reference source. Treat it as an
  implementation reference, not vendored project code.
- `reference/flac-test-files/` contains reference FLAC-encoded files for future
  decoder/compatibility regression work.
- `reference/rfc9639.txt` is the local copy of the FLAC RFC and should be treated
  as the specification reference for native FLAC bitstream work.
- All reference trees and legacy helper inputs remain ignored by Git and should
  not be edited unless the change is intentional and documented.

## Implementation Checkpoint - 2026-07-04

The project now has a committed C++20/CMake CLI named `ld-compress-ng`.

Implemented:

- Native LDS 10-bit pack/unpack conversion.
- CPU compression to Ogg FLAC `.ldf` using `libFLAC` and `libogg`.
- Decompression from Ogg FLAC and native FLAC to packed `.lds`.
- MD5-based verification, optionally against an original `.lds`.
- `compress`, `decompress`, `verify`, `convert`, `bench`, and `devices`
  subcommands.
- Optional OpenCL device enumeration. OpenCL compression remains reserved and
  fails before writing output.
- Native FLAC writer primitives, STREAMINFO, frame headers, CRC handling, and
  native `.flac.ldf` output.
- Experimental `native-verbatim` backend.
- Experimental scalar `native-fixed` backend with constant, fixed/Rice,
  LPC/Rice, and verbatim subframe selection.
- Native wasted-bits support for the low zero bits in LDS-derived PCM.
- Rice partition search, scalar LPC order search, LPC coefficient precision
  control, and frame sample count control.
- Frame-level threading for native FLAC encoding with ordered output and bounded
  in-flight work.
- Native decision stats for subframe type, fixed/LPC predictor order, Rice
  partition order, and wasted-bit counts.
- Generated test fixtures, opt-in real-fixture regression tests, and a
  real-fixture tuning sweep helper at `tools/sweep_real_fixtures.py`.

Current default native tuning values:

- Frame samples: `4608`.
- Maximum LPC order: `12`.
- LPC coefficient precision: `10`.
- Maximum Rice partition order: `4`.
- Thread count: `1`, unless explicitly set with `--threads`.

Real-fixture sweep result:

- Broad sweep artifact paths are under ignored `build/real-fixture-sweeps/`.
- Current broad sweep winner across the six local real fixtures:
  `threads=8`, `frame=4608`, `lpc=12`, `prec=10`, `rice=4`.
- Aggregate native-fixed size after LPC quantization candidate selection:
  `81,329,035` bytes, still about `+1.55%` larger than CPU/libFLAC for the same
  fixtures.
- The broad sweep justified changing the native LPC coefficient precision default
  from `12` to `10`, but the native scalar encoder still needs algorithmic work
  rather than only knob tuning.

Immediate engineering focus:

- Improve native LPC/Rice encoding quality using `reference/rfc9639.txt`,
  `reference/flac/`, and `reference/decode-orc/` as read-only references.
- Validate that native `.flac.ldf` files remain compatible with libFLAC and,
  when practical, with `reference/decode-orc/`.
- Add targeted tests from `reference/flac-test-files/` only when they are useful
  for this compressor's native FLAC surface.
- Keep CPU/libFLAC Ogg `.ldf` as the production default until native/GPU output
  is both compatible and competitive.

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
ld-compress-ng compress [--backend cpu|native-verbatim|native-fixed|opencl] INPUT [OUTPUT]
ld-compress-ng decompress INPUT [OUTPUT]
ld-compress-ng verify INPUT [--source ORIGINAL.lds]
ld-compress-ng convert --pack|--unpack INPUT [OUTPUT]
ld-compress-ng bench [--threads 1,4,8] INPUT
ld-compress-ng devices
```

Initial behavior:

- `compress` defaults to CPU compression using Ogg FLAC-compatible `.ldf` output.
- `compress --backend cpu|native-verbatim|native-fixed|opencl` should select
  between the implemented CPU path, experimental native FLAC writer paths, and
  the later OpenCL-native FLAC encoder. Until the GPU encoder exists, `opencl`
  is a reserved backend name that must fail before writing output.
- `decompress` accepts existing `.ldf`, `.raw.oga`, and `.flac.ldf` inputs where
  supported by the implemented decoder path.
- `verify` reports hashes for the compressed input and the decompressed/repacked
  LDS stream, and can compare against an original `.lds` file when provided.
- `convert` exposes the LDS packing/unpacking logic directly for diagnostics and
  test fixtures.
- `bench` compares the CPU/libFLAC path with native FLAC backends across selected
  thread counts, using temporary outputs so performance and compression-ratio
  checks do not require hand-managed files. It supports native tuning sweeps over
  frame size, LPC order, LPC coefficient precision, Rice partition order, and
  thread count.
- `devices` lists available OpenCL platforms/devices when OpenCL support is
  built, and remains an enumeration scaffold until GPU compression exists.

Default output naming should preserve existing conventions unless explicitly
overridden:

- CPU compressed output: `INPUT_BASENAME.ldf`
- Native FLAC output, including GPU compressed output later: `INPUT_BASENAME.flac.ldf`
- Decompressed output: `INPUT_BASENAME.lds`

The tool should refuse to overwrite existing outputs unless `--overwrite` is
provided.

## Implementation Phases

### Phase 1: CPU Path and Test Harness

- Create a focused C++20/CMake project at the repository root. Done.
- Implement native LDS packing and unpacking from the `ld-decode-tools`
  converter algorithm, without Qt. Done.
- Implement CPU Ogg FLAC compression via `libFLAC` and `libogg`. Done.
- Implement decompression for the formats needed to round-trip current CPU
  output. Done for Ogg FLAC and native FLAC.
- Implement `compress`, `decompress`, `verify`, and `convert`. Done.
- Add generated test fixtures so tests do not require real RF captures. Done.
- Keep the baseline CPU implementation portable and scalar until correctness and
  compatibility are established on both arm64 and amd64/x86_64.

### Phase 2: GPU Backend

- Keep the public compression boundary at packed LDS input stream to compressed
  output file plus conversion stats. Do not expose OpenCL buffers, kernel task
  structs, or FlaLDF subframe internals through the public API.
- Build native FLAC writer primitives before porting the OpenCL encoder,
  starting with bit writing, CRC helpers, STREAMINFO, and verbatim frame output.
  Done for the scalar native writer path.
- Add an experimental native-FLAC verbatim backend to exercise the writer through
  the real CLI before introducing compressed subframes or GPU work. Done.
- Add a scalar fixed-predictor/Rice backend as the first actually compressed
  native FLAC output path, then extend it with conservative Rice partition
  search before optimized partition ranges or GPU residual work. Done.
- Add scalar subframe selection so native FLAC can choose constant, fixed/Rice,
  or verbatim subframes per frame before adding heavier predictors. Done.
- Add FLAC wasted-bits support so native subframes can avoid storing the low
  zero bits that are inherent in unpacked 10-bit LDS samples. Done.
- Add scalar LPC/Rice subframes as the first heavier predictor path before
  porting the FlaLDF/OpenCL task scheduler. Done.
- Add native tuning controls for frame sample count, maximum LPC order, LPC
  coefficient precision, and maximum Rice partition order so FlaLDF-style
  settings can be benchmarked before changing defaults. Done.
- Add native compression stats for frame/subframe decisions so optimization work
  is driven by fixture behavior rather than guesses. Done.
- Add opt-in frame-level threading for native FLAC encoding. Keep output ordered
  by frame number and keep bounded in-flight work so large captures do not turn
  into unbounded memory use. Done.
- Improve scalar native LPC/Rice compression enough to close the remaining
  CPU/libFLAC gap on real fixtures before starting the OpenCL port in earnest.
- Port FlaLDF host-side encoder logic to native C++.
- Reuse or adapt the existing OpenCL kernel from `FlaLDF/`.
- Extend the initial OpenCL platform/device enumeration into explicit device
  selection for GPU compression.
- Add the `devices` subcommand. Done for enumeration scaffolding.
- Preserve current GPU-style native FLAC `.flac.ldf` output unless a deliberate
  format migration is chosen later.
- Treat Metal support on macOS as a later optional backend after the OpenCL path
  and CPU compatibility suite are working.

### Phase 3: Hardening and Packaging

- Maintain Linux and macOS build documentation, including the required CPU
  dependency set and optional OpenCL packages.
- Add CI or a local equivalent for generated fixtures.
- Add performance checks against the old shell pipeline.
- Keep a lightweight benchmark subcommand for local CPU/native backend
  comparisons across arm64 and amd64/x86_64 hosts.
- Keep an opt-in real-fixture regression suite for ignored reference captures so
  default tests remain self-contained while native tuning has a repeatable
  scoreboard. Done.
- Use benchmark sweeps across frame sizes, LPC orders, LPC coefficient
  precisions, Rice partition orders, and thread counts before changing native
  compression defaults. Done for the first default precision change; repeat
  before future default changes.
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
- Verify native FLAC `.flac.ldf` decode parity for the verbatim backend before
  adding compressed native/GPU output.
- Verify native fixed-predictor/Rice output against the same decode/repack
  parity checks, including partitioned residuals, before adding wider partition
  search or GPU acceleration.
- Verify native subframe selection for constant, fixed/Rice, and verbatim
  fallback frames.
- Verify scalar LPC/Rice subframes with libFLAC decode parity before adding GPU
  LPC work.
- Verify native wasted-bits encoding with libFLAC decode parity.
- Verify threaded native FLAC output is byte-for-byte identical to the
  single-threaded output for generated fixtures.
- For the OpenCL phase, test device enumeration, explicit device selection, CPU
  fallback behavior, and decompressed-output parity with the CPU backend.

## Constraints

- Stay inside `/Users/epowell/Development/laserdisc/compress` unless explicit
  permission is granted first.
- Do not edit vendored or cloned source trees (`FlaLDF/`, `ld-decode-tools/`)
  or ignored reference trees under `reference/` unless the change is intentional
  and documented.
- Preserve original license notices when lifting code or porting implementation
  details.
- Keep Linux and macOS compatibility as a first-order constraint, including both
  arm64 and amd64/x86_64 CPU targets.
- Treat `ld-compress`, `FlaLDF/`, `ld-decode-tools/`, and `reference/` contents
  as references until the replacement has tests proving compatible behavior.
- Keep `AGENTS.md` reserved for contributor or agent operating rules, not the
  full project design.

## Agent Operating Notes

The top-level `AGENTS.md` exists and is intentionally short. It contains only
operator rules: read this plan before implementation, stay inside the project
tree unless permission is granted, avoid unintentional edits to reference
sources, preserve license notices, and keep explanations technically direct for
an experienced UNIX/Linux administrator.
