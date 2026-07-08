# Handoff Notes

Last updated: 2026-07-08, during the paused 1.1.1 publish after full
real-fixture validation and refreshed speed/size documentation.

This file is for maintainer/agent continuity. It is intentionally not installed
by CMake; release-facing installed docs are listed explicitly in
`CMakeLists.txt`.

## Current Repository State

- Active branch: `main`.
- `origin/main` was pushed to the 1.1.1 release-prep commit
  `a390f5a` (`Polish 1.1.1 release notes`) during the first publish attempt.
- Local `main` has one more release-validation/docs update on top of that push.
- Local branch `codex/vulkan-1.1` still exists as release-branch history
  context. It can be kept or deleted later.
- Annotated tag `v1.1.0` has been pushed to origin.
- Annotated tag `v1.1.1` exists locally and has not been pushed to origin.
  Make sure it points at the final release commit before pushing it.
- GitHub release was created as source-only, with no binary assets:
  `https://github.com/JoeyLemur/ld-compress-ng/releases/tag/v1.1.0`
- Project version is `1.1.1` in `CMakeLists.txt`; `ld-compress-ng --version`
  prints `ld-compress-ng 1.1.1`.

## Post-1.1 Mainline Work

- Added benchmark/sweep-only native analysis profiles, including order-guess
  and mean-Rice speed profiles. The normal `compress` command still uses exact
  native analysis.
- Improved OpenCL throughput with selected Rice-parameter handoff, reusable
  generated-analysis sessions, larger accepted compression batches, and
  cooperative generated autocorrelation.
- Pipelined the shared accelerated host flow so the host can ingest/MD5 the next
  batch while the GPU analyzes one batch and then write the previous analyzed
  frames in order.
- Retained Vulkan/OpenCL compatibility through the shared native writer while
  improving accelerator sweep throughput.

Current accepted speed-focused reference:

- Sweep artifact:
  `build/real-fixture-sweeps/real-fixture-sweep-20260708-145656.{csv,md}`.
- Sweep shape: `threads=8`, `frame=4608`, `lpc=12`, `prec=12`, Rice orders
  `5,6`, `analysis-profile=order-guess-mean-estimate-rice`, OpenCL/Vulkan
  session reuse enabled.
- Six-fixture aggregate rows:
  CPU/libFLAC `80,086,984` bytes in `2.440` seconds; native-fixed Rice order
  `6` `79,926,901` bytes in `1.689` seconds; OpenCL Rice order `6`
  `79,946,987` bytes in `0.814` seconds; Vulkan Rice order `6`
  `79,946,934` bytes in `0.813` seconds.
- Validation for the wrap-up checkpoint: `cmake --build build` passed, full
  GPU-visible `ctest --test-dir build --output-on-failure` passed with `21/21`
  tests, focused OpenCL/Vulkan/real-fixture CTest rerun passed with `9/9`
  tests, the all-six-fixture OpenCL/Vulkan roundtrip helper wrote
  `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`,
  and the final focused sweep above completed.

## What 1.1 Shipped

- Linux-first Vulkan native FLAC acceleration backend.
- OpenCL throughput improvements: larger batches, persistent analysis state,
  best-task readback, parallel exact/Rice costing, and cooperative generated
  autocorrelation.
- Shared accelerator host flow used by OpenCL and Vulkan.
- Real-fixture sweep support for OpenCL and Vulkan.
- Full OpenCL/Vulkan real-fixture roundtrip helper:
  `tools/roundtrip_real_fixtures.py`.
- Release/docs cleanup, including LGPL/CUETools notice coverage for the Vulkan
  shader path.

## 1.1 Release Validation That Passed

Required release gate:

```sh
cmake --build build
ctest --test-dir build -LE real-fixtures --output-on-failure
build/ld-compress-ng --version
build/ld-compress-ng --help
cmake -S . -B build-release-cpu-only -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLDCOMPRESS_ENABLE_OPENCL=OFF \
    -DLDCOMPRESS_ENABLE_VULKAN=OFF
cmake --build build-release-cpu-only --parallel
ctest --test-dir build-release-cpu-only -LE real-fixtures --output-on-failure
cmake --install build --prefix /tmp/ld-compress-ng-install-1.1-final
/tmp/ld-compress-ng-install-1.1-final/bin/ld-compress-ng --version
```

Observed results:

- Main build passed with OpenCL and Vulkan enabled.
- Non-real-fixture CTest passed: `17/17`.
- CPU-only lane passed: `16/16`, with PyAV/reference-loader tests skipped in
  that CPU-only build because the PyAV interpreter was not configured there.
- Install smoke passed and installed only release-facing docs:
  `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`,
  `docs/PROJECT_PLAN.md`, `docs/build-and-testing.md`, and
  `docs/release-checklist.md`.

Optional full local gate:

```sh
python3 tools/check_local_matrix.py \
    --build-root build/local-check-release \
    --all-local \
    --include-opencl-real-fixture \
    --opencl-device 1 \
    --include-vulkan-real-fixture \
    --vulkan-device 1 \
    --python-executable /home/epowell/.pyenv/versions/3.13.13/envs/ld/bin/python \
    --jobs 2 \
    --strict-optional
```

Observed result: all selected validation steps passed, including default,
no-OpenCL, no-Vulkan, FLAC test-file, scalar real-fixture, OpenCL real-fixture,
and Vulkan real-fixture lanes.

Exhaustive accelerator roundtrip:

```sh
python3 tools/roundtrip_real_fixtures.py \
    --backends opencl,vulkan \
    --opencl-device 1 \
    --vulkan-device 1
```

Final report:

- `build/real-fixture-roundtrips/real-fixture-roundtrip-20260708-145623/`
- Six local `.lds` fixtures, two backends, twelve total rows.
- Each row ran `compress`, `verify --source`, `decompress`, and decoded
  size/MD5 comparison against the source `.lds`.
- Aggregate input: `149,954,560` bytes.
- OpenCL output: `79,892,119` bytes; aggregate compress time `4.452` seconds.
- Vulkan output: `79,892,217` bytes; aggregate compress time `3.938` seconds.

## Device Assumptions

Use a GPU-visible shell for accelerator validation. Sandboxed contexts can hide
the NVIDIA devices or expose only CPU Vulkan/llvmpipe.

Device indexes used for final validation on `smaug`:

- OpenCL device `1`: NVIDIA GeForce RTX 5070 Ti.
- Vulkan device `1`: NVIDIA GeForce RTX 5070 Ti.
- Vulkan device `0`: integrated AMD GPU. It is usable for functional smoke
  testing but should not be used for performance conclusions.
- Vulkan device `2`: NVIDIA GeForce RTX 4070 SUPER.
- Vulkan device `3`: llvmpipe CPU Vulkan implementation.

AMD Vulkan support is intended through standard Vulkan compute design, but the
1.1 release did not validate discrete AMD hardware.

## Current Defaults And Useful Commands

Default compression remains CPU/libFLAC Ogg FLAC `.ldf` for compatibility:

```sh
build/ld-compress-ng compress capture.lds
```

Native FLAC backends write `.flac.ldf`. Use `opencl`/`vulkan` for accelerated
compression; keep `native-fixed` as a scalar reference/debug backend:

```sh
build/ld-compress-ng compress --backend native-fixed --threads 8 capture.lds
build/ld-compress-ng compress --backend opencl --device 1 --threads 8 capture.lds
build/ld-compress-ng compress --backend vulkan --device 1 --threads 8 capture.lds
```

Native tuning defaults:

- `--frame-samples 4608`
- `--lpc-order 12`
- `--lpc-precision 12`
- `--rice-partition-order 5`
- `--threads 1` by default

For routine OpenCL/Vulkan benchmark comparisons, use `--threads 8`. OpenCL and
Vulkan still default to one thread for `compress`, but additional threads
parallelize the CPU selected-frame writer after GPU analysis. Use CPU/libFLAC
for normal CPU-only compression; use `native-fixed` when scalar reference
behavior is needed.

Speed-profile sweep command shape:

```sh
python3 tools/sweep_real_fixtures.py \
    --binary build/ld-compress-ng \
    --fixtures reference/testdata/ld-decode-testdata-ci/1cf698d2025e8515e9ef57e34adaf85a194da96a \
    --threads 8 \
    --frame-samples 4608 \
    --lpc-order 12 \
    --lpc-precision 12 \
    --rice-partition-order 5,6 \
    --analysis-profile order-guess-mean-estimate-rice \
    --include-opencl \
    --reuse-opencl-session \
    --opencl-device 1 \
    --include-vulkan \
    --reuse-vulkan-session \
    --vulkan-device 1
```

## Important Decisions To Preserve

- Do not chase tiny OpenCL/Vulkan compression-ratio or runtime deltas unless a
  future task explicitly chooses a performance/tuning pass.
- Treat `native-fixed` and `native-verbatim` as reference/debug backends, not
  production CPU compression recommendations. Keep them available for scalar
  oracle, native writer, and fixture coverage.
- OpenCL/Vulkan outputs are compatible and roundtrip correctly on the validated
  Linux/NVIDIA host.
- The CPU/native writer remains the compatibility authority; accelerators choose
  analysis candidates and selected subframes.
- Vulkan and OpenCL may differ slightly in LPC coefficient/candidate choices due
  to floating-point summation/precision. Exact byte-for-byte candidate parity is
  not a release requirement.
- `docs/remote-sync.md` is maintainer-only and source-tree-only. Do not install
  it unless intentionally changing release doc policy.
- Preserve LGPL/CUETools notices in `src/opencl_analysis.cpp` and
  `shaders/vulkan_fixed_constant.comp` when modifying accelerator analysis code.
- Reference trees under `reference/`, `FlaLDF/`, and `ld-decode-tools/` should
  remain read-only unless a task explicitly says otherwise.

## Good Places To Resume Future Work

Start future work from the current local `main`. `origin/main` may still be at
the published `v1.1.0` release until the post-release mainline commits are
pushed, so check `git status --short --branch` and `git log --oneline -5`
before syncing.

Potential next branches:

- `codex/1.1.x-maintenance` for small bug fixes after the release.
- `codex/1.2-planning` for feature planning.
- `codex/amd-vulkan-validation` if discrete AMD hardware becomes available.

Reasonable next tasks:

- Validate Vulkan on real AMD discrete hardware and document results.
- Decide whether to keep, delete, or archive the merged `codex/vulkan-1.1`
  branch.
- If desired later, add binary packaging or GitHub Actions as a separate scoped
  plan; they were intentionally out of scope for 1.0/1.1.
- Revisit accelerator performance only at the architecture level. The next
  promising direction is reusable per-frame ingest facts or writer-ready
  residual/Rice state, not shader micro-tuning.
- Consider future macOS acceleration as a Metal backend; OpenCL on macOS is
  deprecated and was not treated as a runtime validation target.
