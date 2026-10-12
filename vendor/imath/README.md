# Imath (vendored)

| | |
|---|---|
| Version | 3.1.12 (as pinned by OpenEXR 3.3.6 and OpenColorIO 2.4.2) |
| Source | https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v3.1.12.tar.gz |
| SHA-256 | `8a1bc258f3149b5729c2f4f8ffd337c0e57f09096e4ba9784329f40c4a9035da` |
| License | BSD-3-Clause (`LICENSE.md`) |

Used by OpenEXR, OpenColorIO and OpenImageIO.

## What is included

Copied unmodified from the release tarball: `src/Imath/` (without its
CMakeLists.txt), `config/ImathConfig.h.in`, `LICENSE.md`. Tests, Python
bindings and the website are omitted.

## Local changes

`BUILD.bazel` is ours; it generates `ImathConfig.h` from the upstream
template with CMake's default option values. No source files are modified.

## Upgrading

Replace the files listed above from the new release and update the version
values in `BUILD.bazel` and this file.
