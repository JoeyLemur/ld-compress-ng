# Remote Sync Notes

Use this when moving the macOS working repo to the Linux OpenCL validation host
(`smaug`) for Codex CLI work.

## One-Shot Bundle Sync

From macOS:

```sh
cd /Users/epowell/Development/laserdisc/compress
git status --short
git log --oneline -5
git bundle create /tmp/ld-compress-ng-main.bundle main
scp /tmp/ld-compress-ng-main.bundle epowell@smaug:~/Development/
```

On `smaug`, for an existing clone at `~/Development/compress`:

```sh
cd ~/Development/compress
git status --short
git bundle verify ~/Development/ld-compress-ng-main.bundle
git fetch ~/Development/ld-compress-ng-main.bundle main
git merge --ff-only FETCH_HEAD
git log --oneline -5
```

If the clone does not exist yet:

```sh
cd ~/Development
git clone ~/Development/ld-compress-ng-main.bundle compress
cd compress
git log --oneline -5
```

## Repeated Sync With A Bare Repo

Create a bare repo on `smaug` once:

```sh
ssh epowell@smaug 'mkdir -p ~/Development/git && git init --bare ~/Development/git/ld-compress-ng.git'
```

From macOS:

```sh
cd /Users/epowell/Development/laserdisc/compress
git remote add smaug epowell@smaug:~/Development/git/ld-compress-ng.git
git push smaug main
```

On `smaug`, either clone from the bare repo:

```sh
cd ~/Development
git clone ~/Development/git/ld-compress-ng.git compress
```

Or update an existing working clone:

```sh
cd ~/Development/compress
git remote add smaug-local ~/Development/git/ld-compress-ng.git
git fetch smaug-local
git merge --ff-only smaug-local/main
```

For a single handoff, the bundle workflow is simpler. For ongoing OpenCL work,
the bare repo workflow avoids repeated bundle files.

## Linux OpenCL Validation Commands

```sh
cd ~/Development/compress
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLDCOMPRESS_ENABLE_OPENCL=ON
cmake --build build --parallel
build/ld-compress-ng devices
build/test_opencl_analysis
ctest --test-dir build --output-on-failure
```

If `build/test_opencl_analysis` prints no skip messages and exits `0`, the
OpenCL analysis kernels compiled and ran on an available device.
