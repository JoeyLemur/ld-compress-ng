# Changelog

## 1.2.0 - 2026-07-09

- Added the macOS-only Metal native FLAC acceleration backend using Apple
  Command Line Tools, `Metal.framework`, and `Foundation.framework`.
- Added `compress --backend metal`, `--metal-device INDEX`,
  `bench --include-metal`, `bench --reuse-metal-session`, and Metal device
  reporting in `ld-compress-ng devices`.
- Metal follows the existing accelerator contract: GPU full-frame analysis,
  shared native FLAC writer output, scalar handling for short final tails, and
  native `.flac.ldf` output only.
- Moved Metal generated-LPC candidate analysis onto the GPU, including
  windowed autocorrelation, Levinson-Durbin, coefficient quantization,
  optional fixed-order pruning, and mean-Rice speed-profile support.
- Fixed Metal LPC task coefficient ordering against the native selected writer
  and added selected-writer recost regressions so Metal task decisions cannot
  silently write a different predictor.
- Added the `compare_metal_scalar_frames` diagnostic for first-mismatch
  analysis against scalar native decisions, coefficients, Rice shape,
  estimated bits, and selected-writer recosts.
- Added Metal device, smoke, analysis, CLI, benchmark, real-fixture helper, and
  no-Metal build coverage. Hardware-visible Metal tests skip cleanly when no
  Metal device is visible to the process.
- Documented CLT-only macOS setup and validation; runtime Metal source
  compilation is used, so full Xcode and offline `.metallib` artifacts are not
  required.
- Restored Metal real-fixture size parity and closed the initial speed gap on
  Apple M5 Pro device `0`: the six-fixture exact roundtrip writes
  `79,892,801` bytes versus `79,867,690` for native-fixed, and the final
  speed-profile sweep writes `79,946,831` bytes in `0.626s`.
- Improved the Metal speed-profile path with an Apple CommonCrypto MD5 backend,
  pre-shifted autocorrelation input, and pre-shifted exact-analysis input while
  keeping the compressed format, CLI, and output-size class unchanged.
- The final Metal speed-profile checkpoint is currently faster than the
  documented Linux RTX 5070 Ti OpenCL/Vulkan rows for the same six-fixture
  rice6 class (`0.626s` Metal versus `0.814s` OpenCL and `0.813s` Vulkan).

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
