# OpenSubdiv (vendored)

| | |
|---|---|
| Version | 3.6.1 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/PixarAnimationStudios/OpenSubdiv/archive/refs/tags/v3_6_1.tar.gz |
| SHA-256 | `e9d99a480d80e999667643747c76eb0138d36c418fc154fd71b4bd65be103d52` |
| License | Modified Apache-2.0 (`LICENSE.txt`, `NOTICE.txt`) |

Used by pxOsd and hdSt. Linked into `//bazel/install:opensubdiv` (a shared
library, like CMake's `libosdCPU`/`libosdGPU`).

## What is included

Copied unmodified from the release tarball: `opensubdiv/{sdc,vtr,far,bfr,
hbr,osd}/`, `opensubdiv/version.{h,cpp}`, `tools/stringify/stringify.py`,
`LICENSE.txt`, `NOTICE.txt` (CMakeLists.txt files removed). Examples,
tutorials, regression tests, docs and the GL loader are omitted.

## Local changes

`BUILD.bazel` is ours. It mirrors the options `build_scripts/build_usd.py`
passes (no OpenMP, TBB, CUDA, OpenCL, DirectX, GLEW, GLFW or Ptex; GLSL
patch shader source everywhere, MSL on macOS). Kernels are stringified into
`*.gen.h` with the upstream `stringify.py`, as `osd_stringify` does.

Difference from CMake: of the osdGPU sources, only the GLSL/MSL patch shader
sources (all USD uses) are built; the OpenGL and Metal compute evaluators,
vertex buffers and patch tables are not. Like `build_usd.py`'s Release
dependency builds, OpenSubdiv is compiled with `NDEBUG` (its asserts fire on
valid test data in `testUsdImagingGLGeomSubsets`). No source files are
modified.

## Upgrading

Replace the files listed above from the new release, check the source lists
in `opensubdiv/*/CMakeLists.txt`, update the version in `BUILD.bazel`, and
update this file.
