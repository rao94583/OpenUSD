# Bazel build for OpenUSD — Phase 2 handoff

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
  All of them are linked into one monolithic shared library, `//pxr:usd_ms`;
  every Python extension links dynamically against it, so there is one copy
  of each registry.

## Phases

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Skeleton, oneTBB, macro, pxr.h, arch + tf | `bazel build //...`, smoke tests pass |
| 1 ✅ | Rest of pxr/base + Python | `from pxr import Tf, Gf, Vt` works |
| **2** | **pxr/usd/\*, usdValidation, CLI tools, plugInfo substitution** | **open a stage from Python; `bazel run` usdcat** |
| 3 | Port tests (`pxr_build_test`/`pxr_register_test`, baseline diffs from `cmake/macros/testWrapper.py`) | core tests pass |
| 4 | Imaging: vendor OpenSubdiv 3.6.1; hgiMetal (default), hgiGL, hd, hdSt, hdx, usdImaging | headless Storm render matches baseline |
| 5 | `cmake2bazel --check` in CI, `py_wheel`, optional `bazel vendor` | — |

## What exists (Phases 0–1)

- `MODULE.bazel`: Bazel 9.3.0 (`.bazelversion`); `apple_support` 2.10.1
  (`repo_name = "build_bazel_apple_support"`, declared before rules_cc),
  `bazel_skylib` 1.9.2, `platforms` 1.1.0, `rules_cc` 0.2.26,
  `rules_python` 2.4.2 with hermetic Python 3.11 (matches VERSIONS.md).
  Commit `MODULE.bazel.lock` with any dependency change.
- `.bazelrc`: `-std=c++17`, macOS min 13.0 via `--enable_platform_specific_config`.
- `vendor/onetbb`: oneTBB 2021.12.0 → `//vendor/onetbb:tbb` (`alwayslink`,
  so usd_ms carries all of tbb for the objects linked against it).
- `pxr/BUILD.bazel` (hand-written): `//pxr` = generated `pxr.h` +
  project-wide defines; `//pxr:usd_ms` = `cc_shared_library` over
  `PXR_LIBRARIES`, `exports_filter` `//pxr/...` and `//vendor/...`,
  `-undefined dynamic_lookup` on macOS. Install name `@rpath/libusd_ms.dylib`
  (Bazel's default).
- **Generator** `bazel/tools/cmake2bazel.py` (`--write`, `--check`, or no
  flag to print). Starting at `pxr/CMakeLists.txt` it follows
  `add_subdirectory()` into the directories in `SCOPE` (now
  `pxr/external`, `pxr/base`) and interprets: `set` (incl. `CACHE`), `unset`,
  `option`, `list(APPEND|PREPEND|REMOVE_ITEM)`, `if/elseif/else` (NOT, AND,
  OR, parentheses, DEFINED, TARGET, COMMAND, EXISTS, STREQUAL, MATCHES,
  EQUAL/LESS/GREATER, VERSION_*, IN_LIST), `foreach` (plain and `IN
  LISTS/ITEMS`), `function`/`macro` definitions (bodies never run; calls are
  ignored), `return`, `message(FATAL_ERROR)`, `pxr_library`,
  `target_compile_definitions`, `configure_file`. Everything else is ignored.
  Option defaults come from evaluating `cmake/defaults/Options.cmake`;
  platform variables (`APPLE`, `CMAKE_DL_LIBS`, `M_LIB`, `PYTHON_LIBRARIES`,
  ...) are in `PLATFORM_VARS`. Runs once per OS (macos, linux) and puts
  differences under `per_os`; differences in attributes `per_os` does not
  support are errors. Unsupported constructs raise errors rather than being
  skipped silently. Writes:
  - `pxr/**/BUILD.bazel` for every directory with a `pxr_library()`;
  - `bazel/pxr_libraries.bzl`: `PXR_LIBRARIES` (labels in CMake order) and
    `PXR_PYTHON_MODULES` (`struct(library, module, package)` for each
    library with `PYMODULE_CPPFILES`, in CMake order).
  LIBRARIES translation: pxr library names → labels; `EXTERNAL_LABELS`
  (`TBB::tbb`, `Python3::Python` → `current_py_cc_headers`); `-…` flags →
  `linkopts` (kept as groups, e.g. `-framework Foundation`); other bare
  names → `-l<name>`. Generator expressions are errors unless listed in
  `DROPPED_GENEX` (boost/python's NDEBUG). `target_compile_definitions` on
  a library → `defines`/`local_defines`, on `_<lib>` →
  `pymodule_local_defines`, on other targets (tests) → ignored.
  `configure_file(x.h.in ${PROJECT_BINARY_DIR}/include/<dir>/x.h)` →
  `pxr_configure_file` + `generated_headers`; other outputs ignored.
- `bazel/pxr_library.bzl`: `pxr_library(name, libraries, public_classes,
  python_public_classes, public_headers, python_public_headers,
  private_classes, python_private_classes, private_headers,
  python_private_headers, cppfiles, python_cppfiles, pymodule_cppfiles,
  pymodule_files, resource_files, doxygen_files, copts, linkopts, defines,
  local_defines, pymodule_local_defines, generated_headers, per_os)` and
  `pxr_configure_file(name, src, out, substitutions)`. Per library it makes:
  - `:<name>` static `cc_library` (alwayslink) with `MFB_*`,
    `PXR_BUILD_LOCATION`, `<NAME>_EXPORTS` defines; `.mm` files under
    `per_os["macos"]` go to a companion `objc_library`;
  - `moduleDeps.cpp` (rule `pxr_module_deps_cpp`, template
    `//cmake/macros:moduleDeps.cpp.in`) compiled into the library when
    `pymodule_cppfiles` is set and `python_cppfiles` has no
    `moduleDeps.cpp`; reqs are the `//pxr/...` entries of `libraries`
    (external ones skipped);
  - `:_<name>` `cc_library` with the extension's objects (MFB defines +
    `pymodule_local_defines`), `:<name>_pyfiles` filegroup,
    `:<name>_resources` filegroup (placeholder, see Phase 2).
- `bazel/pxr_python.bzl`: `pxr_python_package(name, modules, monolithic)`
  builds `pxr/<Module>/_<lib>.so` as `cc_shared_library` targets (names
  with slashes) with `dynamic_deps = [usd_ms]`, symlinks each module's
  pyfiles into `pxr/<Module>/` (paths relative to the package, so tf's
  `testenv/*.py` land in `pxr/Tf/testenv/` as in CMake), writes
  `pxr/__init__.py` (`__all__`), and wraps it all in a `py_library` with
  `imports = ["."]`. Used by `//bazel/python:pxr`.
- Generated BUILD files for `pxr/external/boost{,/python}` and all of
  `pxr/base` (arch, tf, gf, pegtl, js, trace, work, plug, vt, ts).
- `bazel/smoke/`: `archSmoke`, `tfSmoke` (C++, static linking; executables
  using tf also need `@rules_python//python/cc:current_py_cc_libs`), and
  `pythonSmoke` (py_test importing all pxr/base modules, exercising Tf, Gf,
  Vt, Work, Plug, Trace, Ts and checking types are shared across modules).

Verified at the end of Phase 1: `bazel build //...`, `bazel test //...` (3
tests) and `cmake2bazel.py --check` pass on macOS arm64. At runtime the
process loads one libpython (the interpreter's), one `libusd_ms.dylib`, and
the extensions reference pxr symbols as undefined-from-libusd_ms (no
duplicate copies).

## Gotchas already learned

- Bazel 9 has no native C++/ObjC rules: load `cc_library`, `cc_test`,
  `cc_binary`, `cc_shared_library` from `@rules_cc//cc:<rule>.bzl`,
  `objc_library` from `@rules_cc//cc:objc_library.bzl`, `py_library`/`py_test`
  from `@rules_python//python:<rule>.bzl`. `cc_library` rejects `.mm`.
- Legacy macros cannot inspect `select()` values — hence `per_os` dicts.
- apple_support's toolchain enables `-Wthread-safety`; the macro adds
  `-Wno-thread-safety-analysis` on macOS to match CMake.
- Headers are included repo-root relative; no include remapping. arch, tf
  and gf disable CMake's current-dir includes (own `math.h`, `semaphore.h`,
  `half.h`); Bazel never adds package dirs, keep it that way.
- All headers, including private ones, go in `hdrs`.
- `PXR_BUILD_TESTS` defaults to ON and the generator evaluates it that way,
  so e.g. `_ts` includes the tsTest framework with
  `TS_BUILD_TEST_FRAMEWORK` — consistent with porting tests in Phase 3.
- `cc_shared_library` computes rpaths for `dynamic_deps` itself
  (`@loader_path/../../../../_solib_darwin_arm64/_Upxr` etc.) and they are
  correct for both bazel-bin and runfiles; dyld resolves `@loader_path`
  from the real path. An installed layout (wheel) will need its own rpath —
  Phase 5.
- A library is "exported" by `usd_ms` only if covered by `exports_filter`;
  anything else in an extension's closure gets linked into it statically
  (a second copy). Keep new vendored deps under `//vendor` and pxr code
  under `//pxr`.
- `pxr_python_package` assumes every module in `PXR_PYTHON_MODULES` has
  `pymodule_files` (true so far). CMake only lists modules with
  `PYMODULE_CPPFILES` in `__all__`; pure-Python packages (none yet) would
  need a second list.
- Shell is zsh: unquoted `$VAR` does not word-split; use `${=VAR}`.
- Use the session scratchpad for downloads; vendored sources must come from
  official release tarballs with recorded SHA-256.

## Phase 2 tasks

1. **Extend `SCOPE`** with `pxr/usd`, then `pxr/usdValidation`; run
   `cmake2bazel.py` and fix what it rejects. Commands seen in those
   CMakeLists that the generator does not yet handle: `pxr_plugin` (4
   uses; in a monolithic build core plugins are `OBJECT_PLUGIN`s linked
   into usd_ms — see `Public.cmake` ~line 316), `add_definitions` (2),
   `file` (3), `find_program` (3), `pxr_python_bin` (12), `pxr_cpp_bin` (8).
   `usd` checks in `codegenTemplates/moduleDeps.cpp` as a resource, not a
   python cppfile, so the generated moduleDeps rule still applies there.
2. **Resources and plugInfo.json.** Replace the `<name>_resources`
   placeholder: install `resource_files` under
   `<dir of libusd_ms>/usd/<name>/resources/`, substituting plugInfo.json
   per `_plugInfo_subst` in `cmake/macros/Private.cmake` (~line 159:
   `PLUG_INFO_ROOT`, `PLUG_INFO_PLUGIN_NAME`, `PLUG_INFO_LIBRARY_PATH`,
   `PLUG_INFO_RESOURCE_PATH`), plus the top-level `usd/plugInfo.json` that
   includes `*/resources/`. Plug finds them through
   `PXR_BUILD_LOCATION=usd` relative to the library containing plug, i.e.
   next to `pxr/libusd_ms.dylib` in bazel-bin and runfiles. Make sure
   `//bazel/python:pxr` and binaries carry them as runfiles.
3. **CLI tools:** `pxr_python_bin` → `py_binary` depending on
   `//bazel/python:pxr` (usdcat etc. live in `pxr/usd/bin/`);
   `pxr_cpp_bin` → `cc_binary` linking `usd_ms` dynamically
   (`dynamic_deps`) plus `current_py_cc_libs`.
4. **Smoke tests:** Python test that opens an in-memory stage, defines a
   prim, exports it to a string, and reads a `.usda` file back;
   `bazel run //pxr/usd/bin/usdcat -- <file>` works.

## Done criteria for Phase 2

- `bazel build //...` and `bazel test //...` pass on macOS arm64.
- `cmake2bazel.py --check` reports no drift.
- No hand-written lists in generated BUILD files.
- Commit on `bazel-build` with a descriptive message (end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push to
  `origin` only when asked. Update this file into `PHASE3.md`.

## Useful commands

```sh
bazel build //...                          # everything
bazel test //...                           # smoke tests
python3 bazel/tools/cmake2bazel.py --write # regenerate BUILD files
python3 bazel/tools/cmake2bazel.py --check # fail on drift
otool -L bazel-bin/bazel/python/pxr/Tf/_tf.so   # linkage
```
