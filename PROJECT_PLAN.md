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
- `reference/ld-decode/` contains the upstream RF decoder and is the direct
  compatibility reference for ingesting FLAC-compressed RF captures. Its loader
  routes `.raw.oga`, `.ldf`, and `.flac` inputs through FFmpeg/PyAV FLAC
  decode, so FlaLDF `.flac.ldf` output follows the `.ldf` path. Its bundled
  `scripts/ld-compress` documents the legacy CPU Ogg FLAC and FlaLDF native
  FLAC workflows that this tool is replacing.
- `reference/decode-orc/` contains the downstream post-RF consumer for decoded
  TBC and CVBS artifacts (`.tbc`, `.tbcy`, `.tbcc`, `.composite`, `.y`, `.c`).
  The current reference tree does not directly read `.ldf`, `.raw.oga`,
  `.flac.ldf`, or FLAC RF captures; treat it as a compatibility reference for
  the eventual decoded-data pipeline, not as a direct compressed-RF reader.
- `reference/FFmpeg/` contains the FFmpeg source tree. Use `libavcodec/flacenc.c`
  and `libavcodec/lpc.c` as read-only references for FFmpeg's FLAC encoder
  heuristics, LPC windowing, coefficient quantization, Rice partitioning, and
  compression-level behavior.
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
- Optional OpenCL device enumeration.
- Explicit OpenCL device-selection plumbing for the GPU backend.
- A separate `opencl_backend` boundary that validates native-FLAC output and
  device selection before running the OpenCL/native-FLAC encoder path.
- Native FLAC writer primitives, STREAMINFO, frame headers, CRC handling, and
  native `.flac.ldf` output.
- Experimental `native-verbatim` backend.
- Experimental scalar `native-fixed` backend with constant, fixed/Rice,
  LPC/Rice, and verbatim subframe selection.
- Native wasted-bits support for the low zero bits in LDS-derived PCM.
- Rice partition search, scalar LPC order search, LPC coefficient precision
  control, and frame sample count control.
- Public scalar `analyze_mono_best_frame()` decision surface for future OpenCL
  analysis parity tests, without exposing private residual or subframe structs.
- FLACCL-compatible mono OpenCL analysis task ABI, selected-task plan builder,
  and device-free tests for the host-side task layout.
- Hardware-optional OpenCL best-method execution smoke for the mono analysis
  task buffers. It compiles and runs a project-local OpenCL reducer when an
  OpenCL device is available, and skips cleanly on hosts without devices.
- Hardware-optional OpenCL fixed/constant analysis smoke using FLACCL-derived
  wasted-bits and residual-size estimation kernels, with LGPL notices preserved
  in the local kernel source.
- Device-free scalar exact fixed/constant OpenCL task analysis that fills the
  FLACCL task ABI with exact wasted-bits, Rice partition, and bit-size choices
  for parity tests against the native scalar selector.
- Hardware-optional OpenCL fixed/constant exact analysis parity against the
  scalar task oracle, including Rice partition search and partition-order
  limit enforcement.
- Public scalar LPC analysis records for best-LPC and per-order LPC candidates,
  exposing coefficient precision, quantization shift, coefficients, wasted
  bits, Rice partition order, and exact bit count without exposing residual
  vectors.
- Device-free FLACCL LPC task oracle that maps scalar per-order LPC analysis
  into task ABI fields with FLACCL coefficient ordering.
- Hardware-optional OpenCL LPC exact residual/Rice analysis parity for
  pre-populated scalar LPC tasks, including multi-order task groups and
  best-method selection.
- Hardware-optional OpenCL mono LPC autocorrelation/coefficient generation for
  one-window all-order task groups, feeding the exact residual/Rice analyzer.
- Device-free scalar generated-LPC task oracle for the current one-window
  OpenCL path, plus hardware-optional validation of generated order, shift,
  coefficient precision, coefficient ordering, and exact-analysis stability.
- Hardware-optional mixed OpenCL generated analysis for encoder-shaped mono
  task groups, combining generated LPC orders with constant and fixed
  candidates before exact Rice analysis and best-method selection.
- OpenCL generated LPC analysis for encoder-shaped task groups now evaluates
  both rectangular and Tukey-windowed autocorrelation candidates, matching the
  scalar native analyzer's first window-family split before exact Rice costing.
- OpenCL generated LPC analysis also uses the two spare FLACCL-style task slots
  for high-order Welch-windowed LPC candidates, keeping the default mixed task
  group at the 32-task OpenCL limit while improving exact-costed candidate
  coverage.
- Scalar native LPC analysis now mirrors that high-order Welch candidate shape
  for the top two configured LPC orders, preserving most of the all-order Welch
  compression gain without the full scalar runtime cost.
- Encoder-facing OpenCL generated frame analysis wrapper that builds mixed
  mono task plans from frame samples and maps best FLACCL tasks to native
  `FlacSubframeDecision` records for future writer integration.
- Native selected-subframe writer bridge that can write exact OpenCL-selected
  constant, fixed/Rice, and LPC/Rice subframes without exposing private writer
  residual/Rice structures, including hardware-optional OpenCL-selected native
  FLAC round-trip coverage.
- Initial OpenCL native FLAC compression backend that analyzes full frames on
  the selected OpenCL device, writes selected native FLAC subframes, handles a
  short final frame through the scalar native selector, and round-trips through
  the CLI on hosts with an available OpenCL device.
- OpenCL opt-in benchmarking through `bench --include-opencl` and the
  real-fixture sweep helper, preserving the default CPU/native-only benchmark
  behavior on hosts without usable OpenCL devices.
- Linux OpenCL validation on `smaug`, Debian 13-era amd64 kernel
  `6.12.94+deb13-amd64`, NVIDIA OpenCL 3.0 CUDA runtime. The OpenCL analysis
  smoke tests compiled and ran on an RTX 4070 SUPER / RTX 5070 Ti host, and the
  full default CTest suite passed.
- Frame-level threading for native FLAC encoding with ordered output and bounded
  in-flight work.
- Native decision stats for subframe type, fixed/LPC predictor order, Rice
  partition order, and wasted-bit counts.
- Native FLAC decode hardening for STREAMINFO presence, RF-shaped 40 kHz mono
  16-bit streams, decoded sample count, and decoded PCM MD5.
- Native FLAC STREAMINFO/frame-header contract tests.
- Opt-in FLAC decoder testbench regression coverage for rejecting non-RF-shaped
  and malformed FLAC inputs cleanly.
- Skip-safe external native-FLAC decode CTest coverage that generates native
  `.flac.ldf` output, checks FFmpeg can probe/decode it as mono 40 kHz FLAC,
  runs the reference `ld-decode` PyAV-based `ldf_reader.py`, and directly
  exercises the reference `make_loader()` suffix dispatch for CPU Ogg `.ldf`,
  CPU Ogg `.raw.oga`, native `.flac.ldf`, and native `.flac`; all available
  paths compare emitted signed 16-bit mono PCM against the expected
  LDS-unpacked samples.
- Opt-in real-fixture `ld-decode` loader compatibility coverage that compresses
  ignored `.lds` captures to native `.flac.ldf`, verifies reference
  `make_loader()` dispatch for `.flac.ldf` and `.flac`, and compares decoded
  PCM windows against LDS-unpacked samples without materializing whole captures
  as PCM. The CTest path is bounded to one fixture for both scalar native and
  OpenCL backends; the helper mode can run the full fixture tree and optionally
  include CPU Ogg `.ldf`/`.raw.oga` suffixes.
- A build-only OpenCL/scalar frame decision diagnostic,
  `compare_opencl_scalar_frames`, that unpacks real `.lds` fixtures, analyzes
  identical full-frame sample windows through scalar native and OpenCL
  generated-LPC paths, and reports the first per-frame decision/bit-count,
  LPC coefficient, and OpenCL-selected native writer recost mismatches as CSV.
- Generated test fixtures, opt-in real-fixture regression tests, and a
  real-fixture tuning sweep helper at `tools/sweep_real_fixtures.py`.

Current default native tuning values:

- Frame samples: `4608`.
- Maximum LPC order: `12`.
- LPC coefficient precision: `12`.
- Maximum Rice partition order: `5`.
- Thread count: `1`, unless explicitly set with `--threads`.

Real-fixture sweep result:

- Broad sweep artifact paths are under ignored `build/real-fixture-sweeps/`.
- Latest pinned current-default comparison:
  `build/real-fixture-sweeps/real-fixture-sweep-20260705-102155.{csv,md}`.
- Current default target across the six local real fixtures:
  `threads=1`, `frame=4608`, `lpc=12`, `prec=12`, `rice=5`.
- Aggregate native-fixed size after Tukey plus top-two-order Welch-windowed LPC
  candidates: `79,867,690` bytes, about `-0.27%` smaller than CPU/libFLAC for
  the same fixtures. The broader scalar all-order Welch experiment reached
  `79,865,754` bytes but took `274.760` sweep seconds; the top-two-order shape
  keeps nearly the same size result at `210.522` sweep seconds in the latest
  pinned comparison.
- Aggregate OpenCL size after adding Tukey plus two high-order Welch generated
  LPC candidates: `79,952,087` bytes, about `-0.17%` smaller than CPU/libFLAC
  and down from the Tukey-only OpenCL aggregate of `80,443,214` bytes
  (`+0.44%`) and the pre-Tukey OpenCL aggregate of `81,217,362` bytes
  (`+1.41%`). In the latest pinned comparison, OpenCL took `182.213` sweep
  seconds versus CPU/libFLAC at `2.414` seconds and scalar native-fixed at
  `210.522` seconds.
- Current default aggregate decision stats: scalar native-fixed and OpenCL both
  choose LPC/Rice for all `26,037` frames with wasted bits `6`; scalar chooses
  LPC orders `12:14408,11:10406,10:567,8:298`, while OpenCL chooses
  `12:14509,11:10389,8:462,10:190`. Scalar Rice partition orders are
  `0:10548,5:7181,4:6664,3:117`; OpenCL's are
  `0:11441,5:6935,4:6181,3:115`.
- Initial per-frame diagnostic on `ntsc/ggv-ntsc-mb-v2800.lds` shows the
  OpenCL/scalar gap starts at frame `0`: across `2,026` full frames, scalar
  estimated `45,969,167` subframe bits and OpenCL estimated `46,016,008`
  (`+46,841` bits), with `720` decision-shape mismatches, `2,001` bit-count
  mismatches, `1,721` OpenCL-larger frames, and `280` OpenCL-smaller frames.
- The same diagnostic now confirms that OpenCL-selected subframes recost
  exactly through the native writer on that fixture (`46,016,008` bits,
  `0` recost delta, `0` recost mismatches). Frame `0` has the same LPC/Rice
  shape as scalar but differs in LPC coefficients, and all `2,026` compared
  frames have coefficient differences, so the remaining gap is in generated-LPC
  coefficient generation/quantization rather than native selected-subframe
  writing or Rice recosting.
- Earlier Tukey-only retuning found Rice partition order `6` at `79,914,216`
  bytes, but the current top-two-order Welch result with order `5` is smaller;
  keep `5` as the default speed/size tradeoff.

Immediate engineering focus:

- Continue OpenCL-heavy development on the Linux/NVIDIA host now that macOS has
  no usable local OpenCL device. Treat OpenCL as the Linux-first GPU path and
  keep Metal for macOS as a later optional backend.
- Continue native FLAC compatibility hardening using `reference/rfc9639.txt`
  and `reference/flac/` as read-only references.
- Use `reference/ld-decode/` as the direct compatibility target for compressed
  RF input (`.ldf`, `.raw.oga`, and FlaLDF `.flac.ldf`). Keep
  `reference/decode-orc/` for later decoded TBC/CVBS pipeline compatibility.
- The current Linux host has `ffmpeg`/`ffprobe`, plus PyAV and the full
  reference `ld-decode` loader dependency set available through
  `/home/epowell/.pyenv/versions/3.13.13/envs/ld/bin/python`. With CMake
  configured using that `Python3_EXECUTABLE`, `ffmpeg_native_flac_compat`,
  `ld_decode_pyav_compat`, and `ld_decode_loader_compat` pass against
  generated compatibility fixtures. The opt-in
  `ld_decode_loader_real_fixture_compat` CTest passes against the first local
  real fixture, and the helper mode has passed against all six local real
  fixtures in native `.flac.ldf`/`.flac` mode. The opt-in
  `ld_decode_loader_opencl_real_fixture_compat` CTest also passes on the first
  local real fixture, and the helper mode has passed against all six local real
  fixtures using OpenCL-produced native `.flac.ldf`/`.flac` output.
- Use `reference/FFmpeg/` as an additional read-only FLAC encoder heuristic
  reference when evaluating future Welch-window, coefficient refinement, or
  higher-order LPC experiments.
- Bounded FFmpeg-style LPC coefficient perturbation was tested as a scalar-only
  default-path experiment after reviewing FFmpeg `multi_dim_quant`. On the
  two-fixture gate, refining the top two LPC orders improved native bytes from
  `10,061,741` to `10,054,933` but raised elapsed time from `28.380` to
  `56.515` seconds; refining only the top order reached `10,055,417` bytes at
  `43.152` seconds. Do not enable it by default in this form; revisit only
  behind an explicit higher-compression tuning mode or after changing the
  OpenCL task shape.
- Do not replace OpenCL generated-LPC independent coefficient rounding with
  error-feedback rounding wholesale. A two-fixture post-Tukey check regressed
  slightly; revisit it only as an additional exact-costed candidate or as part
  of a broader generated-LPC task-shape change.
- Add targeted tests from `reference/flac-test-files/` only when they are useful
  for this compressor's native FLAC surface. Done for the first rejection-focused
  opt-in suite; expand only as native FLAC compatibility work needs it.
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
ld-compress-ng bench [--threads 1,4,8] [--include-opencl] INPUT
ld-compress-ng devices
```

Initial behavior:

- `compress` defaults to CPU compression using Ogg FLAC-compatible `.ldf` output.
- `compress --backend cpu|native-verbatim|native-fixed|opencl` should select
  between the implemented CPU path, experimental native FLAC writer paths, and
  the experimental OpenCL-native FLAC encoder.
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
  thread count, plus opt-in OpenCL rows with `--include-opencl`.
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
- Add Tukey-windowed LPC analysis candidates alongside rectangle/no-window
  analysis, using exact residual cost to choose the best predictor. Done.
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
- Start the GPU port with a mono OpenCL analysis path that compares
  wasted-bits, fixed/LPC, Rice partition, and best-method decisions against the
  scalar native-fixed encoder before exposing GPU residual/Rice bitstream
  writing.
- For the first mono OpenCL analysis slice, feed host-unpacked `int32_t` mono
  samples directly, skip FLACCL stereo/channel decorrelation, mirror the
  FLACCL task struct layout exactly, and run the analysis kernels through
  best-method selection only. Treat FLACCL residual-size estimates as heuristic
  until parity with the scalar exact-cost selector is characterized. Done for
  the host-side task ABI, selected-task plan builder, and best-method execution
  smoke. Done for the first fixed/constant wasted-bits and residual-size
  estimation path using the FLACCL CPU-style OpenCL kernels. Done for a
  device-free scalar exact fixed/constant task analyzer with Rice partition
  search, so later kernels have a task-ABI parity oracle on hosts without
  OpenCL devices. Done for exact fixed/constant OpenCL kernel analysis parity
  against that scalar oracle, including Rice partition search and partition
  order limits. Done for scalar best/per-order LPC analysis records and a
  device-free FLACCL LPC task oracle with coefficient-order conversion. Done
  for OpenCL exact LPC residual/Rice partition analysis of pre-populated LPC
  tasks against the scalar oracle, including multi-order task groups and
  best-method selection. Done for a first mono one-window OpenCL LPC
  autocorrelation/coefficient-generation path that fills all order slots before
  exact residual/Rice analysis. FLACCL-style multi-window generation, heuristic
  order/precision pruning, and integration into the encoder path still need to
  be ported. Kernel code copied or adapted from FLACCL must keep the original
  LGPL-2.1-or-later notices and local modification notes. The current
  fixed/constant, pre-populated LPC, and generated-LPC OpenCL analysis tests
  have been validated on Linux/NVIDIA hardware; macOS currently has no local
  OpenCL device for runtime kernel validation.
- Extend the initial OpenCL platform/device enumeration into explicit device
  selection for GPU compression. Done for CLI plumbing and metadata selection;
  real compression still awaits the FlaLDF-derived encoder port.
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
  compression defaults. Done for the first default precision change and the
  Tukey-windowed LPC retune; repeat before future default changes.
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
- Verify native FLAC STREAMINFO/frame header fields and reject decoded streams
  whose sample count or PCM MD5 does not match STREAMINFO.
- Run opt-in FLAC decoder testbench checks against `reference/flac-test-files/`
  to verify clean rejection of valid audio FLAC files that are not RF-shaped
  inputs, malformed metadata, and raw FLAC frame streams.
- For the OpenCL phase, test device enumeration, explicit device selection, CPU
  fallback behavior, and decompressed-output parity with the CPU backend.
- Before GPU output is enabled, add analysis-parity tests that run the
  FlaLDF-derived mono OpenCL kernels against generated frames and compare their
  selected subframe decisions with the scalar native-fixed path.
- Keep scalar analysis parity coverage in `test_flac_native_writer` so
  `analyze_mono_best_frame()` and `write_mono_best_frame()` agree before any
  OpenCL analyzer result is trusted.

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
