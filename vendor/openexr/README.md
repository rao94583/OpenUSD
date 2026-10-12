# OpenEXR (vendored)

| | |
|---|---|
| Version | 3.3.6 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.3.6.tar.gz |
| SHA-256 | `2e3c14c0fd47938819cab0a1d9e0a6dc3d276cdec556745eb7521f6add4deed9` |
| License | BSD-3-Clause (`LICENSE.md`) |

Used by OpenImageIO. Requires `//vendor/imath` and `//vendor/libdeflate`.

## What is included

Copied unmodified from the release tarball: `src/lib/` (Iex, IlmThread,
OpenEXR, OpenEXRCore, OpenEXRUtil; CMakeLists.txt files removed), the
config header templates `cmake/*Config*.h.in`, and `LICENSE.md`. Tools,
examples, tests, docs and Python bindings are omitted.

## Local changes

`BUILD.bazel` is ours, structured after the one shipped with OpenEXR (which
hard-codes 3.3.0 values). Config headers are generated from the upstream
templates with the values OpenEXR's CMake would produce, per platform. No
source files are modified.

## Upgrading

Replace the files listed above from the new release, update the version
values in `BUILD.bazel`, re-check `cmake/CMakeLists.txt` for new config
checks and the library CMakeLists.txt files for new or excluded sources,
and update this file.
