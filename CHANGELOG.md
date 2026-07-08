# Changelog

## 1.1.1 - 2026-07-08

- Improved accelerated native-FLAC throughput with reusable OpenCL analysis
  sessions, OpenCL selected Rice-parameter handoff, larger OpenCL compression
  batches, pipelined accelerator ingest/analyze/write flow, and faster selected
  writer bitstream paths.
- Added benchmark/sweep support for native analysis profiles, including
  order-guess and mean-Rice speed profiles, plus OpenCL/Vulkan session reuse for
  multi-row accelerator sweeps.
- Documented the current 2026-07-08 real-fixture speed sweep and refreshed
  maintainer-facing notes so post-1.1 work is not hidden behind the release
  handoff.

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
