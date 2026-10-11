"""pxr_library: the Bazel counterpart of pxr_library() in cmake/macros/Public.cmake.

Arguments mirror the CMake keywords (lower-cased) so that BUILD.bazel files
are a mechanical translation of CMakeLists.txt. LIBRARIES entries are Bazel
labels rather than CMake target names.

Every library is a static cc_library with alwayslink, because TfType and
TF_REGISTRY_FUNCTION registration rely on static initializers that a linker
would otherwise drop. All libraries are linked into one shared library
(//bazel/install:usd_ms), and Python extension modules and programs link
against it dynamically, so there is one copy of each registry.
"""

load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:objc_library.bzl", "objc_library")
load("@rules_python//python:py_binary.bzl", "py_binary")
load("//bazel:pxr_install.bzl", "pxr_resources")
load("//bazel:pxr_python.bzl", "pxr_module_deps_cpp")

# What programs need at runtime: the monolithic library and its resources.
_MONOLITHIC = "//bazel/install:usd_ms"
_RESOURCES = "//bazel/install:resources"

# Flags from cmake/defaults/gccclangshareddefaults.cmake and clangdefaults.cmake.
PXR_COPTS = [
    "-Wall",
    "-Wformat-security",
    "-Wmismatched-tags",
    "-Wno-deprecated",
    "-Wno-deprecated-declarations",
    "-Wno-unused-local-typedefs",
    "-pthread",
] + select({
    # -Wthread-safety comes from the apple_support toolchain, not CMake.
    "@platforms//os:macos": ["-ffp-contract=off", "-Wno-thread-safety-analysis"],
    "//conditions:default": [],
})

PXR_LINKOPTS = select({
    "@platforms//os:linux": ["-pthread", "-ldl", "-lm"],
    "//conditions:default": ["-pthread"],
})

def pxr_configure_file(name, src, out, substitutions):
    """configure_file() of a header: replaces @VAR@ placeholders in src."""
    expand_template(
        name = name,
        out = out,
        substitutions = substitutions,
        template = src,
    )

_PYTHON_DEPS = [
    "//pxr/external/boost/python",
    "@rules_python//python/cc:current_py_cc_headers",
]

def python_module_name(name):
    # Mirrors _get_python_module_name: tf -> Tf, usdGeom -> UsdGeom.
    return name[0].upper() + name[1:]

def _is_objc(src):
    return src.endswith(".mm") or src.endswith(".m")

def _without_objc(srcs):
    return [s for s in srcs if not _is_objc(s)]

_OS_CONSTRAINTS = {
    "linux": "@platforms//os:linux",
    "macos": "@platforms//os:macos",
}

_PER_OS_FIELDS = [
    "libraries",
    "public_headers",
    "private_headers",
    "cppfiles",
    "copts",
    "linkopts",
]

def _unique(values):
    return {v: None for v in values}.keys()

def _library_name(label):
    """Library name of a pxr label: //pxr/base/tf -> tf."""
    if ":" in label:
        return label.split(":")[-1]
    return label.split("/")[-1]

def _per_os_select(per_os, field, transform = None):
    """select() over per_os[os][field], or [] if no OS sets the field."""
    branches = {}
    for os, values in per_os.items():
        if os not in _OS_CONSTRAINTS:
            fail("per_os: unknown OS %r; expected one of %s" % (os, _OS_CONSTRAINTS.keys()))
        for key in values:
            if key not in _PER_OS_FIELDS:
                fail("per_os[%r]: unknown field %r; expected one of %s" % (os, key, _PER_OS_FIELDS))
        value = values.get(field, [])
        if transform:
            value = transform(value)
        if value:
            branches[_OS_CONSTRAINTS[os]] = value
    if not branches:
        return []
    branches["//conditions:default"] = []
    return select(branches)

def pxr_library(
        name,
        plugin = False,
        libraries = [],
        public_classes = [],
        python_public_classes = [],
        public_headers = [],
        python_public_headers = [],
        private_classes = [],
        python_private_classes = [],
        private_headers = [],
        python_private_headers = [],
        cppfiles = [],
        python_cppfiles = [],
        pymodule_cppfiles = [],
        pymodule_files = [],
        resource_files = [],
        doxygen_files = [],
        copts = [],
        linkopts = [],
        defines = [],
        local_defines = [],
        pymodule_local_defines = [],
        generated_headers = [],
        per_os = {},
        visibility = ["//visibility:public"]):
    """Declares a USD library.

    Args:
      name: library name, also PXR_PACKAGE (e.g. "tf").
      plugin: True for pxr_plugin(). Core plugins are linked into the
        monolithic library like other libraries (OBJECT_PLUGIN); only their
        resources go elsewhere (plugin/usd instead of lib/usd).
      libraries: labels of libraries this one links against.
      public_classes: names X contributing X.h (public) and X.cpp.
      python_public_classes: as public_classes, compiled because Python
        support is always enabled in this build.
      public_headers: additional public headers.
      python_public_headers: Python-only public headers.
      private_classes: names X contributing X.h (private) and X.cpp.
      python_private_classes: Python-only private classes.
      private_headers: additional private headers.
      python_private_headers: Python-only private headers.
      cppfiles: additional sources; .mm files are compiled as Objective-C++.
      python_cppfiles: Python-only sources compiled into the library.
      pymodule_cppfiles: sources of the _<name> Python extension module.
      pymodule_files: pure-Python files of the pxr.<Name> package.
      resource_files: files installed under lib/usd/<name>/resources.
      doxygen_files: accepted for parity with CMake; unused.
      copts: extra compiler flags.
      linkopts: extra linker flags.
      defines: public definitions (target_compile_definitions PUBLIC).
      local_defines: private definitions of the library.
      pymodule_local_defines: private definitions of the Python extension.
      generated_headers: labels of headers made by pxr_configure_file.
      per_os: platform-specific additions, the equivalent of CMake's
        if(APPLE)/if(WIN32) blocks. Maps "macos" or "linux" to a dict with
        any of the keys libraries, public_headers, private_headers, cppfiles,
        copts, linkopts.
      visibility: target visibility.
    """
    _ignore = doxygen_files  # Documentation is not built with Bazel.

    uses_python = bool(
        python_public_classes or python_public_headers or
        python_private_classes or python_private_headers or python_cppfiles,
    )

    all_public_classes = public_classes + python_public_classes
    all_private_classes = private_classes + python_private_classes

    # CMake copies private headers into the build tree's include directory
    # too, so other libraries and tests can include them. Match that by
    # exporting every header.
    hdrs = (
        [c + ".h" for c in all_public_classes + all_private_classes] +
        public_headers + python_public_headers +
        private_headers + python_private_headers
    )
    srcs = (
        [c + ".cpp" for c in all_public_classes + all_private_classes] +
        cppfiles + python_cppfiles
    )
    # CMake target sources are sets: a class may be listed twice (e.g. in
    # CMakeLists.txt and generatedSchema.classes.txt).
    hdrs = _unique(hdrs)
    srcs = _unique(srcs)
    pymodule_cppfiles = _unique(pymodule_cppfiles)
    objc_srcs = [s for s in srcs if _is_objc(s)]
    cc_srcs = _without_objc(srcs)

    # Platform-specific inputs, split the same way.
    hdrs = hdrs + generated_headers + _per_os_select(
        per_os,
        "public_headers",
    ) + _per_os_select(per_os, "private_headers")
    cc_srcs = cc_srcs + _per_os_select(
        per_os,
        "cppfiles",
        _without_objc,
    )
    os_objc_srcs = {
        os: [s for s in values.get("cppfiles", []) if _is_objc(s)]
        for os, values in per_os.items()
    }

    deps = (
        ["//pxr"] + libraries + (_PYTHON_DEPS if uses_python else []) +
        _per_os_select(per_os, "libraries")
    )
    copts = PXR_COPTS + copts + _per_os_select(per_os, "copts")
    linkopts = PXR_LINKOPTS + linkopts + _per_os_select(per_os, "linkopts")

    mfb_defines = [
        "MFB_PACKAGE_NAME=" + name,
        "MFB_ALT_PACKAGE_NAME=" + name,
        "MFB_PACKAGE_MODULE=" + python_module_name(name),
    ]
    library_defines = mfb_defines + [
        "PXR_BUILD_LOCATION=usd",
        "PXR_PLUGIN_BUILD_LOCATION=../plugin/usd",
        name.upper() + "_EXPORTS=1",
    ] + local_defines

    # The library registers its Python module and the libraries whose
    # modules it needs (Public.cmake, genModuleDepsCpp.cmake), unless the
    # package provides its own moduleDeps.cpp.
    if pymodule_cppfiles and "moduleDeps.cpp" not in python_cppfiles:
        pxr_module_deps_cpp(
            name = name + "_module_deps",
            out = "moduleDeps.cpp",
            library = name,
            module = python_module_name(name),
            reqs = [
                _library_name(lib)
                for lib in libraries
                if lib.startswith("//pxr/")
            ],
        )
        cc_srcs = cc_srcs + [":" + name + "_module_deps"]

    # Objective-C++ sources (cc_library does not accept .mm) go into a
    # companion objc_library. It only exists on macOS, the only OS where
    # pxr has Objective-C++ code.
    if objc_srcs or any(os_objc_srcs.values()):
        if objc_srcs or [os for os, v in os_objc_srcs.items() if v and os != "macos"]:
            fail("pxr_library(%s): Objective-C++ sources must be listed under per_os[\"macos\"]" % name)
        objc_name = name + "_objc"
        objc_library(
            name = objc_name,
            non_arc_srcs = os_objc_srcs["macos"],
            hdrs = hdrs,
            # objc_library has no local_defines; pass them as flags.
            copts = copts + ["-D" + d for d in defines + library_defines],
            deps = deps,
            alwayslink = True,
            target_compatible_with = ["@platforms//os:macos"],
            visibility = ["//visibility:private"],
        )
        deps = deps + select({
            "@platforms//os:macos": [":" + objc_name],
            "//conditions:default": [],
        })

    cc_library(
        name = name,
        srcs = cc_srcs,
        hdrs = hdrs,
        copts = copts,
        linkopts = linkopts,
        defines = defines,
        local_defines = library_defines,
        deps = deps,
        alwayslink = True,
        visibility = visibility,
    )

    # Objects of the _<name> Python extension module. //bazel/python links
    # them into _<name>.so against the monolithic library.
    if pymodule_cppfiles:
        cc_library(
            name = "_" + name,
            # Schema modules' module.cpp includes this from its directory.
            srcs = pymodule_cppfiles + native.glob(
                ["generatedSchema.module.h"],
                allow_empty = True,
            ),
            copts = copts,
            local_defines = mfb_defines + pymodule_local_defines,
            deps = [":" + name] + _PYTHON_DEPS,
            alwayslink = True,
            visibility = visibility,
        )
    if pymodule_files:
        native.filegroup(
            name = name + "_pyfiles",
            srcs = pymodule_files,
            visibility = visibility,
        )

    if resource_files:
        pxr_resources(
            name = name + "_resources",
            library = name,
            plugin = plugin,
            entries = resource_files,
            srcs = {e.split(":")[0]: None for e in resource_files}.keys(),
            visibility = visibility,
        )

def pxr_cpp_bin(name, libraries = []):
    """A C++ program, like pxr_cpp_bin() in Public.cmake: <name>.cpp.

    pxr libraries come from the monolithic library; the program links
    libpython itself, as CMake executables do.
    """
    cc_binary(
        name = name,
        srcs = [name + ".cpp"],
        copts = PXR_COPTS,
        linkopts = PXR_LINKOPTS,
        deps = ["//pxr"] + libraries + [
            "@rules_python//python/cc:current_py_cc_libs",
        ],
        dynamic_deps = [_MONOLITHIC],
        data = [_RESOURCES],
        visibility = ["//visibility:public"],
    )

def pxr_python_bin(name, python_file):
    """A Python program, like pxr_python_bin() in Public.cmake.

    Programs run with the Python packages CMake looks for (Jinja2, for the
    schema generators) importable, as they are in CMake's interpreter.
    """
    py_binary(
        name = name,
        srcs = [python_file],
        main = python_file,
        deps = [
            "//bazel/install:python",
            "//vendor/jinja2",
        ],
        visibility = ["//visibility:public"],
    )
