# Bazel build for OpenUSD — Phase 1 handoff

Self-contained context for continuing the Bazel build of this OpenUSD fork.
Repo: `~/Code/OpenUSD`, branch `bazel-build` (pushed to `origin` =
github.com/rao94583/OpenUSD; `upstream` = Pixar, never push there). Phase 0
commit: `0fcb89b8c`.

## Goal and settled decisions

Bazel build of OpenUSD with Python bindings, dependencies vendored as source.

- **Scope:** core (`pxr/base`, `pxr/usd`, `usdValidation`, CLI tools, Python)
  plus imaging (Hydra/Storm/usdImaging, OpenSubdiv). usdview, exec and
  optional plugins come later.
- **Platform:** macOS arm64 now; keep Linux possible (all platform
  differences through `select()`/`per_os`, no macOS-only assumptions).
- **CMake stays** as the reference build. BUILD.bazel files are generated
  from CMakeLists.txt by `bazel/tools/cmake2bazel.py`; never hand-edit
  generated lists, fix the generator instead.
- **Vendoring:** C++ deps as source under `vendor/<dep>/` with our own
  BUILD.bazel and a README (version, URL, SHA-256, local changes). Bazel rule
  sets and the hermetic Python come from BCR.
- **Linking model:** each module is a static `cc_library` with
  `alwayslink = True` (TfType/TF_REGISTRY_FUNCTION need static initializers).
  All modules get linked into **one monolithic shared library**; every Python
  extension links dynamically against it so there is one copy of each
  registry.

## Phases

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Skeleton, oneTBB, macro, pxr.h, arch + tf | `bazel build //...`, smoke tests pass |
| **1** | **Rest of pxr/base + Python** | **`from pxr import Tf, Gf, Vt` works** |
| 2 | pxr/usd/*, usdValidation, CLI tools, plugInfo substitution | open a stage from Python; `bazel run` usdcat |
| 3 | Port tests (`pxr_build_test`/`pxr_register_test`, baseline diffs from `cmake/macros/testWrapper.py`) | core tests pass |
| 4 | Imaging: vendor OpenSubdiv 3.6.1; hgiMetal (default), hgiGL, hd, hdSt, hdx, usdImaging | headless Storm render matches baseline |
| 5 | `cmake2bazel --check` in CI, `py_wheel`, optional `bazel vendor` | — |

## What exists (Phase 0)

- `MODULE.bazel`: Bazel 9.3.0 (`.bazelversion`); `apple_support` 2.10.1
  (`repo_name = "build_bazel_apple_support"`, declared before rules_cc),
  `bazel_skylib` 1.9.2, `platforms` 1.1.0, `rules_cc` 0.2.26,
  `rules_python` 2.4.2 with hermetic Python 3.11 (matches VERSIONS.md).
  Commit `MODULE.bazel.lock` with any dependency change.
- `.bazelrc`: `-std=c++17`, macOS min 13.0 via `--enable_platform_specific_config`.
- `vendor/onetbb`: oneTBB 2021.12.0 → `//vendor/onetbb:tbb`.
- `pxr/BUILD.bazel`: `//pxr` = generated `pxr.h` (from `pxr.h.in`, values in
  `bazel/version.bzl`) + project-wide defines.
- `bazel/pxr_library.bzl`: `pxr_library(name, libraries, public_classes,
  python_public_classes, public_headers, python_public_headers,
  private_classes, python_private_classes, private_headers,
  python_private_headers, cppfiles, python_cppfiles, pymodule_cppfiles,
  pymodule_files, resource_files, doxygen_files, copts, linkopts, per_os)`.
  Sets `MFB_PACKAGE_NAME/MFB_ALT_PACKAGE_NAME=<name>`,
  `MFB_PACKAGE_MODULE=<Name>`, `PXR_BUILD_LOCATION=usd`,
  `PXR_PLUGIN_BUILD_LOCATION=../plugin/usd`, `<NAME>_EXPORTS=1`. Any
  `python_*` arg adds deps on `//pxr/external/boost/python` and
  `@rules_python//python/cc:current_py_cc_headers`. `per_os = {"macos"|"linux":
  {libraries, public_headers, private_headers, cppfiles, copts, linkopts}}`
  becomes `select()`s. `.mm` files (only allowed under `per_os["macos"]`) go to
  a companion `objc_library` (`non_arc_srcs`; CMake never enables ARC).
  `pymodule_*` and `resource_files` currently only produce
  `<name>_pymodule_srcs` / `<name>_resources` filegroups — **Phase 1 replaces
  the pymodule placeholder.**
- BUILD.bazel for `pxr/base/arch` (hand-written `per_os`), `pxr/base/tf`,
  `pxr/external/boost`, `pxr/external/boost/python`.
- `bazel/tools/cmake2bazel.py <CMakeLists.txt> --labels name=label ...`:
  prints translated `pxr_library()` calls. Maps `TBB::tbb` and
  `${PYTHON_LIBRARIES}`; warns about (and drops) other `${VAR}` tokens. Does
  not evaluate `set()`/`if()`. Generated files start with a
  `# Generated from CMakeLists.txt…` comment and the `pxr_library` load line;
  the header is currently prepended by hand (shell), not by the script.
- `bazel/smoke/`: `archSmoke`, `tfSmoke` (checks a TfType registered only via
  an unreferenced TF_REGISTRY_FUNCTION). Executables using tf also need
  `@rules_python//python/cc:current_py_cc_libs`.

## Gotchas already learned

- Bazel 9 has no native C++/ObjC rules: load `cc_library`, `cc_test`,
  `cc_binary`, `cc_shared_library` from `@rules_cc//cc:<rule>.bzl` and
  `objc_library` from `@rules_cc//cc:objc_library.bzl`. `cc_library` rejects
  `.mm`.
- Legacy macros cannot inspect `select()` values — hence `per_os` dicts.
- apple_support's toolchain enables `-Wthread-safety`; the macro adds
  `-Wno-thread-safety-analysis` on macOS to match CMake.
- Upstream oneTBB BUILD passes x86-only `-mwaitpkg`; ours selects it by CPU.
- Headers are included repo-root relative (`"pxr/base/tf/token.h"`), which
  Bazel's `-iquote .` / bin dir already provide; no include remapping. arch
  has its own `math.h` (CMake disables current-dir includes for it); keep it
  that way.
- All headers, including private ones, go in `hdrs` (CMake exposes private
  headers in the build tree).
- Shell is zsh: unquoted `$VAR` does not word-split; use `${=VAR}`.
- Use the session scratchpad for downloads; vendored sources must come from
  official release tarballs with recorded SHA-256.

## Phase 1 tasks

1. **Generator: evaluate CMake variables.** Make `cmake2bazel.py` interpret
   the small CMake subset used in `pxr/base`: `set()`, `list(APPEND)`,
   `if/elseif/else/endif` over `WIN32`, `APPLE`, `EMSCRIPTEN`, `PXR_*` options
   (CMake defaults from `cmake/defaults/Options.cmake`) and `TARGET x`.
   Evaluate once per OS (macos, linux); where results differ, emit `per_os`.
   Regenerate arch so its `per_os` is no longer hand-written. Add
   `--write` (rewrite BUILD.bazel) and `--check` (fail on drift) modes.
2. **BUILD files for the rest of `pxr/base`**, in CMake order: `gf`, `pegtl`,
   `js`, `trace`, `work`, `plug`, `vt`, `ts` (deps: gf→arch,tf; js→tf;
   pegtl→arch; trace→arch,js,tf,tbb; work→tf,trace,tbb; plug→arch,tf,js,
   trace,work,tbb; vt→arch,tf,gf,trace,tbb; ts→vt,gf,tf). Specifics:
   - `ts` builds all its lists via `set(libs …)`, `set(classes …)` etc.
     — needs task 1.
   - `work` selects its implementation; use the default `workTBB`
     (`work_impl_classes`/`work_impl_headers` under `workTBB/`), and generate
     `pxr/base/work/impl.h` from `impl.h.in` with `@PXR_WORK_IMPL_HEADER@` →
     `"pxr/base/work/workTBB/impl.h"` (an `expand_template` in that package;
     the generator should emit it, or a small `pxr_configure_file` helper).
   - `gf`'s `gfGenCode.py` step only runs with `PXR_VALIDATE_GENERATED_CODE`;
     the generated sources are checked in — skip it.
   - `plug` has an `if` on `PXR_APPLE_FRAMEWORK_RELATIVE_RESOURCES` (off) —
     ignore.
3. **Python extension modules** (new `bazel/pxr_python.bzl`, used by
   `pxr_library` when `pymodule_cppfiles` is set):
   - Generate `moduleDeps.cpp` from `cmake/macros/moduleDeps.cpp.in`
     (logic in `cmake/macros/genModuleDepsCpp.cmake`): a
     `TF_REGISTRY_FUNCTION(TfScriptModuleLoader)` calling
     `RegisterLibrary(TfToken("<name>"), TfToken("pxr.<Name>"), reqs)`, where
     `reqs` are `TfToken("<lib>")` for each **pxr** library in `libraries`
     (skip external ones). Do this only if the package has no checked-in
     `moduleDeps.cpp` (see `Public.cmake` ~line 358). It is compiled into
     the **library**, not the extension.
   - Extension `_<name>.so` (always `.so`, also on macOS) from
     `pymodule_cppfiles` (`module.cpp` + `wrap*.cpp`, which use
     `TF_WRAP_MODULE`), compiled with the same `MFB_*` defines; link against
     the monolithic library dynamically.
   - macOS: extensions and the monolithic library resolve libpython at load
     time (`-undefined dynamic_lookup`, CMake's
     `PXR_PY_UNDEFINED_DYNAMIC_LOOKUP`); C++ executables link
     `current_py_cc_libs`. Verify no second libpython gets loaded.
4. **Monolithic shared library** in `//pxr` (e.g. `usd_ms`,
   `cc_shared_library` over the pxr libraries built so far). Check exported
   symbols and `install_name`/rpath (`@loader_path` on macOS, `$ORIGIN` on
   Linux, chosen by `select()`).
5. **Python package layout** for `bazel run`/`py_test`: a tree
   `pxr/__init__.py` (generated, `__all__ = [...module names]`, see
   `pxr_setup_python` in `Public.cmake` ~line 440), and `pxr/<Name>/` with
   the package's `__init__.py` (+ other `pymodule_files`) and `_<name>.so`.
   `pxr.Tf.PreparePythonModule()` imports `._<name>` from the package
   directory. Expose it as a `py_library` (e.g. `//pxr:python`) with the
   right `imports`, and add a `py_test` smoke test doing
   `from pxr import Tf, Gf, Vt, Work, Plug, Trace, Ts` with a few calls.

## Done criteria for Phase 1

- `bazel build //...` and `bazel test //...` pass on macOS arm64.
- `cmake2bazel.py --check` reports no drift for every generated BUILD.bazel.
- Python smoke test imports all `pxr/base` modules and exercises Tf, Gf, Vt.
- No hand-written lists in generated BUILD files.
- Commit on `bazel-build` with a descriptive message (end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push to
  `origin` only when asked. Update this file into `PHASE2.md` for the next
  handoff.

## Useful commands

```sh
bazel build //...                    # everything
bazel test //bazel/smoke/...         # smoke tests
python3 bazel/tools/cmake2bazel.py pxr/base/tf/CMakeLists.txt \
  --labels arch=//pxr/base/arch tf=//pxr/base/tf boost=//pxr/external/boost
```
