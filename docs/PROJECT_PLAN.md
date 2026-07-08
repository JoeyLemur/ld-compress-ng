# LaserDisc RF Compression Project Plan

## Summary

This project replaces the existing `ld-compress` shell pipeline with a targeted,
self-contained compression tool for LaserDisc RF captures. The project focuses
only on compression, decompression, verification, and the LDS sample-format
conversion needed for those operations.

The preferred implementation is a C++20/CMake command-line tool that targets Linux
and macOS on both arm64 and amd64/x86_64 CPUs. It should avoid depending at
runtime on `ffmpeg`, `.NET`, Mono, `flaldf`, `pv`, `openssl`, `xxd`, or an
external `ld-lds-converter` command. System libraries such as `libFLAC`,
`libogg`, OpenCL, and Vulkan are acceptable.

## Reference And Legacy State

- `ld-compress` is a shell script that orchestrates several external tools.
- Legacy CPU compression streams LDS input through `ld-lds-converter -u`, then
  through `ffmpeg` as signed 16-bit mono PCM at 40 kHz, producing Ogg FLAC output.
- Legacy GPU compression streams LDS input through `ld-lds-converter -r -u`,
  then through `flaldf`, producing native FLAC-style `.flac.ldf` output.
- Decompression streams compressed input through `ffmpeg`, then repacks the
  resulting signed 16-bit PCM with `ld-lds-converter -p`.
- Verification computes hashes over both the compressed file and the decompressed,
  repacked LDS data.
- `FlaLDF/` contains the C# and OpenCL GPU encoder implementation that should be
  treated as a reference implementation and source for native OpenCL backend
  work.
- `ld-decode-tools/` contains the LDS sample converter logic. The relevant
  conversion itself has been lifted into this project without bringing along the
  full Qt-based `ld-decode-tools` build.
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
- Compatibility/debug `native-verbatim` backend.
- Scalar `native-fixed` backend with constant, fixed/Rice,
  LPC/Rice, and verbatim subframe selection.
- Native wasted-bits support for the low zero bits in LDS-derived PCM.
- Rice partition search, scalar LPC order search, LPC coefficient precision
  control, and frame sample count control.
- Public scalar `analyze_mono_best_frame()` decision surface for accelerator
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
- Device-free scalar generated-LPC task oracle for the early one-window OpenCL
  path, plus hardware-optional validation of generated order, shift,
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
  `FlacSubframeDecision` records for writer integration.
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
- Native FLAC decode now rejects STREAMINFO-declared non-RF sample counts before
  writing decoded LDS output, and preserves the first decoder validation error
  instead of overwriting it with a later libFLAC status.
- The `decompress` CLI writes to a same-directory temporary output and renames
  it into place only after decode and late FLAC validations succeed, so bad
  STREAMINFO MD5/sample-count inputs do not replace an existing `.lds` output.
- Native/OpenCL FLAC encoders now keep STREAMINFO min/max block sizes in the
  RFC-required `16..65535` range and exclude short final LDS tails from the
  minimum block-size bound.
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
- The generated external native-FLAC decode fixture now includes a 4-sample
  final short frame (`2 * 4608 + 4` samples), so FFmpeg and the reference
  `ld-decode` loader cover the same short-tail STREAMINFO edge as the native
  writer tests. The standalone `ldf_reader.py` check accepts only the exact
  expected PCM prefix plus small zero padding from PyAV's final plane buffer.
- Compression CLI validation now rejects explicit no-op backend option
  combinations: `--level` is CPU/libFLAC-only, CPU/libFLAC rejects native
  tuning knobs even when they spell default values, and `native-verbatim`
  rejects predictive LPC/Rice knobs that do not affect verbatim frames.
- Opt-in real-fixture `ld-decode` loader compatibility coverage that compresses
  ignored `.lds` captures to native `.flac.ldf`, verifies reference
  `make_loader()` dispatch for `.flac.ldf` and `.flac`, and compares decoded
  PCM windows against LDS-unpacked samples without materializing whole captures
  as PCM. The CTest path is bounded to one fixture for both scalar native and
  OpenCL backends; the helper mode can run the full fixture tree and optionally
  include CPU Ogg `.ldf`/`.raw.oga` suffixes.
- An opt-in build OpenCL/scalar frame decision diagnostic,
  `compare_opencl_scalar_frames`, that unpacks real `.lds` fixtures, analyzes
  identical full-frame sample windows through scalar native and OpenCL
  generated-LPC paths, and reports the first per-frame decision/bit-count,
  LPC coefficient, LPC candidate-family, and OpenCL-selected native writer
  recost mismatches as CSV.
- Generated test fixtures, opt-in real-fixture regression tests, and a
  real-fixture tuning sweep helper at `tools/sweep_real_fixtures.py`.
- A local validation matrix helper at `tools/check_local_matrix.py` that runs
  isolated default and no-OpenCL configure/build/test lanes, optional FLAC
  testbench and real-fixture lanes, and device smoke checks from ignored build
  directories.

Current default native tuning values:

- Frame samples: `4608`.
- Maximum LPC order: `12`.
- LPC coefficient precision: `12`.
- Maximum Rice partition order: `5`.
- Thread count: `1`, unless explicitly set with `--threads`.

Real-fixture sweep result:

- Broad sweep artifact paths are under ignored `build/real-fixture-sweeps/`.
- Pinned exact current-default comparison:
  `build/real-fixture-sweeps/real-fixture-sweep-20260705-102155.{csv,md}`.
- Current default target across the six local real fixtures:
  `threads=1`, `frame=4608`, `lpc=12`, `prec=12`, `rice=5`.
- Aggregate native-fixed size after Tukey plus top-two-order Welch-windowed LPC
  candidates: `79,867,690` bytes, about `-0.27%` smaller than CPU/libFLAC for
  the same fixtures. The broader scalar all-order Welch experiment reached
  `79,865,754` bytes but took `274.760` sweep seconds; the top-two-order shape
  keeps nearly the same size result at `210.522` sweep seconds in that
  pinned comparison.
- Aggregate OpenCL size after adding Tukey plus two high-order Welch generated
  LPC candidates: `79,952,087` bytes, about `-0.17%` smaller than CPU/libFLAC
  and down from the Tukey-only OpenCL aggregate of `80,443,214` bytes
  (`+0.44%`) and the pre-Tukey OpenCL aggregate of `81,217,362` bytes
  (`+1.41%`). In that pinned comparison, OpenCL took `182.213` sweep
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
- Candidate provenance on that diagnostic shows frame `0` is already a
  different candidate-family choice: scalar picks order-12 Tukey with
  error-feedback quantization at `23,085` bits, while OpenCL picks order-12
  Welch with round-to-even quantization at `23,111` bits. In the first `32`
  frames, OpenCL commonly picks the high-order Welch slot while scalar often
  picks Tukey, and recost still matches OpenCL exactly. The follow-up
  candidate and Tukey trace probes below closed this as a tuning/heuristic
  difference rather than a correctness issue.
- The frame `0`, order-12 candidate dump confirms scalar's best Tukey
  candidate recosts to `23,085` bits while OpenCL's same-order Tukey task
  recosts to `23,163` bits, so OpenCL rejects Tukey and chooses Welch at
  `23,111` bits. OpenCL rectangular is slightly better than scalar rectangular,
  and OpenCL Welch is within `3` bits of scalar Welch/error-feedback, so the
  current first-frame regression is concentrated in Tukey generated-LPC
  coefficient parity.
- A focused Tukey generation trace for frame `0`, order `12` shows shifted
  samples match exactly, Tukey weights differ only at float precision
  (`~2.97e-08` max), and windowed samples differ by only `~1.10e-05`.
  The first meaningful split is OpenCL-style float autocorrelation/Levinson:
  lag-0 autocorrelation differs by about `282` on a `24,068,158` scalar value,
  and that is amplified into pre-quantized LPC coefficient deltas up to about
  `0.885`, yielding Tukey quantized coefficient deltas up to `226`. Treat this
  as a float-precision/conditioning limitation rather than an obvious formula
  bug; do not chase a production fix unless a later task explicitly chooses a
  higher-precision or extra-candidate OpenCL design.
- Earlier Tukey-only retuning found Rice partition order `6` at `79,914,216`
  bytes, but the current top-two-order Welch result with order `5` is smaller;
  keep `5` as the default speed/size tradeoff.
- OpenCL correctness is accepted for the current Linux/NVIDIA target: device
  enumeration works, OpenCL-specific CTests pass, reference `ld-decode` loader
  compatibility passes, a direct real-fixture `compress --backend opencl`
  followed by `verify --source` matches the original LDS MD5, and OpenCL
  selected subframes recost exactly through the native writer
  (`opencl_recost_delta_bits=0`, `opencl_recost_mismatches=0`).
- Across that six-fixture exact sweep, the raw LDS inputs total
  `149,954,560` bytes, CPU/libFLAC outputs total `80,086,984` bytes, scalar
  native-fixed outputs total `79,867,690` bytes, and OpenCL outputs total
  `79,952,087` bytes. OpenCL is smaller than CPU/libFLAC by `134,897` bytes and
  larger than scalar native-fixed by `84,397` bytes, which is good enough for
  now; stop OpenCL LPC parity tuning unless a future task explicitly chooses a
  higher-compression or higher-precision OpenCL design.

## 1.1 Vulkan Acceleration History

The 1.1 development branch targeted Linux-first Vulkan compute acceleration for
native FLAC compression. NVIDIA is the local validation target; AMD support is a
design requirement through standard Vulkan compute features, but is not a
release-blocking hardware validation requirement until AMD hardware is
available.

Implemented during the first 1.1 checkpoint:

- Optional `LDCOMPRESS_ENABLE_VULKAN` CMake plumbing. Vulkan support is enabled
  only when Vulkan development files and `glslangValidator` are found; otherwise
  the build keeps working and reports `Vulkan support: not built`.
- `vulkan_devices` enumeration and selection APIs with no-Vulkan stubs, matching
  the OpenCL device-selection pattern.
- `ld-compress-ng devices` now reports grouped OpenCL and Vulkan sections,
  including `shaderInt64` availability for the exact-cost Vulkan analysis path.
- The CLI recognizes `--backend vulkan`, `--device`, and `--vulkan-device`.
  Vulkan compression now writes compatible native FLAC. `--lpc-order 0` runs
  fixed/Rice analysis on Vulkan compute, and the default path generates
  OpenCL-shaped LPC/window/autocorrelation candidates on Vulkan before exact
  costing and writing selected subframes through the shared accelerated host
  flow.
- `test_vulkan_devices` covers disabled-build behavior and runtime device
  selection when Vulkan support is built.
- A CMake-built GLSL-to-SPIR-V compute smoke shader and `test_vulkan_smoke`
  exercise Vulkan instance/device setup, storage buffers, descriptor binding,
  compute pipeline creation, command submission, and host readback.
- A CMake-built, embedded GLSL-to-SPIR-V exact analysis shader and
  `test_vulkan_analysis` compare Vulkan fixed/Rice and pre-populated LPC costing
  against the scalar task oracle, including Rice partition search and
  partition-order limit behavior.
- The OpenCL native-FLAC backend now uses a shared accelerated native-FLAC host
  flow for one-pass LDS ingest, seek-back STREAMINFO/PCM MD5 finalization, frame
  batching, selected native-subframe writing, scalar tail fallback, and native
  stats. OpenCL owns only OpenCL validation/device selection and the batch
  analyzer callback, giving Vulkan a matching plug-in point.
- The Vulkan backend is wired through that shared host flow, including
  selected-subframe handoff, `verify --source` compatibility, fixed/Rice
  diagnostics through `--lpc-order 0`, and default LPC-enabled compression.
- Vulkan exact analysis now has a persistent session object. Compression creates
  one session per run and reuses the Vulkan instance, device, queue, shader
  module, pipeline, descriptor set, command buffer, fence, and grow-only host
  buffers across batches. The old one-shot analysis functions remain as
  compatibility wrappers for tests and diagnostics.
- Native `--stats` now reports coarse accelerated timing splits for backend
  total time, accelerator setup, LDS ingest, analyzer callback, selected-frame
  writing, accelerator task-plan generation, and accelerator exact analysis.
  Vulkan compression also
  reports GPU queue timestamp splits when the selected compute queue supports
  timestamp queries.
- Vulkan exact analysis now dispatches one workgroup per task and uses
  workgroup reductions for wasted-bit scans, amplitude scans, constant checks,
  and exact Rice bit sums. This keeps the task ABI, selected-task handoff, and
  writer path unchanged while replacing the earlier one-lane-per-task shader
  shape.
- Vulkan generated-LPC analysis now runs on the Vulkan shader path before exact
  costing. The backend builds the same generated-task shape as OpenCL
  (rectangular, Tukey, and spare high-order Welch LPC slots plus
  fixed/constant tasks), fills LPC coefficients on the GPU, and then feeds those
  tasks through the existing exact analyzer. `test_vulkan_analysis` covers
  generated task population, exact-cost stability, and partition-order limits.
- Vulkan compression now uses a best-task-only analyzer path, leaving the full
  analyzed-task readback available for tests and diagnostics but avoiding that
  host transfer in the normal writer path. Vulkan compression batches now use
  `128` frames per analysis submission instead of `32`; the full-result tests
  cover that best-only results still match the compatibility path.
- Vulkan descriptor-bound analysis buffers now use device-local memory for
  samples, tasks, selected-task indexes, best-task output, generated LPC
  windows, autocorrelations, and LPC scratch. Host-visible buffers are retained
  only as upload/readback staging, with explicit transfer/compute/host barriers
  preserving the full analyzed-task diagnostic path.
- Vulkan exact residual/Rice analysis now scores all Rice parameters for a
  partition in one residual pass. This preserves exact costs while avoiding the
  old pattern of recomputing the same LPC/fixed residuals once per candidate
  Rice parameter and then again for the selected parameter.
- Vulkan exact analysis now runs the frame prepare pass for all exact-analysis
  batches, not only generated-LPC batches. Mode-0 exact analysis consumes the
  prepared frame-level wasted-bit and amplitude-bit fields instead of rescanning
  the same frame once per task. The Vulkan analysis tests seed stale `wbits` and
  `abits` into an LPC task to guard that prepare remains the source of truth.
- The shared accelerated native-FLAC host flow now avoids two avoidable CPU-side
  copies: it batches incoming LDS samples directly instead of first building
  per-frame vectors, and it writes selected subframes through a span-based
  selected-frame writer instead of slicing each frame into a fresh vector. This
  applies to both OpenCL and Vulkan backends.
- The local validation matrix helper has a `no-vulkan` lane for optional-build
  regression coverage.
- The real-fixture sweep helper can include Vulkan rows with `--include-vulkan`
  and `--vulkan-device`, and its Markdown report now includes per-fixture and
  aggregate Vulkan sections alongside CPU/native/OpenCL.
- The real-fixture external loader compatibility suite and local matrix helper
  now have an opt-in Vulkan lane through `--include-vulkan-real-fixture` and
  `--vulkan-device`. The lane passed locally with the PyAV/ld-decode Python
  interpreter and NVIDIA Vulkan device `1`.
- Vulkan CLI/device hardening now reports whether each enumerated Vulkan device
  is usable by the backend (`available && shaderInt64`), gives out-of-range
  Vulkan device errors with the visible device count and a `devices` hint, and
  has CLI regression coverage for common Vulkan option mistakes and diagnostic
  text. README and build/testing docs describe the backend-local `--device`
  alias, backend-specific `--opencl-device`/`--vulkan-device` selectors, and
  the `bench` ambiguity rule.

Post-checkpoint Vulkan/OpenCL performance history:

- Treat Vulkan/OpenCL throughput as an architecture problem before any
  shader-level micro-tuning. Earlier NVIDIA RTX 5070 Ti timings on
  `ntsc/issue176.lds` after the Vulkan best-task-only readback, 128-frame
  batching, device-local buffer placement, shared writer-copy cleanup, GPU
  queue timestamp instrumentation, one-pass Rice-parameter costing, prepared
  frame-level `wbits`/`abits` reuse, trusted selected-frame decision writing,
  a Vulkan selected Rice-parameter sidecar, selected-writer timing
  instrumentation, and chunked `BitWriter` output show CPU/libFLAC at about
  `0.129` seconds, scalar native-fixed at `1.641` seconds with `8` threads,
  OpenCL at `10.132` seconds, and Vulkan at `0.711` seconds.
  Vulkan output on that fixture is `4,292,100` bytes: `1,508` bytes larger than
  scalar native-fixed's `4,290,592` bytes, `22,033` bytes smaller than
  CPU/libFLAC's `4,314,133` bytes, and `6,134` bytes smaller than OpenCL's
  `4,298,234` bytes. Before one-pass Rice-parameter costing, a focused
  `compress --backend vulkan --stats` run measured `12` batches, `1.994` total
  backend seconds, `1.394` analyzer seconds, `0.305` selected-frame write
  seconds, and `1.3719` GPU seconds in exact residual/Rice analysis. After the
  one-pass Rice change, the same focused run measured `1.088` total backend
  seconds, `0.122` analyzer seconds, `0.306` selected-frame write seconds, and
  only `0.0979` GPU seconds in exact residual/Rice analysis. After reusing
  prepared frame-level `wbits`/`abits`, the focused stats run measured `1.040`
  total backend seconds, `0.116` analyzer seconds, `0.305` selected-frame write
  seconds, and `0.0952` GPU seconds in exact residual/Rice analysis. After
  trusting the analyzer's selected-frame decision for stats and skipping the
  writer-side subframe bit recost, the focused stats run measured `0.742`
  total backend seconds, `0.119` analyzer seconds, `0.289` selected-frame write
  seconds, and `0.0951` GPU seconds in exact residual/Rice analysis. After
  adding a Vulkan sidecar for selected per-partition Rice parameters, the
  focused bench result stayed at about `0.722` seconds while the stats split
  shifted to about `0.144` analyzer seconds and `0.257` selected-frame write
  seconds; a serial first attempt put this work in `choose_best` and regressed
  badly, so the kept version uses a cooperative one-workgroup-per-frame
  sidecar pass. A selected-writer timing split then showed bitstream
  construction as the largest host writer bucket (`~0.140` seconds), ahead of
  validation/wasted-bit checking (`~0.050` seconds), residual generation
  (`~0.035` seconds), and frame output/CRC (`~0.020` seconds). Chunking
  `BitWriter::write_bits()` and skipping unary zero runs dropped the
  bitstream bucket to `~0.082` seconds, selected-frame write to `~0.196`
  seconds, and the focused Vulkan bench to `~0.711` seconds. A final bounded
  selected-writer pass combined selected validation, wasted-bit checking, and
  sample shifting for fixed/LPC accelerated frames while preserving exact
  selected wasted-bit validation. The focused Vulkan stats run then measured
  `0.660` backend seconds, `0.178` analyzer seconds, `0.193` selected-frame
  write seconds, and `0.163` GPU seconds; selected-writer shift time is now
  folded into validation and reports as `0.000` seconds. The matching focused
  bench result was CPU/libFLAC `4,314,133` bytes in `0.137` seconds,
  scalar native-fixed with `8` threads `4,290,592` bytes in `1.668` seconds,
  OpenCL `4,298,234` bytes in `9.872` seconds, and Vulkan `4,292,100` bytes
  in `0.709` seconds. On the NVIDIA fixture path, PCIe transfer/readback is not
  the primary bottleneck; the remaining writer time is mostly bitstream
  construction, validation/shift/wasted-bit checking, residual generation, and
  ordinary frame output/CRC.
- The earlier six-fixture sweep at frame size `4608`, LPC order `12`,
  coefficient precision `12`, Rice partition order `5`, native-fixed `8`
  threads, OpenCL device `1`, and Vulkan device `1`, before the OpenCL
  throughput cleanup, produced aggregate sizes: CPU/libFLAC `80,086,984`
  bytes, scalar native-fixed `79,867,690` bytes in `30.168` seconds, OpenCL
  `79,952,087` bytes in `180.523` seconds, and Vulkan `79,892,217` bytes in
  `37.469` seconds. At that point Vulkan was `24,527` bytes larger than scalar
  native-fixed, `59,870` bytes smaller than OpenCL, and much faster than OpenCL
  on the NVIDIA validation device.
- Move back to broader 1.1 hardening instead of chasing small writer buckets.
  The readback, trusted-decision, sidecar, bit-writer, and selected
  validation/shift changes are also useful for OpenCL: normal OpenCL
  compression discards full analyzed tasks too, so a future OpenCL best-only
  analyzer path and writer handoff could skip avoidable readback/recost work
  while keeping the current full-result APIs for parity diagnostics.
- The first OpenCL throughput cleanup applied those reusable host-flow lessons:
  OpenCL compression now batches `128` frames, keeps a generated-analysis
  session alive across batches, reads back only best tasks for the normal
  generated-LPC compression path, reuses grow-only OpenCL buffers, and fills the
  same coarse task-plan/exact-analysis timing buckets as Vulkan. On
  `ntsc/issue176.lds` with NVIDIA device `1`, the focused `compress --backend
  opencl --stats` run improved from about `10.30` wall seconds
  (`46` batches, `9.735` analyzer seconds) to about `3.18` wall seconds
  (`12` batches, `2.587` exact-analysis seconds) with unchanged `4,298,234`
  byte output. The matching bench row improved to `2.969` seconds. At that
  point, remaining OpenCL time was dominated by the exact residual/Rice kernel
  shape, not
  writer work, host allocation churn, or full-task readback; the next OpenCL
  performance step is a bounded parallel exact/Rice analysis kernel pass,
  modeled after the Vulkan workgroup reduction design.
- The bounded OpenCL exact/Rice kernel pass now dispatches one `64`-lane
  workgroup per selected task and scores all Rice parameters for each partition
  in one residual pass, preserving the task ABI, selected-task handoff, and
  compressed output. The focused `compress --backend opencl --stats` run on
  `ntsc/issue176.lds` with NVIDIA OpenCL device `1` improved again to about
  `1.86` wall seconds, `12` batches, and `0.701` analyzer/exact-analysis
  seconds with unchanged `4,298,234` byte output. The matching bench row is now
  `1.097` seconds, faster than scalar `native-fixed --threads 8` at `1.668`
  seconds on that fixture, while Vulkan remains faster at `0.712` seconds.
- OpenCL `--stats` now reports generated-analysis stage timings. On the same
  fixture/device, the split measured `0.0029` seconds upload, `0.0007` wasted
  bits, `0.5875` generated autocorrelation, `0.0004` LPC solve, `0.0002`
  quantization, `0.1092` exact/Rice analysis, `0.0002` choose-best, and
  `0.0001` readback. The matching no-stats bench row stayed at `1.089`
  seconds. The remaining OpenCL analyzer bottleneck is generated
  autocorrelation, not exact/Rice, readback, choose-best, or LPC quantization.
- OpenCL generated autocorrelation now uses the same broad workgroup shape as
  Vulkan: one `64`-lane workgroup per frame/window/lag, with local float
  reductions. This intentionally changes floating-point summation order, so
  generated LPC coefficients and exact selected candidates are not byte-identical
  to the prior serial OpenCL path. On `ntsc/issue176.lds` with NVIDIA OpenCL
  device `1`, the focused stats run measured about `0.011` seconds in
  autocorrelation and `0.126` analyzer seconds total, down from `0.587` and
  `0.702` respectively. The matching no-stats bench row improved to `0.516`
  seconds with `4,291,949` byte output, source verification passed, and this is
  slightly smaller/faster than the then-current Vulkan row on that fixture
  (`4,292,100` bytes in `0.715` seconds). The largest remaining OpenCL time is
  now outside generated analysis, primarily selected-frame writer work and
  backend/host overhead.
- The follow-up six-fixture sweep after cooperative OpenCL autocorrelation wrote
  `build/real-fixture-sweeps/real-fixture-sweep-20260706-231838.{csv,md}`.
  Aggregate CPU/libFLAC output was `80,086,984` bytes. Best scalar native-fixed
  stayed `79,867,690` bytes in `29.960` seconds with `threads=8`,
  `frame=4608`, `lpc=12`, `prec=12`, and `rice=5`. Best OpenCL is now
  `79,892,119` bytes in `7.909` seconds with the same frame/LPC/Rice settings,
  `-0.24%` vs CPU/libFLAC and only `24,429` bytes larger than native-fixed.
  This confirms the OpenCL autocorrelation speedup holds across the local real
  fixtures; OpenCL is now near native-fixed size and materially faster than
  scalar native-fixed on this validation set.
- A refreshed combined six-fixture sweep with OpenCL and Vulkan enabled wrote
  `build/real-fixture-sweeps/real-fixture-sweep-20260706-232419.{csv,md}`.
  With OpenCL device `1` and Vulkan device `1` both selecting the RTX 5070 Ti,
  aggregate output sizes were CPU/libFLAC `80,086,984` bytes, scalar
  native-fixed `79,867,690` bytes in `29.997` seconds, OpenCL `79,892,119`
  bytes in `7.938` seconds, and Vulkan `79,892,217` bytes in `9.370` seconds.
  Vulkan remains source-compatible and essentially tied with OpenCL on
  compressed size (`98` bytes larger across all fixtures), but OpenCL is now
  faster on this validation sweep after the cooperative autocorrelation work.
  Continue Vulkan 1.1 hardening and compatibility coverage before reopening
  performance work.
- The Vulkan real-fixture local matrix lane now passes from a GPU-visible
  context using `tools/check_local_matrix.py` with `--include-vulkan-real-fixture`
  and `--vulkan-device 1`, plus the PyAV-capable `ld-decode` Python
  interpreter. The selected tests passed: scalar real fixtures, real-fixture
  `ld-decode` loader compatibility, and Vulkan real-fixture `ld-decode` loader
  compatibility on the RTX 5070 Ti. A sandboxed run of the same helper can see
  only CPU Vulkan/llvmpipe and report the requested NVIDIA device as
  unavailable, so accelerator matrix lanes must run from a GPU-visible context.
  The Python real-fixture compatibility helper now mirrors the Vulkan backend's
  implicit selection policy: explicit devices are honored when backend-usable,
  otherwise it picks the first backend-usable discrete GPU, then the first
  backend-usable non-CPU device, and skips CPU Vulkan unless it was explicitly
  requested. The parser policy is covered by the
  `external_decode_device_selection` CTest.
- The full GPU-visible local matrix now passes with default, no-OpenCL,
  no-Vulkan, FLAC test-file, and real-fixture lanes enabled via
  `tools/check_local_matrix.py`, `--all-local`, both accelerator real-fixture
  switches, OpenCL device `1`, and Vulkan device `1`. The real-fixture lane ran
  four tests successfully: scalar fixture compression, scalar `ld-decode`
  loader compatibility, OpenCL real-fixture loader compatibility on OpenCL
  device `1`, and Vulkan real-fixture loader compatibility on Vulkan device
  `1`.
- Full accelerator real-fixture roundtrip coverage is now repeatable through
  `tools/roundtrip_real_fixtures.py`. The latest GPU-visible run wrote
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`
  and passed all six local fixtures on OpenCL device `1` and Vulkan device `1`.
  Each row compressed, verified with `verify --source`, decompressed, and
  compared decoded size plus MD5 against the source `.lds`. Aggregate input was
  `149,954,560` bytes; OpenCL output was `79,892,119` bytes with `4.452`
  compress seconds, and Vulkan output was `79,892,217` bytes with `3.938`
  compress seconds.

## Performance Pass Wrap-Up - 2026-07-08

The current accepted speed-focused reference is the GPU-visible sweep at
`build/real-fixture-sweeps/real-fixture-sweep-20260708-145656.{csv,md}`. It
used `threads=8`, `frame=4608`, `lpc=12`, `prec=12`, Rice orders `5,6`, and
`analysis-profile=order-guess-mean-estimate-rice` with OpenCL/Vulkan session
reuse enabled. Treat older timing notes above as historical checkpoint data.

Current aggregate speed-profile results across the six local real fixtures:

| Backend | Rice order | Output bytes | Elapsed time |
| --- | ---: | ---: | ---: |
| CPU/libFLAC | - | `80,086,984` | `2.440s` |
| Native-fixed | `5` | `79,930,556` | `1.649s` |
| Native-fixed | `6` | `79,926,901` | `1.689s` |
| OpenCL | `5` | `79,950,606` | `0.829s` |
| OpenCL | `6` | `79,946,987` | `0.814s` |
| Vulkan | `5` | `79,950,496` | `0.834s` |
| Vulkan | `6` | `79,946,934` | `0.813s` |

The pass accepted broad throughput changes that kept compressed bytes unchanged
for OpenCL/Vulkan on the speed sweep:

- OpenCL now returns selected Rice parameters through the sidecar handoff, so
  the selected-frame writer does not recompute them for fixed/LPC subframes.
- OpenCL generated analysis keeps a reusable session and grow-only buffers for
  compression/sweep reuse.
- The accelerated writer trusts the selected decision for stats and uses faster
  Rice bitstream emission.
- The shared accelerated host flow avoids avoidable frame-vector slicing and
  now pipelines batch analysis: while the GPU analyzes one full batch, the host
  can ingest/MD5 the next batch and then write the previous analyzed frames in
  order. This mirrors libFLAC's main structural advantage: fewer large serial
  phase boundaries.
- OpenCL uses a larger accepted compression batch size of `2048` frames. Vulkan
  remains at `512` frames because larger Vulkan batches were flat or slower.

The final pipeline change moved the current speed-profile rows materially:

- OpenCL Rice order `6`: `1.033` seconds before the final OpenCL batch/pipeline
  pass to `0.814` seconds, with unchanged `79,946,987` bytes.
- Vulkan Rice order `6`: `1.298` seconds before the final pipeline pass to
  `0.813` seconds, with unchanged `79,946,934` bytes.

Rejected experiments from this pass:

- Direct selected-frame Rice emission from shifted samples was correct but
  slower; it moved residual prediction into the bitstream hot path.
- Big-endian frame-output write tweaks were mixed/noisy and not worth carrying.
- Larger accelerated scan chunking and chunk-level group append regressed or
  failed to move the scan bucket.
- `4096`-frame GPU batches rolled over; OpenCL lost part of the `2048` benefit
  and Vulkan regressed substantially. Vulkan `2048` was not kept.

The useful libFLAC lessons are now documented by code and measurements:

- libFLAC uses reusable frame-local workspaces and carries analysis results into
  writing instead of rebuilding large transient state after each decision.
- It shifts wasted bits once per frame, then analyzes/writes the reduced-width
  signal.
- Its normal compression levels prefer guessed orders and mean-derived Rice
  estimates over exhaustive exact searches.
- It avoids our older shape of `scan -> analyze -> write` as three mostly serial
  phases.

Stop this performance pass here unless a new task explicitly reopens broad
tuning. The next high-value speed idea is not a micro-optimization: make LDS
ingest produce reusable per-frame facts such as wasted bits, constant/amplitude
flags, and optionally shifted sample spans, then feed those facts into
OpenCL/Vulkan task planning and the selected writer. A larger follow-up would
make the analyzer produce writer-ready residual/Rice state, but that should be
treated as a separate design pass.

Validation status for the wrap-up checkpoint:

- `cmake --build build` passed.
- Full GPU-visible `ctest --test-dir build --output-on-failure` passed with
  `21/21` tests, including OpenCL/Vulkan analysis, real-fixture compatibility,
  and FLAC test-file coverage.
- Focused OpenCL/Vulkan/real-fixture CTest rerun passed with `9/9` tests.
- Full accelerator real-fixture roundtrip coverage passed for all six local
  fixtures on OpenCL device `1` and Vulkan device `1`, writing
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`.

  | Backend | Input bytes | Output bytes | Compress time | Validation |
  | --- | ---: | ---: | ---: | --- |
  | OpenCL | `149,954,560` | `79,892,119` | `4.452s` | Verified, decompressed, and MD5-matched source `.lds` data. |
  | Vulkan | `149,954,560` | `79,892,217` | `3.938s` | Verified, decompressed, and MD5-matched source `.lds` data. |

- The final focused speed sweep wrote
  `build/real-fixture-sweeps/real-fixture-sweep-20260708-145656.{csv,md}`.

Immediate engineering focus:

- Move on from OpenCL LPC parity tuning. Keep the Linux/NVIDIA OpenCL path under
  regression coverage, keep the Linux-first Vulkan backend covered, treat Metal
  for macOS as a later optional backend, and reopen accelerator tuning only for
  an explicit higher-compression or higher-speed design pass.
- Continue native FLAC compatibility hardening using `reference/rfc9639.txt`
  and `reference/flac/` as read-only references.
- Use `reference/ld-decode/` as the direct compatibility target for compressed
  RF input (`.ldf`, `.raw.oga`, and FlaLDF `.flac.ldf`). Keep
  `reference/decode-orc/` for later decoded TBC/CVBS pipeline compatibility.
- The current Linux host has `ffmpeg`/`ffprobe`, plus PyAV and the full
  reference `ld-decode` loader dependency set available through a
  PyAV-capable Python environment. With CMake
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
- Keep CPU/libFLAC Ogg `.ldf` as the default and recommended CPU compression
  path for maximum compatibility. Treat scalar native-fixed and native-verbatim
  as reference/debug backends for native writer coverage, tuning, and
  accelerator oracle work.

## Recommendation

Build a single native CLI named `ld-compress-ng`, using C++20 and CMake.

C++ is the best fit for this targeted project because:

- The LDS 10-bit packed to 16-bit signed conversion is simple bit-level streaming
  code and already exists in C++ form.
- CPU compression can use `libFLAC` and `libogg` directly instead of shelling out
  to `ffmpeg`.
- The FlaLDF GPU path is OpenCL plus substantial host-side FLAC framing, Rice, and
  LPC code. Porting that host code is more direct in C++ than in Python or Rust.
- A C++/CMake project maps cleanly to both Linux and macOS packaging.

Python remains useful for tests or helper scripts, but not for the main
compressor or the OpenCL backend.

## Proposed CLI

The CLI should use explicit subcommands rather than mirroring the old option soup.

```text
ld-compress-ng compress [--backend cpu|native-verbatim|native-fixed|opencl|vulkan] INPUT [OUTPUT]
ld-compress-ng decompress INPUT [OUTPUT]
ld-compress-ng verify INPUT [--source ORIGINAL.lds]
ld-compress-ng convert --pack|--unpack INPUT [OUTPUT]
ld-compress-ng bench [--threads 8] [--analysis-profile NAME[,NAME...]]
                     [--include-opencl] [--include-vulkan]
                     [--reuse-opencl-session] [--reuse-vulkan-session] INPUT
ld-compress-ng devices
```

Implemented behavior:

- `compress` defaults to CPU compression using Ogg FLAC-compatible `.ldf` output.
- `compress --backend cpu|native-verbatim|native-fixed|opencl|vulkan` selects
  between the implemented CPU/libFLAC path, reference/debug native FLAC writer
  paths, the OpenCL-native FLAC encoder, and the Linux-first Vulkan encoder.
- `decompress` accepts existing `.ldf`, `.raw.oga`, and `.flac.ldf` inputs where
  supported by the implemented decoder path.
- `verify` reports hashes for the compressed input and the decompressed/repacked
  LDS stream, and can compare against an original `.lds` file when provided.
- `convert` exposes the LDS packing/unpacking logic directly for diagnostics and
  test fixtures.
- `bench` compares the CPU/libFLAC path with native FLAC backends across selected
  thread counts, using temporary outputs so performance and compression-ratio
  checks do not require hand-managed files. It supports native tuning sweeps over
  frame size, LPC order, LPC coefficient precision, Rice partition order,
  analysis profile, and thread count, plus opt-in OpenCL and Vulkan rows with
  `--include-opencl` and `--include-vulkan`. Accelerator sweeps can reuse one
  analysis session per backend with `--reuse-opencl-session` and
  `--reuse-vulkan-session`.
- `devices` lists available OpenCL and Vulkan devices when support is built and
  provides the backend-local indexes used by accelerated compression.

Default output naming preserves existing conventions unless explicitly
overridden:

- CPU compressed output: `INPUT_BASENAME.ldf`
- Native FLAC output, including accelerated output: `INPUT_BASENAME.flac.ldf`
- Decompressed output: `INPUT_BASENAME.lds`

The tool refuses to overwrite existing outputs unless `--overwrite` is provided.

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
- Add a native-FLAC verbatim backend to exercise the writer through the real CLI
  before introducing compressed subframes or GPU work. Done.
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
  Done for the current exact native reference path.
- Port FlaLDF/FLACCL-style host-side encoder logic to native C++. Done for the
  mono RF analysis/compression path used by OpenCL and Vulkan.
- Reuse or adapt the existing OpenCL kernel from `FlaLDF/`. Done for the
  current fixed/LPC generated analysis kernels, with local LGPL notices kept.
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
  exact residual/Rice analysis. Done for the current encoder-shaped generated
  task set, including rectangular, Tukey, high-order Welch, order-guess, and
  mean-Rice speed-profile variants used by benchmark sweeps. Kernel code copied
  or adapted from FLACCL must keep the original LGPL-2.1-or-later notices and
  local modification notes. The current
  fixed/constant, pre-populated LPC, generated-LPC, and encoder-facing OpenCL
  compression tests have been validated on Linux/NVIDIA hardware; macOS
  currently has no local OpenCL device for runtime kernel validation.
- Extend the initial OpenCL platform/device enumeration into explicit device
  selection for GPU compression. Done for CLI plumbing, metadata selection, and
  the OpenCL-native FLAC compression backend.
- Add the `devices` subcommand. Done for grouped OpenCL and Vulkan enumeration.
- Preserve current GPU-style native FLAC `.flac.ldf` output unless a deliberate
  format migration is chosen later.
- Treat Metal support on macOS as a later optional backend after the OpenCL path
  and CPU compatibility suite are working.

### Phase 3: Hardening and Packaging

- Maintain Linux and macOS build documentation, including the required CPU
  dependency set and optional OpenCL/Vulkan packages.
- Add CI or a local equivalent for generated fixtures. Done for a local helper
  that covers default, no-OpenCL, optional FLAC-testbench, and optional
  real-fixture validation lanes.
- Add performance checks against the old shell pipeline.
- Keep a lightweight benchmark subcommand for local CPU/libFLAC, scalar
  reference, and accelerator comparisons across arm64 and amd64/x86_64 hosts.
- Keep an opt-in real-fixture regression suite for ignored reference captures so
  default tests remain self-contained while native tuning has a repeatable
  scoreboard. Done.
- Use benchmark sweeps across frame sizes, LPC orders, LPC coefficient
  precisions, Rice partition orders, and thread counts before changing native
  compression defaults. Done for the first default precision change and the
  Tukey-windowed LPC retune; repeat before future default changes.
- Document compatibility with historical `.ldf`, `.raw.oga`, and `.flac.ldf`
  files. Done in the README, build/testing notes, and compatibility tests.
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

- Stay inside the active project checkout unless explicit permission is granted
  first.
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
