# libpng (vendored)

| | |
|---|---|
| Version | 1.6.47 (as pinned by `build_scripts/build_usd.py`) |
| Source | https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz |
| SHA-256 | `631a4c58ea6c10c81f160c4b21fa8495b715d251698ebc2552077e8450f30454` |
| License | libpng-2.0 (`LICENSE`) |

Used by OpenImageIO. Requires `//vendor/zlib`.

## What is included

Copied unmodified from the release tarball: the top-level `png*.c` and
`png*.h` files (without `pngtest.c`), the C sources of `arm/` and `intel/`,
`scripts/pnglibconf.h.prebuilt`, and `LICENSE`. Other ports, contrib,
tests and build scripts are omitted.

## Local changes

`BUILD.bazel` is ours. `pnglibconf.h` is the prebuilt default
configuration, which is what CMake generates with default options. No
source files are modified.

## Upgrading

Replace the files listed above from the new release, check the source
lists in `CMakeLists.txt`, and update this file.
