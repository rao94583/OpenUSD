# MaterialX (vendored)

| | |
|---|---|
| Version | 1.39.5 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/AcademySoftwareFoundation/MaterialX/archive/refs/tags/v1.39.5.tar.gz |
| SHA-256 | `c0d739b70a36f6f72888a0e8e66db5c83ae87c40737cc9b51c108166804f3a3b` |
| License | Apache-2.0 (`LICENSE`; bundled code in `THIRD-PARTY.md`) |

Used by usdMtlx, hdMtlx, hdSt (MaterialX materials in Storm) and
usdBakeMtlx. Linked into `//bazel/install:materialx` (`libMaterialX`), one
shared library like MaterialX's `MATERIALX_BUILD_MONOLITHIC`. The data
library (`libraries/`) is installed under usdMtlx's `resources/libraries`,
where `UsdMtlxStandardLibraryPaths()` finds it.

## What is included

Copied unmodified from the release tarball (CMakeLists.txt files removed):

- `source/MaterialX{Core,Format,GenShader,GenHw,GenGlsl,GenMsl,Render,
  RenderHw,RenderGlsl}/`, with their bundled `External/` code (PugiXML,
  stb_image, cgltf, tinyobjloader, glad)
- `source/MaterialXTrace/{Export.h,Tracing.h}`: included by MaterialXGenShader
  even though MaterialXTrace is only built with Perfetto tracing
- `libraries/`, `LICENSE`, `THIRD-PARTY.md`

Omitted: the OSL, MDL and Slang generators, MaterialXRenderMsl/Osl/Slang,
the viewer, graph editor, Python and JavaScript bindings, tests,
documentation and `resources/` (133 MB of test assets). USD links none of
them.

## Local changes

`BUILD.bazel` and `mx_add_library.bzl` are ours. They mirror the CMake
files with `build_usd.py`'s options (shared libraries; no OCIO, OIIO,
Python or tests). `MaterialXCore/Generated.h` comes from the upstream
template through `cmake_configure_file`. No source files are modified.

Warnings are silenced with `-w` instead of MaterialX's `-Wall -Wshadow ...`.
`GL_SILENCE_DEPRECATION` (Apple-only in CMake) is defined on every OS for
MaterialXRenderGlsl; only Apple's OpenGL headers read it.

## Upgrading

Replace the directories listed above from the new release, compare
`source/*/CMakeLists.txt` and the top-level `CMakeLists.txt` with the BUILD
file (new modules, new definitions, the version in `_VERSION`), and update
this file.
