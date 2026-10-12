OpenUSD, built with Bazel
=========================

This repository is a fork of Pixar's
[OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD) that builds
**factory USD**, Pixar's sources with no changes to their behavior, with
[Bazel](https://bazel.build) instead of CMake. Everything Pixar's default
build produces is built and tested: the core libraries, Python bindings,
command-line tools, Hydra and the Storm renderer (on Metal), MaterialX
shading and usdview. The CMake build is still here and still works. The
Bazel build is generated from it and checked against it, so upstream
releases can be merged in.

The build is the foundation for a larger goal: a native macOS app that
replaces usdview. It will have a modern UI and a built-in Claude Code panel
for editing and querying scenes in plain English
([see below](#where-this-is-going)).

Contents
--------

- [Status](#status)
- [Where this is going](#where-this-is-going)
- [Quick start](#quick-start)
- [Everyday commands](#everyday-commands)
- [How the build works](#how-the-build-works)
- [Dependencies](#dependencies)
- [Repository layout](#repository-layout)
- [Staying in sync with upstream](#staying-in-sync-with-upstream)
- [Known issues and limitations](#known-issues-and-limitations)
- [Upstream OpenUSD](#upstream-openusd)

Status
------

| Area | State |
|---|---|
| Platform | macOS on Apple silicon (arm64). Linux is planned; the build files already separate per-OS settings, but Linux has not been built yet. Windows is not planned. |
| Core | `pxr/base`, `pxr/usd`, `usdValidation`, schema code generators |
| Python | Bindings for every module, using a hermetic Python 3.11 |
| Tools | usdcat, usdtree, usddiff, usdchecker, usdrecord, usdresolve, sdfdump, ... |
| Imaging | Hydra, Storm rendering through Metal, usdImaging, OpenSubdiv |
| MaterialX | MaterialX 1.39.5: usdMtlx, hdMtlx, MaterialX materials in Storm |
| usdview | Built and tested, with PySide6 6.11.2 |
| Tests | Pixar's CMake test suite, ported: about 1,650 tests. All tests that apply to macOS pass. Storm render tests compare images with OpenImageIO's `idiff`, as in Pixar's CI. |
| Packaging | A pip-installable wheel; fully offline builds |
| Not yet built | `exec` and the optional plugins Pixar's default build leaves off (OSL, Ptex, Embree, Alembic, Draco, OpenVDB, RenderMan) |
| CI | None yet |

Where this is going
-------------------

usdview is Pixar's reference viewer and inspector. It is powerful, but it is
a Python and Qt desktop tool with a UI to match. The plan is to build a
**native macOS replacement for usdview** on top of the libraries built
here, with these goals:

- **Everything usdview does.** Browse the prim hierarchy, inspect
  properties, metadata, composition and layer stacks, switch variants,
  scrub time, pick cameras and render with Storm.
- **A modern, native UI.** A Mac app that feels like one, rather than a
  port of usdview's Qt widgets.
- **A Claude Code panel.** Describe what you want in plain English, and
  Claude does it:
  - *Edits*, such as "make every light in this room 20% warmer" or "add a
    variant set for the chair's fabric".
  - *Queries*, such as "which prims have no material bound?" or "why is
    this mesh invisible in the render?".
- **Skills for Claude.** The panel will come with skills for two APIs:
  - **The USD Python API** (`pxr`), the same bindings this repository
    builds, for reading and editing the scene.
  - **A full UI manipulation API**, for driving the app itself: selecting
    prims, framing the camera, opening inspectors, changing viewport
    settings and so on. The user and Claude work on the same scene through
    the same interface.

A reproducible, fully source-built USD with Python bindings is the first
step toward that app. This repository provides it.

Quick start
-----------

### Prerequisites

- macOS on Apple silicon (developed on macOS 27 with Xcode 27; the build
  targets macOS 13 or later)
- Xcode. Bazel uses its compilers and the macOS SDK.
- [Bazelisk](https://github.com/bazelbuild/bazelisk), which installs the
  Bazel version in `.bazelversion` (9.3.0):

  ```sh
  brew install bazelisk
  ```

That is all. You don't need CMake, a system Python, Homebrew libraries or
`build_usd.py`. C++ dependencies are compiled from source in `vendor/`.
Python and the Bazel rule sets are downloaded by Bazel and pinned by
checksum.

### Build and test

```sh
git clone https://github.com/rao94583/OpenUSD.git
cd OpenUSD
git checkout bazel-build

bazel build //...     # everything: libraries, Python, tools, tests
bazel test //...      # the full test suite (about 8 minutes on an M-series Mac)
```

The first build compiles USD and all its dependencies, which takes a while.
Later builds are incremental.

### Try it

```sh
bazel run //pxr/usdImaging/bin/usdview -- $PWD/extras/usd/tutorials/convertingLayerFormats/Sphere.usda
```

`bazel run` runs programs from inside Bazel's output tree, so pass absolute
paths (`$PWD/...`) to files in the repository.

Everyday commands
-----------------

### Command-line tools

```sh
bazel run //pxr/usd/bin/usdcat -- $PWD/extras/usd/tutorials/convertingLayerFormats/Sphere.usda
bazel run //pxr/usd/bin/usdtree -- $PWD/extras/usd/tutorials/convertingLayerFormats/Sphere.usda
bazel run //pxr/usdValidation/bin/usdchecker -- /path/to/asset.usd
bazel run //pxr/usdImaging/bin/usdrecord -- /path/to/scene.usd /tmp/frame.png
```

To list all the tools:

```sh
bazel query 'kind("cc_binary|py_binary", //pxr/usd/bin/... + //pxr/usdImaging/bin/... + //pxr/usdValidation/bin/...)'
```

### Running tests

```sh
bazel test //...                                  # everything
bazel test //pxr/usd/usdGeom/...                  # one module
bazel test //pxr/imaging/hdSt:testHdStBasicDrawing_Adaptive_lighting_lv1_hull   # one test
cat bazel-testlogs/pxr/imaging/hdSt/<test>/test.log         # a test's output
```

Tests run exactly as CMake registers them: the same commands, the same test
environments, and the same output and image comparisons, through Pixar's
`cmake/macros/testWrapper.py`. The usdview tests open real windows, so run
them in a logged-in desktop session.

### Using USD from Python: the wheel

`//bazel/wheel` packages the Python package, the shared libraries, all
plugin resources and the Python tools into a standard wheel:

```sh
bazel build -c opt //bazel/wheel
# -> bazel-bin/bazel/wheel/openusd-0.27.2-cp311-cp311-macosx_13_0_arm64.whl  (~33 MB)
```

Build it with `-c opt` (optimized). Without it, the wheel is a larger,
slower debug-friendly build. The wheel is for CPython 3.11, the
interpreter the extensions are built against. Bazel's own 3.11 works:

```sh
PY=$(bazel info output_base)/external/rules_python++python+python_3_11_aarch64-apple-darwin/bin/python3
$PY -m venv ~/usd-venv
~/usd-venv/bin/pip install "bazel-bin/bazel/wheel/openusd-0.27.2-cp311-cp311-macosx_13_0_arm64.whl[usdview]"

~/usd-venv/bin/python -c "from pxr import Usd; print(Usd.GetVersion())"
~/usd-venv/bin/usdview scene.usda
```

The `[usdview]` extra installs PySide6 and PyOpenGL. Leave it off for a
library-only install. `bazel test //bazel/wheel:wheel_test` checks that the
wheel works outside Bazel; it runs Pixar's own install test for the
`usd-core` wheel.

The distribution is called `openusd` so it doesn't collide with Pixar's
`usd-core` on PyPI. That wheel is built differently and leaves out imaging.

### Offline builds

Bazel downloads its rule sets, the hermetic Python and usdview's PyPI
packages on first use. To build without network access, vendor them once
while online:

```sh
bazel vendor --config=offline //...   # fills bazel-vendor/ (~600 MB, git-ignored)
bazel build --config=offline //...    # never downloads anything
```

### Regenerating BUILD files

The `BUILD.bazel` files under `pxr/` are generated from the
`CMakeLists.txt` files. Never edit them by hand:

```sh
python3 bazel/tools/cmake2bazel.py --write   # regenerate after changing CMake files
python3 bazel/tools/cmake2bazel.py --check   # fail if they are out of date
```

How the build works
-------------------

### BUILD files are generated from CMake

`bazel/tools/cmake2bazel.py` reads the `CMakeLists.txt` files and writes
the matching Bazel targets. It is a small interpreter for the part of CMake
that OpenUSD's build uses. It follows `add_subdirectory()`, evaluates
`if()`, variables and user-defined functions, and understands Pixar's
macros (`pxr_library`, `pxr_plugin`, `pxr_python_bin`, `pxr_build_test`,
`pxr_register_test`, ...). Library sources, headers, Python files,
resources, tests and their test data all come from what CMake declares.

The generator runs once per target OS and merges the results. OS-specific
differences become `select()`s and `target_compatible_with`. It never looks
at the machine it runs on, so the output is the same everywhere. CMake
commands that would change the build but that it doesn't implement are
errors, not silently ignored. When Pixar changes a `CMakeLists.txt`,
regenerating picks the change up.

The hand-written part is small: the Starlark macros in `bazel/`
(`pxr_library.bzl`, `pxr_test.bzl`, `pxr_python.bzl`, `pxr_install.bzl`,
`pxr_wheel.bzl`), the project-wide definitions in `pxr/BUILD.bazel`, and the
BUILD files of the vendored dependencies.

### Dependencies are vendored as source

Every C++ dependency is checked into `vendor/<name>/` as source from its
official release, at the version in Pixar's `VERSIONS.md`. Each comes with
our own `BUILD.bazel` and a `README.md` that records the version, the
download URL, its SHA-256 and any local changes (usually none). Nothing is
fetched at build time except:

- Bazel rule sets and a hermetic Python 3.11, from the Bazel Central
  Registry (pinned in `MODULE.bazel.lock`)
- usdview's prebuilt Python packages (PySide6-Essentials, shiboken6,
  PyOpenGL) from PyPI, pinned by SHA-256 in
  `bazel/requirements_lock.txt`. Qt is too large to build from source here.

`bazel vendor` (above) removes even those downloads.

### Linking

Every module is compiled as a static library, and all of them are linked
into **one shared library, `libusd_ms`**, like CMake's monolithic build
(`PXR_BUILD_MONOLITHIC`). oneTBB, OpenSubdiv and MaterialX are separate
shared libraries next to it, as in a CMake build. Python extension modules,
tools and tests all link against these four libraries, so the whole process
shares one copy of every USD registry.

### The install tree

`//bazel/install` lays out its outputs like a CMake install:

```
lib/libusd_ms.dylib, libtbb.dylib, libopensubdiv.dylib, libMaterialX.dylib
lib/usd/<module>/resources/...     plugInfo.json, schemas, shaders, ...
plugin/usd/<plugin>/resources/...
lib/python/pxr/...                 the Python package
```

USD's plugin system finds resources relative to `libusd_ms`, just as in an
installed USD, so nothing in USD needs patching. The wheel is this same tree
under `pxr/`.

### Rendering on macOS

As in Pixar's default macOS build, OpenGL and Metal support are both
enabled, and Storm renders through **Metal** (`hgiMetal`). Storm image tests
compare their output against Pixar's baseline images with `idiff`, built
from a vendored OpenImageIO.

Dependencies
------------

All built from source in `vendor/` unless noted.

| Dependency | Version | Used for |
|---|---|---|
| oneTBB | 2021.12.0 | Threading (work, everywhere) |
| OpenSubdiv | 3.6.1 | Subdivision surfaces (pxOsd, Storm) |
| MaterialX | 1.39.5 | MaterialX shading (usdMtlx, hdMtlx, Storm) |
| OpenImageIO | 3.1.8.0 | `idiff` for image comparison tests |
| OpenColorIO, OpenEXR, Imath, libpng, libjpeg-turbo, libtiff, zlib, libdeflate, expat, yaml-cpp, pystring, minizip-ng, fmt, robin-map | as pinned by OpenImageIO / `VERSIONS.md` | OpenImageIO's dependencies |
| Jinja2, MarkupSafe | 3.1.6, 3.0.3 | Schema code generators (usdGenSchema) |
| Python | 3.11 (hermetic, from Bazel's rules_python) | Bindings, tools, tests |
| PySide6-Essentials, shiboken6, PyOpenGL | 6.11.2, 6.11.2, 3.1.5 (PyPI, hash-pinned) | usdview |

Repository layout
-----------------

| Path | What it is |
|---|---|
| `pxr/` | Pixar's USD sources (unchanged), with generated `BUILD.bazel` files |
| `bazel/` | Starlark macros, the install tree, the wheel, smoke tests, handoff notes |
| `bazel/tools/cmake2bazel.py` | The CMake-to-Bazel generator |
| `bazel/tools/pxr_test_runner.py` | Runs a ported CMake test through `testWrapper.py` |
| `bazel/install/` | The install tree (`libusd_ms` and friends, resources, Python package) |
| `bazel/wheel/` | The wheel and its test |
| `bazel/PHASE6.md` | Detailed notes on the port: decisions, gotchas, next steps |
| `vendor/` | Third-party sources, each with its BUILD file and README |
| `MODULE.bazel`, `.bazelrc`, `.bazelversion` | Bazel configuration |
| `CMakeLists.txt`, `cmake/`, `build_scripts/` | Pixar's CMake build, unchanged |

Staying in sync with upstream
-----------------------------

The fork tracks Pixar's repository (`upstream`). Because Pixar's sources
are unchanged and the BUILD files are generated, an upstream merge is
usually:

```sh
git fetch upstream
git merge upstream/release                   # or a release tag
python3 bazel/tools/cmake2bazel.py --write   # pick up CMake changes
bazel test //...
```

If the generator reports an unsupported CMake command, it needs a small
extension. If Pixar bumps a dependency version in `VERSIONS.md`, update the
matching directory in `vendor/` (each README explains how).

Known issues and limitations
----------------------------

- **macOS arm64 only** for now. The Linux settings are written but untested.
- **No CI yet.** `bazel test //...` is run by hand.
- **One test is excluded:** `testUsdImagingGLBasisCurvesWithScale` differs
  from its baseline on 0.5% of pixels at curve edges with Metal (the limit
  is 0.3%). Not yet diagnosed.
- **usdview tests need focus.** `testUsdviewNavigationKeys` fails while
  another app is in front, because its window can't get keyboard focus.
- **`usdBakeMaterialX`** writes the baked MaterialX file, then fails an
  assertion: MaterialX 1.39.5 no longer adds `_baked` to material names, but
  the script expects it to. This is an upstream incompatibility.
- **Not yet built:** `exec`, and the optional plugins listed under
  [Status](#status).
- The wheel's C++ tools (usdcat, usdtree, usdchecker, sdfdump, sdffilter)
  are not included; its Python tools are.

Upstream OpenUSD
----------------

Universal Scene Description (USD) is Pixar's system for authoring, reading
and streaming time-sampled scene description for interchange between
graphics applications.

- Website and documentation: [openusd.org](https://openusd.org)
- API documentation: [openusd.org/release/api](https://openusd.org/release/api/index.html)
- Pixar's repository and README (including the CMake/`build_usd.py` build
  for Linux, macOS, Windows, iOS, visionOS and WebAssembly):
  [PixarAnimationStudios/OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD)
- CMake build options: [BUILDING.md](BUILDING.md)
- Tested third-party versions: [VERSIONS.md](VERSIONS.md)

USD is licensed under the [Tomorrow Open Source Technology License](LICENSE.txt).
The vendored dependencies keep their own licenses; see each
`vendor/<name>/` directory.
