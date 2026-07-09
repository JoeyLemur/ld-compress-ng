# Changelog

## 1.2.0 - 2026-07-09

1.2.0 adds a macOS Metal accelerator for writing native FLAC `.flac.ldf`
captures. The normal CPU/libFLAC path is unchanged; Metal is an optional
backend for Apple systems with a visible Metal device.

User-facing changes:

- New `compress --backend metal` support on macOS. Metal builds use Apple
  Command Line Tools plus `Metal.framework` and `Foundation.framework`; a full
  Xcode project and prebuilt `.metallib` files are not required.
- `ld-compress-ng devices` now reports Metal devices, and `bench` can include
  Metal rows with `--include-metal`, `--metal-device INDEX`, and
  `--reuse-metal-session`.
- Metal follows the same output contract as the OpenCL and Vulkan accelerators:
  it writes native FLAC `.flac.ldf` files, uses the shared native FLAC writer,
  and falls back to the scalar path for short final frame tails.
- Explicit device selection is supported with `--metal-device INDEX`; for a
  single Metal `compress` run, backend-local `--device INDEX` also works.

Reliability and validation:

- Fixed an early Metal LPC coefficient-ordering issue that could make GPU
  analysis choose one predictor while the native writer encoded another. New
  selected-writer recost tests now guard that handoff so Metal-selected
  predictors are written with the same bits and coefficients measured during
  analysis.
- Added Metal device, smoke, analysis, CLI, benchmark, real-fixture helper, and
  no-Metal build coverage. Hardware-visible Metal tests skip cleanly when no
  Metal device is visible to the process, which is expected in some sandboxed
  shells.
- Added `compare_metal_scalar_frames`, a maintainer diagnostic for finding the
  first frame where Metal and scalar native analysis diverge.

Performance notes:

- On Apple M5 Pro device `0`, the six-fixture exact roundtrip writes
  `79,892,801` bytes with Metal versus `79,867,690` bytes with `native-fixed`,
  keeping Metal within `0.032%` of the scalar native output size on that
  fixture set.
- The final Metal speed-profile checkpoint writes `79,946,831` bytes in
  `0.626s` across the same six real capture fixtures. Hardware differs, but
  for scale, the documented Linux RTX 5070 Ti rice6 rows for the same
  benchmark class are `0.814s` OpenCL and `0.813s` Vulkan.

## 1.1.1 - 2026-07-08

- Made OpenCL and Vulkan compression substantially faster in the current
  real-capture benchmark sweep while preserving verified round trips back to
  the original `.lds` data.
- Documented the current six-fixture validation numbers in table form.

  Normal exact-analysis OpenCL/Vulkan `compress` roundtrip:

  | Backend | Input bytes | Output bytes | Compress time | Validation |
  | --- | ---: | ---: | ---: | --- |
  | OpenCL | `149,954,560` | `79,892,119` | `4.452s` | Verified and decompressed back to the original `.lds` data. |
  | Vulkan | `149,954,560` | `79,892,217` | `3.938s` | Verified and decompressed back to the original `.lds` data. |

  Speed-focused benchmark profile:

  | Backend | Output bytes | Elapsed time |
  | --- | ---: | ---: |
  | CPU/libFLAC | `80,086,984` | `2.440s` |
  | Native-fixed | `79,926,901` | `1.689s` |
  | OpenCL | `79,946,987` | `0.814s` |
  | Vulkan | `79,946,934` | `0.813s` |

- Kept the compatibility story unchanged: CPU compression still defaults to
  Ogg FLAC `.ldf`, while OpenCL and Vulkan continue to write native FLAC
  `.flac.ldf` files.
- Added repeatable benchmark controls for comparing faster analysis profiles
  and reusing GPU setup during multi-row OpenCL/Vulkan sweeps.
- Refreshed the 2026-07-08 performance notes and maintainer documentation so
  the release summary focuses on what users can expect, not encoder internals.

## 1.1.0 - 2026-07-07

- Added the Linux-first Vulkan native FLAC acceleration backend, using Vulkan
  compute for fixed/Rice and GPU-generated LPC candidate analysis while keeping
  the CPU native FLAC writer as the compatibility authority.
- Validated Vulkan native FLAC output against the reference `ld-decode` loader
  on the local Linux/NVIDIA fixture lane; AMD remains intended to be supported
  through standard Vulkan compute but is not yet hardware-validated.
- Improved OpenCL throughput with larger batches, persistent analysis state,
  best-task readback, parallel exact/Rice costing, and cooperative generated
  autocorrelation.
- Added mixed OpenCL/Vulkan device-selection hardening, accelerator real-fixture
  matrix coverage, and parser tests for mixed-GPU device output.
- Added exhaustive OpenCL/Vulkan real-fixture roundtrip validation tooling that
  compresses, verifies against source LDS, decompresses, and byte-compares every
  local fixture/backend pair.
- Refreshed build, testing, and release documentation for source releases with
  optional OpenCL and Vulkan accelerator lanes.

## 1.0.0 - 2026-07-06

Initial 1.0 release of `ld-compress-ng`.

- Added a focused C++20/CMake CLI for LaserDisc RF capture compression,
  decompression, verification, LDS packing conversion, benchmarking, and OpenCL
  device enumeration.
- Added CPU/libFLAC Ogg FLAC `.ldf` compression and FLAC decode back to packed
  `.lds`.
- Added native FLAC `.flac.ldf` output through scalar `native-fixed`, OpenCL,
  and compatibility/debug `native-verbatim` backends.
- Added decode-time FLAC shape validation, STREAMINFO sample-count validation,
  decoded PCM MD5 checks, and temporary-output protection for failed
  decompression.
- Added compatibility coverage for FFmpeg/PyAV and reference `ld-decode` loader
  paths, plus opt-in real-fixture and FLAC decoder testbench suites.
- Added consumer README, detailed development docs, LGPL-2.1-or-later licensing,
  CMake install support, and a release checklist.
