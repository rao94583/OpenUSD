# OpenImageIO (vendored)

| | |
|---|---|
| Version | 3.1.8.0 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/AcademySoftwareFoundation/OpenImageIO/archive/refs/tags/v3.1.8.0.tar.gz |
| SHA-256 | `6749eeb89f8b8b8c61261801149a3863d352ff17980933c58199293ef2310816` |
| License | Apache-2.0 (`LICENSE.md`; third-party notices in `THIRD-PARTY.md`) |

Provides `idiff`, which `cmake/macros/testWrapper.py` runs for
`IMAGE_DIFF_COMPARE` in imaging tests (CMake sets `IMAGE_DIFF_TOOL` to OIIO's
idiff; `build_scripts/build_usd.py` builds OIIO's tools for imaging tests).
The hioOiio plugin (`PXR_BUILD_OPENIMAGEIO_PLUGIN`) stays off.

Requires `//vendor/fmt`, `//vendor/imath`, `//vendor/libjpeg-turbo`,
`//vendor/libpng`, `//vendor/libtiff`, `//vendor/opencolorio`,
`//vendor/openexr`, `//vendor/robin-map` and `//vendor/zlib`.

## What is included

Copied unmodified from the release tarball: `src/include/`, `src/libutil/`,
`src/libOpenImageIO/`, `src/libtexture/`, `src/idiff/`, the plugin
directories `src/{bmp,cineon,dds,dpx,fits,hdr,ico,iff,jpeg,null,openexr,png,
pnm,psd,rla,sgi,softimage,targa,term,tiff,zfile}.imageio/`, `LICENSE.md`
and `THIRD-PARTY.md`, without CMakeLists.txt files and `*_test.cpp` unit
tests. Other tools, plugins needing other libraries, Python bindings, docs
and the test suite are omitted.

## Local changes

`BUILD.bazel` is ours. It builds a static `libOpenImageIO_Util` and
`libOpenImageIO` with embedded plugins (`EMBEDPLUGINS=ON`) for the formats
above, and `idiff`. `oiioversion.h` and `buildopts.h` are generated from the
upstream templates, and fmt's headers are copied to
`OpenImageIO/detail/fmt/` as CMake's `OIIO_INTERNALIZE_FMT` does. No source
files are modified.

## Upgrading

Replace the files listed above from the new release, update the version
values and source lists in `BUILD.bazel` from the CMakeLists.txt files, and
update this file.
