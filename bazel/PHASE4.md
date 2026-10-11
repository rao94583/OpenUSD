# Bazel build for OpenUSD — Phase 4 handoff

Self-contained context for continuing the Bazel build of this OpenUSD fork.
Repo: `~/Code/OpenUSD`, branch `bazel-build` (pushed to `origin` =
github.com/rao94583/OpenUSD; `upstream` = Pixar, never push there).

## Goal and settled decisions

Bazel build of OpenUSD with Python bindings, dependencies vendored as source.

- **Scope:** core (`pxr/base`, `pxr/usd`, `usdValidation`, CLI tools, Python)
  plus imaging (Hydra/Storm/usdImaging, OpenSubdiv). usdview, exec and
  optional plugins come later.
- **Platform:** macOS arm64 now; keep Linux possible (all platform
  differences through `select()`/`per_os`, no macOS-only assumptions).
- **CMake stays** as the reference build. BUILD.bazel files under `pxr/` are
  generated from CMakeLists.txt by `bazel/tools/cmake2bazel.py`; never
  hand-edit them, fix the generator instead.
- **Vendoring:** C++ and Python deps as source under `vendor/<dep>/` with
  our own BUILD.bazel and a README (version, URL, SHA-256, local changes);
  nothing is fetched from PyPI. Bazel rule sets and the hermetic Python come
  from BCR.
- **Linking model:** each module is a static `cc_library` with
  `alwayslink = True` (TfType/TF_REGISTRY_FUNCTION need static initializers).
  All of them, core plugins included, are linked into one monolithic shared
  library, `//bazel/install:usd_ms`. **oneTBB is a separate shared library**
  (`//bazel/install:tbb`), as in CMake builds: pxr static initializers call
  into tbb, and only a separate image is guaranteed to be initialized first
  (work's thread limits crashed otherwise). Python extensions, programs and
  tests link both dynamically (`PXR_DYNAMIC_DEPS` in `pxr_library.bzl`).
- **Install tree:** `//bazel/install` mirrors CMake's install layout so Plug
  finds resources relative to the monolithic library as in an installed
  build. Tests mirror CMake's `<prefix>/tests` per package (below).

## Phases

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Skeleton, oneTBB, macro, pxr.h, arch + tf | `bazel build //...`, smoke tests pass |
| 1 ✅ | Rest of pxr/base + Python | `from pxr import Tf, Gf, Vt` works |
| 2 ✅ | pxr/usd/\*, usdValidation, CLI tools, plugInfo substitution | open a stage from Python; `bazel run` usdcat |
| 3 ✅ | Port tests (`pxr_build_test`/`pxr_register_test`, baseline diffs via `cmake/macros/testWrapper.py`) | core tests pass |
| **4** | **Imaging: vendor OpenSubdiv 3.6.1; hgiMetal (default), hgiGL, hd, hdSt, hdx, usdImaging** | **headless Storm render matches baseline** |
| 5 | `cmake2bazel --check` in CI, `py_wheel`, optional `bazel vendor` | — |

## What exists (Phases 0–3)

- `MODULE.bazel`: Bazel 9.3.0 (`.bazelversion`); `apple_support` 2.10.1
  (`repo_name = "build_bazel_apple_support"`, declared before rules_cc),
  `bazel_skylib` 1.9.2, `platforms` 1.1.0, `rules_cc` 0.2.26,
  `rules_python` 2.4.2 with hermetic Python 3.11 (matches VERSIONS.md).
  Commit `MODULE.bazel.lock` with any dependency change.
- `.bazelrc`: `-std=c++17`, macOS min 13.0 via `--enable_platform_specific_config`.
- `vendor/onetbb`: oneTBB 2021.12.0 → `//vendor/onetbb:tbb` (`alwayslink`,
  so libtbb carries all of it).
- `vendor/jinja2` (3.1.6) and `vendor/markupsafe` (3.0.3, pure Python, C
  speedups omitted): `py_library` targets from the PyPI sdists, for the
  schema code generators and test scripts.
- `pxr/BUILD.bazel` (hand-written): `//pxr` = generated `pxr.h` +
  project-wide defines (matches `CXXDefaults.cmake` for core; the
  `PXR_*_SUPPORT_ENABLED` defines in `Packages.cmake` are imaging-only and
  belong to Phase 4).
- `cmake/macros/BUILD.bazel`: exports `moduleDeps.cpp.in`, `testWrapper.py`.
- **`//bazel/install`** (hand-written BUILD, rules in
  `bazel/pxr_install.bzl` and `bazel/pxr_python.bzl`):
  - `:tbb` → `lib/libtbb.dylib`; `:usd_ms` → `lib/libusd_ms.dylib` over
    `PXR_LIBRARIES` (`.so` on Linux, via `select()` on `shared_lib_name`),
    `dynamic_deps = [":tbb"]`, `exports_filter = ["//pxr:__subpackages__"]`,
    `-undefined dynamic_lookup` on macOS, install names `@rpath/<file>`.
  - `:resources` — `lib/usd/plugInfo.json`, `plugin/usd/plugInfo.json`,
    `lib/usd/<lib>/resources/`, `plugin/usd/<plugin>/resources/`, with
    plugInfo.json substituted as in a monolithic build (`LibraryPath` "").
  - `:python` — `py_library` with `imports = ["lib/python"]`: the pxr
    package with `_<lib>.so` extensions; carries usd_ms, tbb, resources.
- **Generator** `bazel/tools/cmake2bazel.py` (`--write`, `--check`, or no
  flag to print). Starting at `pxr/CMakeLists.txt` it follows
  `add_subdirectory()` into `SCOPE` (`pxr/external`, `pxr/base`,
  `pxr/usd`, `pxr/usdValidation`) and interprets: `set` (incl. `CACHE`),
  `unset`, `option`, `list(APPEND|PREPEND|REMOVE_ITEM)`, `if/elseif/else`
  (NOT, AND, OR, parentheses, DEFINED, TARGET, COMMAND, EXISTS, STREQUAL,
  MATCHES, EQUAL/LESS/GREATER, VERSION_*, IN_LIST; empty condition is
  false), `foreach`, **user-defined `function`/`macro` calls** (with ARGN,
  ARGV*, `cmake_parse_arguments`), `return`, `message(FATAL_ERROR)`,
  `find_*` (always `<VAR>-NOTFOUND`), `get_filename_component`,
  `string(REPLACE|CONCAT)`, `get_target_property` (NOTFOUND),
  `add_definitions(-D…)`, `pxr_library`, `pxr_plugin`, `pxr_cpp_bin`,
  `pxr_python_bin`, `target_compile_definitions`, `configure_file`, and for
  tests `pxr_build_test`, `pxr_build_test_plugin`, `pxr_test_scripts`,
  `pxr_create_test_module`, `pxr_register_test`, plus `add_executable`,
  `add_library(SHARED)`, `target_sources`, `target_link_libraries`,
  `set_target_properties(PREFIX|SUFFIX)`, `install(TARGETS|FILES)` into
  `tests/` (boost::python's tests use these directly). Unknown `pxr_*`
  commands and other variable-setting commands (`file`, `math`,
  `execute_process`, `get_property`, `cmake_path`, …) are errors. CMake's
  legacy unquoted arguments (`ENV VAR="a b"`) are tokenized as one argument.
  Option defaults come from evaluating `cmake/defaults/Options.cmake`, with
  `PXR_BUILD_MONOLITHIC=ON` (our build is monolithic; it skips tests CMake
  skips in monolithic builds). Platform variables, find-module results
  (`JINJA2_FOUND`, `M_LIB`, ...) and placeholders (`CMAKE_INSTALL_PREFIX` =
  `<prefix>`, `PYTHON_EXECUTABLE` = `<python>`, `PROJECT_BINARY_DIR` =
  `<build>`) are in `PLATFORM_VARS`. Runs once per OS (macos, linux):
  library differences go under `per_os`; tests registered on some OSes only
  get `target_compatible_with`; other test differences are errors. Writes
  `pxr/**/BUILD.bazel` and `bazel/pxr_libraries.bzl`.
- `bazel/pxr_library.bzl`: `pxr_library`, `pxr_cpp_bin`, `pxr_python_bin`
  (also `exports_files` its source for tests), `pxr_configure_file`,
  `PXR_COPTS` (now with `-I.` and `-I$(BINDIR)`, matching CMake's `-I`
  include dir, for `<pxr/...>` includes), `PXR_DYNAMIC_DEPS`.
- **Tests** (`bazel/pxr_test.bzl`, `bazel/tools/pxr_test_runner.py`): each
  package with tests lays out CMake's `<prefix>/tests` in its own output
  dir:
  - `pxr_test_program(name)` → `cc_binary` `tests/<name>`;
  - `pxr_test_library(name, prefix, suffix)` → `cc_shared_library`
    `tests/lib/<prefix><name><suffix>` (test plugins; boost test modules);
  - `pxr_test_files` → scripts (`tests/<NAME_WE>` or `src:dest`), test
    modules (`tests/<prefix>/lib/python/<mod>/`), substituted test plugin
    plugInfo (`tests/<prefix>/lib/<plugin>/Resources/plugInfo.json`,
    `@TEST_PLUG_INFO_*@` with relative `LibraryPath`), testenvs
    (`tests/ctest/<dir>/`);
  - `pxr_test_tree` filegroup `:test_tree`;
  - `pxr_test(name, arguments, test_trees, tools, python_tools)` → a
    `py_test` running the runner with a JSON spec. `arguments` are
    pxr_register_test's keywords verbatim. Commands referring to
    `<prefix>/tests/X` pull in the test tree of the package installing X;
    `<prefix>/bin/X` pulls in the tool; `PRE_PATH <prefix>/bin` pulls in
    all tools.
  - The runner builds `<prefix>` in `$TEST_TMPDIR` (`lib`, `plugin` →
    //bazel/install; `tests` → merged symlinks of the test trees; `bin` →
    C++ tools symlinked, Python tools installed as their source with the
    `/pxrpythonsubst` shebang rewritten), builds testWrapper.py's arguments
    exactly as pxr_register_test does (TF_FATAL_VERIFY=1, PYTHONPATH =
    `<prefix>/lib/python` + vendored Jinja2/MarkupSafe, `<build>` →
    `$TEST_UNDECLARED_OUTPUTS_DIR`), and runs the real testWrapper.py.
- `bazel/smoke/`: `archSmoke`, `tfSmoke`, `pythonSmoke`, `usdSmoke`,
  `usdcatSmoke`, `usdcheckerSmoke`, `schemaToolsSmoke` (the only coverage
  of usdGenSchema/usdInitSchema; upstream registers no tests for them).

Verified at the end of Phase 3 on macOS arm64: `bazel build //...`;
`bazel test --nocache_test_results //...` — 904 pass, 1 skipped
(`TfMallocTag`, Linux-only), in about 2.5 minutes; `cmake2bazel.py --check`
(57 files). 898 are ported CMake tests (70 boost::python, 200 pxr/base
including the Linux-only one, 628 pxr/usd + usdValidation including the
tools' tests), plus the 7 smoke tests; none are excluded. Corrupting a baseline makes its test fail with `DIFF_COMPARE`.

## Gotchas already learned

- Bazel 9 has no native C++/ObjC/Python rules: load them from
  `@rules_cc//cc:<rule>.bzl`, `@rules_cc//cc:objc_library.bzl`,
  `@rules_python//python:<rule>.bzl`. `cc_library` rejects `.mm`.
- Legacy macros cannot inspect `select()` values — hence `per_os` dicts.
  Starlark rule attributes (e.g. `shared_lib_name`) do accept `select()`.
- apple_support's toolchain enables `-Wthread-safety`; the macro adds
  `-Wno-thread-safety-analysis` on macOS to match CMake.
- Headers are included repo-root relative, with quotes or angle brackets.
  Files a source includes from its own directory must still be declared
  (sandbox): `generatedSchema.module.h` for schema modules; for tests,
  every `.h/.hpp/.inl/.cpp` next to the sources is a `textual_hdrs` of a
  helper library (boost tests include `module_tail.cpp`).
- All headers, including private ones, go in `hdrs`.
- Static initialization order: anything pxr static initializers call must
  live in a separate, earlier-initialized shared library (tbb). Watch for
  this with new vendored deps (OpenSubdiv in Phase 4).
- Outputs must not land in another package's output directory; that is
  why usd_ms and its resources live in `//bazel/install`.
- rules_python sets `PYTHONSAFEPATH=1`; the test runner removes it so test
  scripts can import helpers installed next to them.
- Bazel outputs are read-only; testWrapper.py's testenv copy keeps modes,
  so the runner passes it a writable copy of the testenv.
- `cc_shared_library`/`cc_binary` compute rpaths for `dynamic_deps`; they
  work from bazel-bin and runfiles. dyld resolves real paths, so Plug
  (`ArchGetAddressInfo`) sees `bazel-out/.../bazel/install/lib/`.
  **Linux caveat:** glibc reports the path the library was loaded from,
  probably the `_solib_k8/...` symlink, so Plug would look next to the
  symlink. Needs a fix when Linux is brought up.
- A library is exported by usd_ms only if covered by `exports_filter`;
  anything else in a consumer's closure gets linked into it statically.
  New vendored deps used by pxr headers need their own shared library in
  `PXR_DYNAMIC_DEPS`, like tbb.
- `pxr_python_package` assumes every module in `PXR_PYTHON_MODULES` has
  `pymodule_files` (true so far).
- Shell is zsh: unquoted `$VAR` does not word-split; use `${=VAR}`.
- Use the session scratchpad for downloads; vendored sources must come from
  official release tarballs with recorded SHA-256.

## Phase 4 tasks

1. **Vendor OpenSubdiv 3.6.1** (`VERSIONS.md`) under `vendor/opensubdiv`
   (release tarball, SHA-256, README). Only the CPU parts pxOsd/hdSt need
   (`osd` CPU + Metal/GL evaluators as CMake configures them; check
   `cmake/defaults/Packages.cmake`). Give it its own shared library in
   `//bazel/install` and `PXR_DYNAMIC_DEPS`, like tbb.
2. **Extend `SCOPE`** with `pxr/imaging` and `pxr/usdImaging`; run the
   generator and fix what it rejects. Expect: `PXR_GL_SUPPORT_ENABLED` /
   `PXR_METAL_SUPPORT_ENABLED` (and the matching options) from
   `Packages.cmake`, which `//pxr` or the generator must provide
   per OS; macOS frameworks (Metal, Foundation, AppKit, ...) as `linkopts`;
   `.mm` sources in hgiMetal/garch; shader resources (`.glslfx`) as
   resource files; codegen steps (hdGenSchema uses Jinja2).
   Decide with the user which hgi backends to build (default: hgiMetal on
   macOS, hgiGL on Linux).
3. **Image comparisons:** CMake runs `IMAGE_DIFF_COMPARE` only with
   OpenImageIO's `idiff` (`IMAGE_DIFF_TOOL`), which this build does not
   have; the runner currently skips image diffs exactly as CMake does
   without it. A "Storm render matches baseline" criterion needs an image
   diff: ask the user (vendor OIIO's idiff, or a small comparison tool).
4. Headless rendering on macOS needs Metal (no display); GUI tests are
   gated by `PXR_HEADLESS_TEST_MODE` in CMake.

## Done criteria for Phase 4

- `bazel build //...` and `bazel test //...` pass on macOS arm64, with
  any excluded tests tagged and listed with reasons.
- A headless Storm render test compares against its baseline.
- `cmake2bazel.py --check` reports no drift; no hand-written lists in
  generated BUILD files.
- Commit on `bazel-build` with a descriptive message (end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push to
  `origin` only when asked. Update this file into `PHASE5.md`.

## Useful commands

```sh
bazel build //...                          # everything
bazel test //...                           # ~900 tests
bazel test //pxr/usd/sdf/...               # one package's tests
python3 bazel/tools/cmake2bazel.py --write # regenerate BUILD files
python3 bazel/tools/cmake2bazel.py --check # fail on drift
cat bazel-testlogs/pxr/usd/sdf/<test>/test.log   # testWrapper output
```
