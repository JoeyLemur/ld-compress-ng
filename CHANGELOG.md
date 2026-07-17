# Changelog

## Unreleased

- `compress` now defaults to the deterministic `auto` backend policy: Metal,
  then a usable non-CPU Vulkan device with `shaderInt64`, then OpenCL, then
  CPU/libFLAC. `--backend` remains authoritative and `--backend auto` is
  available explicitly. The selected accelerator writes native FLAC
  `.flac.ldf`; CPU fallback continues to write Ogg FLAC `.ldf`.
- `decompress --progress` now provides a throttled stderr status line with
  decoded-sample progress and elapsed time. It starts after STREAMINFO is read,
  shows a percentage when the stream declares a total sample count, and still
  reports useful decoded-sample counts for FLAC streams with an unknown total.
- Decompression now batches packed LDS output instead of issuing one stream
  write per four decoded samples. It uses libFLAC's final PCM-MD5 result rather
  than a second, per-sample MD5 pass, substantially improving Ogg and native
  FLAC decode throughput while preserving malformed STREAMINFO rejection and
  transactional-output behavior.
- Decoding now rejects any 40 kHz mono 16-bit FLAC sample that cannot be
  losslessly represented by the LDS 10-bit packing grid. Invalid streams leave
  no published LDS output.
- `compress` and `decompress` now write their payload inside a private
  same-directory staging directory created with `mkdtemp`. Without
  `--overwrite`, an atomic no-replace rename prevents a destination created
  during the run from being replaced without requiring hard-link support.
  Older platforms or filesystems without no-replace rename support fall back
  to hard-link publication when available. With `--overwrite`, the payload is
  atomically renamed into place. `SIGINT`, `SIGTERM`, and `SIGHUP` also remove
  active private staging data before preserving the normal signal termination.
- `verify` now hashes compressed input bytes while it sequentially decodes,
  avoiding a second full read of large Ogg and native FLAC captures. It checks
  that the digest covered the whole input before reporting the result.
- Original ld-compress Ogg and FlaLDF captures may contain stale nonzero
  STREAMINFO PCM MD5 fields. The decoder reports a mismatch on stderr but does
  not discard otherwise valid LDS data; `verify --source` is the authoritative
  end-to-end validation.
- Fixed a use-after-free when one threaded selected-frame writer failed while
  another writer was still reading the same analyzed sample batch. OpenCL,
  Vulkan, and Metal writer jobs now retain shared ownership of the batch until
  every in-flight job releases it, including error unwinding. A two-worker
  sanitizer regression covers the failure path without requiring a GPU.
- Compression now writes through same-directory staging and renames its payload
  into place only after the selected backend finishes successfully. Failed CPU,
  native-verbatim, native-fixed, OpenCL, Vulkan, or Metal compression therefore
  leaves an existing destination unchanged even with `--overwrite`, and removes
  the incomplete temporary output.
- Fixed native FLAC compression failing during its final STREAMINFO rewrite for
  LDS captures at or above 80 GiB. Native-verbatim, native-fixed, OpenCL,
  Vulkan, and Metal now write the FLAC-defined `0` (unknown) total-sample count
  when the real count exceeds the 36-bit field, matching the existing
  CPU/libFLAC behavior. The STREAMINFO PCM MD5 remains populated and end-to-end
  `verify --source` still checks the complete decoded capture.

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
