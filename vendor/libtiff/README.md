# libtiff (vendored)

| | |
|---|---|
| Version | 4.7.1 (as pinned by `build_scripts/build_usd.py`) |
| Source | https://download.osgeo.org/libtiff/tiff-4.7.1.tar.gz |
| SHA-256 | `f698d94f3103da8ca7438d84e0344e453fe0ba3b7486e04c5bf7a9a3fabe9b69` |
| License | libtiff (`LICENSE.md`) |

Used by OpenImageIO. Requires `//vendor/zlib` and `//vendor/libjpeg-turbo`.

## What is included

Copied unmodified from the release tarball: `libtiff/*.c` (without the
Windows I/O `tif_win32.c` and the `mkg3states.c` generator), `libtiff/*.h`,
`tif_stream.cxx`, `tiffio.hxx`, the `tif_config.h.cmake.in` and
`tiffconf.h.cmake.in` templates, and `LICENSE.md`. Tools, contrib, tests and
docs are omitted. The shipped `tiffvers.h` is used as is.

## Local changes

`BUILD.bazel` is ours. Config headers are generated from the upstream
templates with CMake's default options and the codecs zlib and
libjpeg-turbo enable (no libdeflate, LZMA, ZSTD, WebP, JBIG or LERC). No
source files are modified.

## Upgrading

Replace the files listed above from the new release, re-check the options
in `cmake/*.cmake` and the sources in `libtiff/CMakeLists.txt`, and update
this file.
