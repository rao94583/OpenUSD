# libjpeg-turbo (vendored)

| | |
|---|---|
| Version | 3.1.4.1 (as pinned by `build_scripts/build_usd.py`) |
| Source | https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.1.4.1.tar.gz |
| SHA-256 | `a7da42b640377c2a9a9665e2c4b0ea60cd5599afb48c2521e6df0c9dc9d15a25` |
| License | BSD-3-Clause and IJG (`LICENSE.md`, `README.ijg`) |

The libjpeg API library (not TurboJPEG), used by libtiff and OpenImageIO.

## What is included

Copied unmodified from the release tarball: `src/*.c`, `src/*.h`,
`src/*.h.in`, `src/wrapper/`, `LICENSE.md`, `README.ijg`. SIMD code, Java
bindings, tests, test images and docs are omitted.

## Local changes

`BUILD.bazel` is ours. It builds `JPEG_SOURCES` from `CMakeLists.txt` with
arithmetic coding and without SIMD (`WITH_SIMD=0`, which
`build_scripts/build_usd.py` also uses on some platforms). Config headers
are generated from the upstream templates; `BUILD` (a date in CMake) is
fixed to `bazel` for reproducibility. No source files are modified.

## Upgrading

Replace the files listed above from the new release, update `_SRCS`
and the version values in `BUILD.bazel` from `CMakeLists.txt`, and update
this file.
