# OpenColorIO (vendored)

| | |
|---|---|
| Version | 2.4.2 (matches `VERSIONS.md` and `build_scripts/build_usd.py`) |
| Source | https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/refs/tags/v2.4.2.tar.gz |
| SHA-256 | `2d8f2c47c40476d6e8cea9d878f6601d04f6d5642b47018eaafa9e9f833f3690` |
| License | BSD-3-Clause (`LICENSE`; third-party notices in `THIRD-PARTY.md`) |

Required by OpenImageIO. Requires `//vendor/expat`, `//vendor/imath`,
`//vendor/minizip-ng`, `//vendor/pystring` and `//vendor/yaml-cpp`.

## What is included

Copied unmodified from the release tarball: `include/OpenColorIO/`,
`src/OpenColorIO/` (without `res/` and CMakeLists.txt files),
`src/utils/`, the header-only `ext/sampleicc/src/include/` and
`ext/xxHash/src/include/`, `LICENSE`, `THIRD-PARTY.md`. Apps, bindings,
docs, tests and the vendor plugins are omitted.

## Local changes

`BUILD.bazel` and `embed_builtin_configs.py` are ours. The library is built
static with `OCIO_USE_SIMD=OFF` (`build_scripts/build_usd.py` also disables
OCIO's SSE on Apple arm64). `OpenColorABI.h` and `CPUInfoConfig.h` are
generated from the upstream templates; `embed_builtin_configs.py` embeds
`builtinconfigs/configs/*.ocio` into `CG.cpp`/`Studio.cpp` exactly as
`src/OpenColorIO/CMakeLists.txt` does. No source files are modified.

## Upgrading

Replace the files listed above from the new release, update the version
values in `BUILD.bazel`, check `src/OpenColorIO/CMakeLists.txt` for new
sources, templates or link dependencies, and update this file.
