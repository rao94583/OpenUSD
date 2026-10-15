# Embree (vendored)

| | |
|---|---|
| Version | 4.3.3 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/RenderKit/embree/archive/refs/tags/v4.3.3.tar.gz |
| SHA-256 | `8a3bc3c3e21aa209d9861a28f8ba93b2f82ed0dc93341dddac09f1f03c36ef2d` |
| License | Apache-2.0 (`LICENSE.txt`; third-party notices in `third-party-programs.txt`) |

Required by the hdEmbree render delegate (`pxr/imaging/plugin/hdEmbree`,
`PXR_BUILD_EMBREE_PLUGIN`). Requires `//vendor/onetbb`.

## What is included

Copied unmodified from the release tarball: `include/`, `common/` and
`kernels/` without the GPU directories (`kernels/sycl/`, `kernels/rthwif/`,
`kernels/level_zero/`; their includes are guarded by `EMBREE_SYCL_SUPPORT`),
CMake files, `embree.rc` and the linker export maps; `LICENSE.txt`,
`third-party-programs.txt`. Tutorials, tests, docs and scripts are omitted.

## Local changes

`BUILD.bazel` is ours. It builds one static library the way
`build_usd.py` configures Embree on Apple arm64 (`EMBREE_MAX_ISA=NEON`,
`EMBREE_ISPC_SUPPORT=OFF`, `EMBREE_TUTORIALS=OFF`), with TBB tasking and
the option defaults of `CMakeLists.txt`. NEON runs Embree's SSE2 code paths
through `common/simd/arm/sse2neon.h`, so there are no per-ISA libraries.
`rtcore_config.h`, `config.h` and `hash.h` are generated from the upstream
templates. No source files are modified.

x86-64 Linux would need the SSE4.2/AVX/AVX2 kernel libraries
(`embree_files()` in `kernels/CMakeLists.txt`) with their ISA flags; not
done yet.

## Upgrading

Replace the files listed above from the new release, update the version
values in `BUILD.bazel`, check `kernels/CMakeLists.txt` and
`common/*/CMakeLists.txt` for new sources or options, and update this file.
