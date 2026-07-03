# ld-compress-ng

Targeted LaserDisc RF capture compression tooling.

This repository is replacing the historical `ld-compress` shell pipeline with a
native C++20/CMake CLI. Reference material from `FlaLDF/`, `ld-decode-tools/`,
and the original `ld-compress` script is intentionally ignored by Git.

See `PROJECT_PLAN.md` for the implementation plan.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

## Current Status

The first implementation slice provides native LDS 10-bit pack/unpack conversion
and a small CLI surface for `convert`.
