"""mx_add_library() from MaterialX's CMakeLists.txt."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:objc_library.bzl", "objc_library")

# add_compile_options() in CMakeLists.txt enables warnings; we silence them as
# for other vendored code.
_COPTS = ["-w"]

# add_definitions() for the shader generators USD uses.
_DEFINES = [
    "MATERIALX_BUILD_GEN_GLSL",
    "MATERIALX_BUILD_GEN_MSL",
]

_DL = select({
    "@platforms//os:macos": [],
    "//conditions:default": ["-ldl"],  # CMAKE_DL_LIBS
})

def mx_add_library(name, srcs, hdrs, deps, copts = [], local_defines = [], defines = [], linkopts = [], objc_srcs = None):
    """One MaterialX module (non-monolithic mx_add_library)."""
    module_deps = list(deps)
    if objc_srcs:
        # ADD_OBJECTIVE_C_CODE: the .m files are compiled as Objective-C++.
        objc_library(
            name = name + "_objc",
            non_arc_srcs = objc_srcs,
            hdrs = hdrs,
            copts = _COPTS + copts + ["-x", "objective-c++"] + ["-D" + d for d in _DEFINES + local_defines + defines],
            includes = ["source"],
            linkopts = linkopts,
            deps = deps,
            alwayslink = True,
            target_compatible_with = ["@platforms//os:macos"],
            visibility = ["//visibility:private"],
        )
        module_deps += select({
            "@platforms//os:macos": [":" + name + "_objc"],
            "//conditions:default": [],
        })
    cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        copts = _COPTS + copts,
        defines = defines,
        local_defines = _DEFINES + local_defines + ["MATERIALX_%s_EXPORTS" % name[len("MaterialX"):].upper()],
        includes = ["source"],
        linkopts = linkopts + _DL,
        deps = module_deps,
        alwayslink = True,
    )

