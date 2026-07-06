# 1.0 Release Checklist

Use this checklist for the `v1.0.0` source release. Binary packages and GitHub
Actions are intentionally out of scope for this release.

## Required Local Gate

Start from a clean working tree:

```sh
git status --short --branch
```

Build and run the normal non-real-fixture suite:

```sh
cmake --build build
ctest --test-dir build -LE real-fixtures --output-on-failure
```

Check version/help output:

```sh
build/ld-compress-ng --version
build/ld-compress-ng --help
```

Run a CPU-only build/test lane:

```sh
cmake -S . -B build-release-noopencl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=OFF
cmake --build build-release-noopencl --parallel
ctest --test-dir build-release-noopencl -LE real-fixtures --output-on-failure
```

Smoke-test installation:

```sh
cmake --install build --prefix /tmp/ld-compress-ng-install
/tmp/ld-compress-ng-install/bin/ld-compress-ng --version
find /tmp/ld-compress-ng-install -maxdepth 6 -type f | sort
```

Confirm the installed files include:

- `bin/ld-compress-ng`
- `share/doc/ld-compress-ng/README.md`
- `share/doc/ld-compress-ng/LICENSE`
- `share/doc/ld-compress-ng/THIRD_PARTY_NOTICES.md`
- `share/doc/ld-compress-ng/CHANGELOG.md`
- `share/doc/ld-compress-ng/docs/build-and-testing.md`
- `share/doc/ld-compress-ng/docs/release-checklist.md`

## Optional Full Local Gate

When local ignored fixtures and GPU access are available, run the full matrix
from a context that can see the OpenCL and Vulkan runtimes. Use explicit device
indexes from `ld-compress-ng devices` on mixed-GPU hosts:

```sh
python3 tools/check_local_matrix.py \
    --build-root build/local-check-release \
    --all-local \
    --include-opencl-real-fixture \
    --opencl-device 0 \
    --include-vulkan-real-fixture \
    --vulkan-device 1 \
    --python-executable /home/epowell/.pyenv/versions/3.13.13/envs/ld/bin/python \
    --jobs 2
```

## Tag And Publish

After the required gate passes, tag and push:

```sh
git tag -a v1.0.0 -m "ld-compress-ng 1.0.0"
git push origin main
git push origin v1.0.0
```

Create the GitHub source release manually from tag `v1.0.0`, using
`CHANGELOG.md` as the release-note source.
