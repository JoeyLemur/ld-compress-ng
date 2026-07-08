# Changelog

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
