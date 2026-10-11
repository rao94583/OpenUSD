# Generated from pxr/**/CMakeLists.txt by bazel/tools/cmake2bazel.py; do not edit.
# Regenerate with: python3 bazel/tools/cmake2bazel.py --write
"""pxr libraries and Python modules in CMake build order."""

PXR_LIBRARIES = [
    "//pxr/external/boost",
    "//pxr/external/boost/python",
    "//pxr/base/arch",
    "//pxr/base/tf",
    "//pxr/base/gf",
    "//pxr/base/pegtl",
    "//pxr/base/js",
    "//pxr/base/trace",
    "//pxr/base/work",
    "//pxr/base/plug",
    "//pxr/base/vt",
    "//pxr/base/ts",
]

# One entry per _<library> Python extension module.
PXR_PYTHON_MODULES = [
    struct(library = "tf", module = "Tf", package = "//pxr/base/tf"),
    struct(library = "gf", module = "Gf", package = "//pxr/base/gf"),
    struct(library = "trace", module = "Trace", package = "//pxr/base/trace"),
    struct(library = "work", module = "Work", package = "//pxr/base/work"),
    struct(library = "plug", module = "Plug", package = "//pxr/base/plug"),
    struct(library = "vt", module = "Vt", package = "//pxr/base/vt"),
    struct(library = "ts", module = "Ts", package = "//pxr/base/ts"),
]
