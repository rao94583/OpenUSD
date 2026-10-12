# Bazel build for OpenUSD — Phase 5 handoff

Self-contained context for continuing the Bazel build of this OpenUSD fork.
Repo: `~/Code/OpenUSD`, branch `bazel-build` (pushed to `origin` =
github.com/rao94583/OpenUSD; `upstream` = Pixar, never push there).

## Goal and settled decisions

Bazel build of OpenUSD with Python bindings, dependencies vendored as source.

- **Scope:** core (`pxr/base`, `pxr/usd`, `usdValidation`, CLI tools, Python)
  plus imaging (Hydra/Storm/usdImaging, OpenSubdiv). Deferred: usdview
  (needs PySide), exec, MaterialX and other optional plugins. The generator
  sets `PXR_BUILD_EXEC=OFF` and `PXR_BUILD_USDVIEW=OFF` until they are
  ported.
- **Platform:** macOS arm64 now; keep Linux possible (all platform
  differences through `select()`/`per_os`, no macOS-only assumptions).
  Linux has not been built yet.
- **Graphics (user decision):** Metal renders on macOS, OpenGL on Linux.
  macOS uses CMake's default of `PXR_ENABLE_GL_SUPPORT=ON` *and*
  `PXR_ENABLE_METAL_SUPPORT=ON`, because hdSt/hdx are only built with GL
  support enabled; Storm renders through hgiMetal there, and garch/glf/hgiGL
  link Apple's OpenGL framework without being the render path.
- **CMake stays** as the reference build. BUILD.bazel files under `pxr/` are
  generated from CMakeLists.txt by `bazel/tools/cmake2bazel.py`; never
  hand-edit them, fix the generator instead.
- **Vendoring:** C++ and Python deps as source under `vendor/<dep>/` with
  our own BUILD.bazel and a README (version, URL, SHA-256, local changes);
  nothing is fetched at build time. Versions follow `VERSIONS.md`, then
  `build_scripts/build_usd.py`, then what the depending project pins. Bazel
  rule sets and the hermetic Python come from BCR.
- **Image comparisons (user decision):** the full OpenImageIO 3.1.8.0 stack
  is vendored to build `idiff`, which `IMAGE_DIFF_COMPARE` uses (CMake's
  `IMAGE_DIFF_TOOL`). The hioOiio plugin stays off; deviating from CMake,
  image comparisons run although `PXR_BUILD_OPENIMAGEIO_PLUGIN` is off.
- **Linking model:** each module is a static `cc_library` with
  `alwayslink = True`. All of them, core plugins included, are linked into
  one monolithic shared library, `//bazel/install:usd_ms`. oneTBB and
  OpenSubdiv are separate shared libraries (`:tbb`, `:opensubdiv`), as in
  CMake builds: pxr static initializers call into tbb, and only a separate
  image is guaranteed to be initialized first. Python extensions, programs
  and tests link all three (`PXR_DYNAMIC_DEPS` in `pxr_library.bzl`). The
  OIIO stack is linked statically into `idiff` only.
- **Install tree:** `//bazel/install` mirrors CMake's install layout so Plug
  finds resources relative to the monolithic library as in an installed
  build. Tests mirror CMake's `<prefix>/tests` per package.

## Phases

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Skeleton, oneTBB, macro, pxr.h, arch + tf | `bazel build //...`, smoke tests pass |
| 1 ✅ | Rest of pxr/base + Python | `from pxr import Tf, Gf, Vt` works |
| 2 ✅ | pxr/usd/\*, usdValidation, CLI tools, plugInfo substitution | open a stage from Python; `bazel run` usdcat |
| 3 ✅ | Port tests via `cmake/macros/testWrapper.py` | core tests pass |
| 4 ✅ | Imaging: OpenSubdiv, hgiMetal/hgiGL, hd, hdSt, hdx, usdImaging; OIIO idiff | headless Storm render matches baseline |
| **5** | **`cmake2bazel --check` in CI, `py_wheel`, optional `bazel vendor`; Linux bring-up** | — |

## What exists (Phases 0–4)

- `MODULE.bazel`: Bazel 9.3.0 (`.bazelversion`); `apple_support` 2.10.1
  (`repo_name = "build_bazel_apple_support"`, declared before rules_cc),
  `bazel_skylib` 1.9.2, `platforms` 1.1.0, `rules_cc` 0.2.26,
  `rules_python` 2.4.2 with hermetic Python 3.11. Commit
  `MODULE.bazel.lock` with any dependency change.
- `.bazelrc`: `-std=c++17`, macOS min 13.0 via
  `--enable_platform_specific_config`.
- **Vendored** (each with README; BUILD files are ours):
  - oneTBB 2021.12.0 (build-only definitions are `local_defines`; public
    ones leaked `_XOPEN_SOURCE` into pxr), Jinja2 3.1.6, MarkupSafe 3.0.3.
  - OpenSubdiv 3.6.1 (`osd_cpu`, and of `osd_gpu` only the GLSL/MSL patch
    shader sources USD uses; kernels stringified with upstream
    `stringify.py`; built with `NDEBUG` like build_usd's Release deps, since
    its asserts fire on valid test data).
  - The OIIO stack: zlib 1.3.2, libdeflate 1.18, Imath 3.1.12, OpenEXR
    3.3.6, libpng 1.6.47, libjpeg-turbo 3.1.4.1 (no SIMD), libtiff 4.7.1,
    expat 2.8.5, yaml-cpp 0.7.0, pystring 1.1.3, minizip-ng 3.0.7,
    OpenColorIO 2.4.2 (`OCIO_USE_SIMD=OFF`; built-in configs embedded by
    `embed_builtin_configs.py`), fmt 10.2.1, robin-map 1.4.0, OpenImageIO
    3.1.8.0 (static, embedded plugins for formats whose deps are vendored,
    `//vendor/openimageio:idiff`). OpenEXR and libpng also expose their
    install layout (`<OpenEXR/...>`, `<libpng16/...>`).
- `bazel/cmake_configure_file.bzl` + `bazel/tools/cmake_configure_file.py`:
  CMake `configure_file()` (`#cmakedefine`, `#cmakedefine01`, `@VAR@`,
  `${VAR}`, `@ONLY`) for vendored config headers from upstream templates.
- `pxr/BUILD.bazel` (hand-written): `//pxr` = generated `pxr.h` +
  project-wide defines from `CXXDefaults.cmake` and, per OS, the imaging
  ones from `Packages.cmake` (`PXR_GL_SUPPORT_ENABLED`,
  `PXR_METAL_SUPPORT_ENABLED` on macOS, `PXR_X11_SUPPORT_ENABLED` on
  Linux). Keep these in step with the generator's `PXR_ENABLE_*` values.
- `cmake/macros/BUILD.bazel`: exports `moduleDeps.cpp.in`, `testWrapper.py`.
- **`//bazel/install`** (hand-written BUILD; rules in `pxr_install.bzl`,
  `pxr_python.bzl`): `:tbb`, `:opensubdiv`, `:usd_ms` (`lib/lib*.dylib`,
  `.so` on Linux), `:resources` (lib/usd and plugin/usd trees with
  substituted plugInfo.json), `:python` (`lib/python/pxr`).
- **Generator** `bazel/tools/cmake2bazel.py` (`--write`, `--check`, or no
  flag to print). Follows `add_subdirectory()` from `pxr/CMakeLists.txt`
  into `SCOPE` (`pxr/external`, `pxr/base`, `pxr/usd`, `pxr/usdValidation`,
  `pxr/imaging`, `pxr/usdImaging`). Interprets the CMake subset pxr uses,
  including user-defined functions with `cmake_parse_arguments`, the test
  macros and boost::python's raw target commands, `add_compile_options`
  (as per-library `copts`), and `CMAKE_INCLUDE_CURRENT_DIR` (emitted as
  `include_current_dir = False` where a directory turns it off). Commands
  that set variables or affect targets but are not implemented are errors.
  Host-independent: `find_*` never probes the host (Apple frameworks become
  `-framework X`), and platform/find-module values are in `PLATFORM_VARS`.
  System CMake targets map to link flags in `SYSTEM_TARGETS`
  (`OpenGL::GL`), vendored ones to labels in `EXTERNAL_LABELS`.
  Runs once per OS: library differences go under `per_os`; libraries,
  tools, test programs and tests existing on some OSes only get
  `target_compatible_with`; test files are the union over OSes and
  `test_tree` lists OS-specific targets in a `select()`.
  `bazel/pxr_libraries.bzl` (`PXR_LIBRARIES`, `PXR_RESOURCES` with OS
  `select()`s; `PXR_PYTHON_MODULES` with `target_compatible_with`, which
  `pxr_python_package` rejects if non-empty until supported).
  `EXCLUDED_TESTS` lists tests that cannot pass here, with reasons.
- `bazel/pxr_library.bzl`: `pxr_library` (all targets take
  `target_compatible_with`; macOS-only libraries may list `.mm` sources
  directly; the `objc_library` also carries the library's `linkopts`, since
  an all-Objective-C++ library like hgiMetal links through it only; headers
  in the package and files under source subdirectories are textual headers,
  as CMake lets sources include them), `pxr_cpp_bin`, `pxr_python_bin`,
  `pxr_configure_file`, `PXR_COPTS`, `PXR_DYNAMIC_DEPS`.
- **Tests** (`bazel/pxr_test.bzl`, `bazel/tools/pxr_test_runner.py`): as in
  Phase 3, plus `image_diff_tool` (idiff for `IMAGE_DIFF_COMPARE`; the
  runner passes the image-diff and threshold arguments and `--post-path` to
  idiff as `pxr_register_test` does with `IMAGE_DIFF_TOOL`) and
  `excluded = "<reason>"` (tags the test `manual`).
- `bazel/smoke/`: 7 smoke tests, kept (cheap; `schemaToolsSmoke` is the
  only coverage of usdGenSchema/usdInitSchema).

Verified at the end of Phase 4 on macOS arm64: `bazel build //...`;
`bazel test --nocache_test_results //...` — 1474 pass, 35 skipped (not
compatible with macOS: OpenGL-only tests and `TfMallocTag`), in about 7
minutes; `cmake2bazel.py --check` (86 files). Storm renders through Metal
and 154 hdSt tests compare images with idiff; replacing a baseline image
makes its test fail with `IMAGE_DIFF_COMPARE`. 18 tests are excluded
(tagged `manual`):

- 15 need PySide for an OpenGL context (usdrecord ×8,
  testUsdAppUtilsFrameRecorder, testUsdImagingGLUsdRenderPass* ×6).
- 2 use MaterialX shaders (testUsdImagingGLMaterialsAndInstancers_*);
  MaterialX support is off by default.
- 1 undiagnosed: testUsdImagingGLBasisCurvesWithScale differs on 0.5% of
  pixels at curve edges (limit 0.3%) with Metal on macOS 27.

## Gotchas already learned

- Bazel 9 has no native C++/ObjC/Python rules: load them from
  `@rules_cc//cc:<rule>.bzl`, `@rules_cc//cc:objc_library.bzl`,
  `@rules_python//python:<rule>.bzl`. `cc_library` rejects `.mm`.
- Legacy macros cannot inspect `select()` values — hence `per_os` dicts.
  Starlark rule attributes (e.g. `shared_lib_name`, string dicts) accept
  `select()`.
- With `-undefined dynamic_lookup`, a missing symbol is not a link error
  but a crash at the call. Check `nm -m -u lib/libusd_ms.dylib | grep
  'dynamically looked up'`: only Python symbols may appear. (Found twice:
  OpenSubdiv's Objective-C++ objects dropped for lack of `alwayslink`, and
  hgiMetal's `-framework Metal` lost on a code-less `cc_library`.)
- Static initialization order: anything pxr static initializers call must
  live in a separate, earlier-initialized shared library.
- Build-only definitions of vendored libraries must be `local_defines`;
  public `defines` reach every pxr target.
- Outputs must not land in another package's output directory; that is
  why usd_ms and its resources live in `//bazel/install`.
- rules_python sets `PYTHONSAFEPATH=1`; the test runner removes it.
  Bazel outputs are read-only; the runner hands testWrapper.py a writable
  copy of the testenv.
- `cc_shared_library`/`cc_binary` compute rpaths for `dynamic_deps`. dyld
  resolves real paths, so Plug (`ArchGetAddressInfo`) sees
  `bazel-out/.../bazel/install/lib/`. **Linux caveat:** glibc reports the
  path the library was loaded from, probably the `_solib_*` symlink, so
  Plug would look next to the symlink.
- A library is exported by usd_ms only if covered by `exports_filter`;
  vendored deps used by pxr headers need their own shared library in
  `PXR_DYNAMIC_DEPS`.
- Shell is zsh: unquoted `$VAR` does not word-split; use `${=VAR}`.
  Globs with no match abort a command; use `find`.
- Use the session scratchpad for downloads; vendored sources must come from
  official release tarballs with recorded SHA-256.

## Phase 5 tasks

1. **CI:** a job running `cmake2bazel.py --check`, `bazel build //...` and
   `bazel test //...` on macOS (and Linux once it builds). Decide with the
   user where CI runs (GitHub Actions on the fork?) and whether GPU tests
   run there (Metal on GitHub's macOS runners).
2. **Wheel:** `py_wheel` for the pxr package (`lib/python/pxr` plus
   `libusd_ms`, `libtbb`, `libopensubdiv` and resources). Needs its own
   rpaths for the installed layout (extensions find `../../../` libs) and a
   decision on the Python tag/platform; compare with the `usd-core` wheel.
3. **Optional `bazel vendor`** for fully offline builds of the BCR rule
   sets and the hermetic Python.
4. **Linux bring-up** (when the user wants it): OpenGL and X11 come from the
   system (`-lGL`, `-lX11`); fix the Plug resource path caveat above;
   OpenSubdiv's GL evaluators are not built (only patch shader sources).
5. Deferred features for later phases, each needing a user decision:
   usdview (PySide), exec, MaterialX (would re-enable 2 excluded tests).

## Useful commands

```sh
bazel build //...                          # everything
bazel test //...                           # ~1500 tests
bazel test //pxr/imaging/hdSt/...          # one package's tests
python3 bazel/tools/cmake2bazel.py --write # regenerate BUILD files
python3 bazel/tools/cmake2bazel.py --check # fail on drift
cat bazel-testlogs/pxr/imaging/hdSt/<test>/test.log   # testWrapper/idiff output
bazel-bin/vendor/openimageio/idiff a.png b.png        # compare images
```
