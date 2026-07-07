# Handoff Notes

Last updated: 2026-07-07, immediately after publishing `v1.1.0`.

This file is for maintainer/agent continuity. It is intentionally not installed
by CMake; release-facing installed docs are listed explicitly in
`CMakeLists.txt`.

## Current Repository State

- Active branch after release: `main`.
- `main` and `origin/main` point to `25f71cd842d4e4aa12e911a320484a5f98049975`
  (`Finalize 1.1 release readiness docs`).
- Local branch `codex/vulkan-1.1` still points at the same commit as `main`.
  It can be kept as history context or deleted later.
- Annotated tag `v1.1.0` has been pushed to origin.
- GitHub release was created as source-only, with no binary assets:
  `https://github.com/JoeyLemur/ld-compress-ng/releases/tag/v1.1.0`
- Project version is `1.1.0` in `CMakeLists.txt`; `ld-compress-ng --version`
  prints `ld-compress-ng 1.1.0`.

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

## Final Validation That Passed

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

- `build/real-fixture-roundtrips/real-fixture-roundtrip-20260707-003152/`
- Six local `.lds` fixtures, two backends, twelve total rows.
- Each row ran `compress`, `verify --source`, `decompress`, and decoded
  size/MD5 comparison against the source `.lds`.
- Aggregate input: `149,954,560` bytes.
- OpenCL output: `79,892,119` bytes; aggregate compress time `9.078` seconds.
- Vulkan output: `79,892,217` bytes; aggregate compress time `9.134` seconds.

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
Vulkan still default to one thread, but additional threads parallelize the CPU
selected-frame writer after GPU analysis. Use CPU/libFLAC for normal CPU-only
compression; use `native-fixed` when scalar reference behavior is needed.

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

Start future work from `main` after pulling `origin/main`.

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
- Revisit accelerator performance only at the architecture level. The known
  remaining costs are mostly writer/backend overhead, not PCIe transfer, and
  OpenCL/Vulkan are already good enough for the 1.1 release target.
- Consider future macOS acceleration as a Metal backend; OpenCL on macOS is
  deprecated and was not treated as a runtime validation target.
