# Bazel build for OpenUSD — Phase 3 handoff

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
- **Vendoring:** C++ deps as source under `vendor/<dep>/` with our own
  BUILD.bazel and a README (version, URL, SHA-256, local changes). Bazel rule
  sets and the hermetic Python come from BCR.
- **Linking model:** each module is a static `cc_library` with
  `alwayslink = True` (TfType/TF_REGISTRY_FUNCTION need static initializers).
  All of them, core plugins included, are linked into one monolithic shared
  library, `//bazel/install:usd_ms`; Python extensions and programs link
  dynamically against it, so there is one copy of each registry.
- **Install tree:** `//bazel/install` mirrors CMake's install layout so Plug
  finds resources relative to the monolithic library as in an installed
  build (see below).

## Phases

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Skeleton, oneTBB, macro, pxr.h, arch + tf | `bazel build //...`, smoke tests pass |
| 1 ✅ | Rest of pxr/base + Python | `from pxr import Tf, Gf, Vt` works |
| 2 ✅ | pxr/usd/\*, usdValidation, CLI tools, plugInfo substitution | open a stage from Python; `bazel run` usdcat |
| **3** | **Port tests (`pxr_build_test`/`pxr_register_test`, baseline diffs from `cmake/macros/testWrapper.py`)** | **core tests pass** |
| 4 | Imaging: vendor OpenSubdiv 3.6.1; hgiMetal (default), hgiGL, hd, hdSt, hdx, usdImaging | headless Storm render matches baseline |
| 5 | `cmake2bazel --check` in CI, `py_wheel`, optional `bazel vendor` | — |

## What exists (Phases 0–2)

- `MODULE.bazel`: Bazel 9.3.0 (`.bazelversion`); `apple_support` 2.10.1
  (`repo_name = "build_bazel_apple_support"`, declared before rules_cc),
  `bazel_skylib` 1.9.2, `platforms` 1.1.0, `rules_cc` 0.2.26,
  `rules_python` 2.4.2 with hermetic Python 3.11 (matches VERSIONS.md).
  Commit `MODULE.bazel.lock` with any dependency change.
- `.bazelrc`: `-std=c++17`, macOS min 13.0 via `--enable_platform_specific_config`.
- `vendor/onetbb`: oneTBB 2021.12.0 → `//vendor/onetbb:tbb` (`alwayslink`,
  so usd_ms carries all of tbb for the objects linked against it).
- `pxr/BUILD.bazel` (hand-written): `//pxr` = generated `pxr.h` +
  project-wide defines (matches `CXXDefaults.cmake` for core; the
  `PXR_*_SUPPORT_ENABLED` defines in `Packages.cmake` are imaging-only and
  belong to Phase 4).
- `cmake/macros/BUILD.bazel`: exports `moduleDeps.cpp.in`.
- **`//bazel/install`** (hand-written BUILD, rules in
  `bazel/pxr_install.bzl` and `bazel/pxr_python.bzl`):
  - `:usd_ms` — `cc_shared_library` over `PXR_LIBRARIES` →
    `lib/libusd_ms.dylib` (`.so` on Linux, via `select()` on
    `shared_lib_name`), `exports_filter` `//pxr/...` + `//vendor/...`,
    `-undefined dynamic_lookup` on macOS, install name
    `@rpath/libusd_ms.dylib`.
  - `:resources` — `pxr_install_resources` over `PXR_RESOURCES`:
    `lib/usd/plugInfo.json` and `plugin/usd/plugInfo.json` (contents as
    written by `pxr_setup_plugins`), each library's resources under
    `lib/usd/<lib>/resources/` and each core plugin's under
    `plugin/usd/<plugin>/resources/`. plugInfo.json placeholders as in a
    monolithic build: `LibraryPath` "", `ResourcePath` "resources", `Root`
    "..", `PLUG_INFO_PLUGIN_NAME` "pxr.<lib>". `<src>:<dest>` and
    `<src>:no_subst` entries are honoured. Files are copied, not symlinked.
  - `:python` — `py_library` with `imports = ["lib/python"]`:
    `lib/python/pxr/__init__.py` (`__all__`), `lib/python/pxr/<Module>/`
    with the module's pyfiles and `_<lib>.so` (`cc_shared_library` targets
    named `lib/python/pxr/<Module>/_<lib>`, `dynamic_deps = [usd_ms]`);
    carries usd_ms and resources as data.
- **Generator** `bazel/tools/cmake2bazel.py` (`--write`, `--check`, or no
  flag to print). Starting at `pxr/CMakeLists.txt` it follows
  `add_subdirectory()` into `SCOPE` (`pxr/external`, `pxr/base`,
  `pxr/usd`, `pxr/usdValidation`) and interprets: `set` (incl. `CACHE`),
  `unset`, `option`, `list(APPEND|PREPEND|REMOVE_ITEM)`, `if/elseif/else`
  (NOT, AND, OR, parentheses, DEFINED, TARGET, COMMAND, EXISTS, STREQUAL,
  MATCHES, EQUAL/LESS/GREATER, VERSION_*, IN_LIST; empty condition is
  false), `foreach` (plain and `IN LISTS/ITEMS`), `function`/`macro`
  definitions (bodies never run; calls ignored), `return`,
  `message(FATAL_ERROR)`, `find_program`/`find_library`/... (always
  `<VAR>-NOTFOUND`: never probes the host), `add_definitions(-D…)`,
  `pxr_library`, `pxr_plugin`, `pxr_cpp_bin`, `pxr_python_bin`,
  `target_compile_definitions`, `configure_file`. Other `pxr_*` commands are
  errors unless listed in `IGNORED_PXR_COMMANDS` (the test macros, for
  now); non-pxr commands are ignored. Option defaults come from evaluating
  `cmake/defaults/Options.cmake`; platform variables are in `PLATFORM_VARS`.
  Runs once per OS (macos, linux) and puts differences under `per_os`.
  `INCLUDE_SCHEMA_FILES` reads `generatedSchema.classes.txt` like
  `_pxr_parse_generated_schema_classes`. Writes `pxr/**/BUILD.bazel` and
  `bazel/pxr_libraries.bzl` (`PXR_LIBRARIES`, `PXR_PYTHON_MODULES`,
  `PXR_RESOURCES`, all in CMake order).
- `bazel/pxr_library.bzl`:
  - `pxr_library(name, plugin, libraries, public_classes, …, resource_files,
    doxygen_files, copts, linkopts, defines, local_defines,
    pymodule_local_defines, generated_headers, per_os)` →
    `:<name>` static alwayslink `cc_library` (+ `objc_library` for `.mm`),
    generated `moduleDeps.cpp`, `:_<name>` extension objects (also gets
    `generatedSchema.module.h`, which schema `module.cpp` files include from
    their directory), `:<name>_pyfiles`, `:<name>_resources`
    (`pxr_resources` provider). Sources and headers are deduplicated, as
    CMake target sources are sets.
  - `pxr_cpp_bin(name, libraries)` → `cc_binary` from `<name>.cpp`,
    `dynamic_deps = [usd_ms]`, links `current_py_cc_libs`, resources as
    data.
  - `pxr_python_bin(name, python_file)` → `py_binary` depending on
    `//bazel/install:python`.
  - `pxr_configure_file(name, src, out, substitutions)`.
- Generated BUILD files: boost, all of `pxr/base`, 22 `pxr/usd` libraries,
  the `usdShaders` core plugin, 8 `usdValidation` libraries, and tools
  (`sdfdump`, `sdffilter`, `usdcat`, `usddiff`, `usddumpcrate`, `usdedit`,
  `usdresolve`, `usdstitch`, `usdstitchclips`, `usdtree`, `usdupdatecrate`,
  `usdchecker`).
- `bazel/smoke/`: `archSmoke`, `tfSmoke` (C++, static), `pythonSmoke`
  (pxr/base modules), `usdSmoke` (stages, file formats, schema fallbacks,
  usdShaders/Sdr, schema libraries), `usdcatSmoke`, `usdcheckerSmoke`.

Verified at the end of Phase 2 on macOS arm64: `bazel build //...`,
`bazel test //...` (6 tests), `cmake2bazel.py --check` (56 files), and
`--help` of all 12 tools.

## Gotchas already learned

- Bazel 9 has no native C++/ObjC/Python rules: load them from
  `@rules_cc//cc:<rule>.bzl`, `@rules_cc//cc:objc_library.bzl`,
  `@rules_python//python:<rule>.bzl`. `cc_library` rejects `.mm`.
- Legacy macros cannot inspect `select()` values — hence `per_os` dicts.
  Starlark rule attributes (e.g. `shared_lib_name`) do accept `select()`.
- apple_support's toolchain enables `-Wthread-safety`; the macro adds
  `-Wno-thread-safety-analysis` on macOS to match CMake.
- Headers are included repo-root relative; no include remapping. Bazel
  never adds package dirs to include paths, but a file's own directory is
  searched for `"..."` includes — such files must still be declared inputs
  (sandbox), e.g. `generatedSchema.module.h`.
- All headers, including private ones, go in `hdrs`.
- `PXR_BUILD_TESTS` defaults to ON and the generator evaluates it that way
  (e.g. `_ts` includes the tsTest framework).
- Outputs must not land in another package's output directory; that is
  why usd_ms and its resources moved from `//pxr` to `//bazel/install`
  (`bazel-bin/pxr/usd/<lib>/` belongs to `//pxr/usd/<lib>`).
- `cc_shared_library`/`cc_binary` compute rpaths for `dynamic_deps`
  (`@loader_path/../../../../_solib_darwin_arm64/_Ubazel_Sinstall_Slib`
  etc.); they work from bazel-bin and runfiles. dyld resolves the real
  path of loaded images, so Plug (`ArchGetAddressInfo`) sees
  `bazel-out/.../bazel/install/lib/` and finds `usd/` next to it.
  **Linux caveat:** glibc reports the path the library was loaded from,
  probably the `_solib_k8/...` symlink, so Plug would look next to the
  symlink. Needs a fix when Linux is brought up (e.g. an explicit rpath to
  `bazel/install/lib`, or resources next to the solib link).
- A library is exported by usd_ms only if covered by `exports_filter`;
  anything else in an extension's or program's closure gets linked into it
  statically (a second copy). Keep vendored deps under `//vendor`, pxr code
  under `//pxr`.
- `pxr_python_package` assumes every module in `PXR_PYTHON_MODULES` has
  `pymodule_files` (true so far).
- **Not built (open decision):** `usdGenSchema`, `usdInitSchema`,
  `usdgenschemafromsdr` need Jinja2 (`JINJA2_FOUND`), which the generator
  leaves undefined. Building them needs a Python dependency
  (rules_python `pip.parse` with a lock file, or vendored wheels); ask the
  user before adding one.
- Shell is zsh: unquoted `$VAR` does not word-split; use `${=VAR}`.
- Use the session scratchpad for downloads; vendored sources must come from
  official release tarballs with recorded SHA-256.

## Phase 3 tasks

In scope (`pxr/external`, `pxr/base`, `pxr/usd`, `pxr/usdValidation`) there
are 144 `pxr_build_test`, 894 `pxr_register_test`, 35 `pxr_test_scripts`,
31 `pxr_build_test_plugin` and 16 `pxr_create_test_module` calls (see
`cmake/macros/Public.cmake`).

1. **Test runner.** `pxr_register_test` runs commands through
   `cmake/macros/testWrapper.py` (testenv dir copied to a temp dir,
   `--diff-compare` against `baseline/`, `--expected-return-code`,
   `--env-var`, `--pre-path`/`--post-path`, pre/post commands with
   redirects, `--files-exist`, `--clean-output-paths`, ...). Reuse
   testWrapper.py itself as the Bazel test driver (a `py_binary`), passing
   the same arguments, rather than reimplementing it.
2. **Generator:** translate the test macros (remove them from
   `IGNORED_PXR_COMMANDS`). Likely shape: `pxr_build_test` → `cc_binary`
   linked against usd_ms; `pxr_test_scripts` → Python files available to
   tests; `pxr_build_test_plugin` → `cc_shared_library` plugin plus its
   `<name>_plugInfo.json` (Public.cmake ~line 516–600);
   `pxr_create_test_module` → a test Python package with plugInfo;
   `pxr_register_test` → a test target invoking the runner. `COMMAND`
   strings reference `${CMAKE_INSTALL_PREFIX}/tests/<prog>` and
   `${CMAKE_INSTALL_PREFIX}/bin/<tool>` — map them to labels. Tests set
   `PXR_PLUGINPATH_NAME` and `PYTHONPATH` via `ENV`/`PRE_PATH`.
3. **Order:** pxr/base first (tf has the most special cases: Windows-only
   tests, `if (NOT EMSCRIPTEN)`, SIGFPE/SIGSEGV expected-crash tests), then
   usd, then usdValidation. Tag known-broken tests rather than dropping
   them, and record why.
4. Keep `bazel/smoke` until the ported tests cover the same ground.

## Done criteria for Phase 3

- `bazel test //...` passes on macOS arm64, with any excluded tests
  tagged and listed with reasons.
- `cmake2bazel.py --check` reports no drift; no hand-written lists in
  generated BUILD files.
- Commit on `bazel-build` with a descriptive message (end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push to
  `origin` only when asked. Update this file into `PHASE4.md`.

## Useful commands

```sh
bazel build //...                          # everything
bazel test //...                           # tests
python3 bazel/tools/cmake2bazel.py --write # regenerate BUILD files
python3 bazel/tools/cmake2bazel.py --check # fail on drift
bazel run //pxr/usd/bin/usdcat -- file.usda
ls bazel-bin/bazel/install/{lib,lib/usd,plugin/usd}  # install tree
```
