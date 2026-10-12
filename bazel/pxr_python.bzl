"""Python support: moduleDeps.cpp generation, extension modules, pxr package.

The counterpart of the Python parts of cmake/macros/Public.cmake and
Private.cmake (_pxr_python_module, pxr_setup_python).
"""

load("@rules_cc//cc:cc_shared_library.bzl", "cc_shared_library")
load("@rules_python//python:py_library.bzl", "py_library")

# Shared libraries that use Python resolve its symbols from the interpreter
# at load time (PXR_PY_UNDEFINED_DYNAMIC_LOOKUP), so only one libpython is
# ever loaded. Linux allows undefined symbols in shared libraries by default.
PXR_PY_SHARED_LINKOPTS = select({
    "@platforms//os:macos": ["-Wl,-undefined,dynamic_lookup"],
    "//conditions:default": [],
})

# Rpaths for the relocatable layouts, beyond the bazel-out ones Bazel adds:
# the install tree (lib/python/pxr/<Module>/_<lib>.so -> lib/) and the wheel
# (pxr/<Module>/_<lib>.so -> pxr/lib/), and between the shared libraries in
# lib/. The counterpart of _pxr_install_rpath in cmake/macros/Private.cmake.
# ($$ escapes make-variable expansion.)
PXR_EXTENSION_RPATHS = select({
    "@platforms//os:macos": [
        "-Wl,-rpath,@loader_path/../../..",
        "-Wl,-rpath,@loader_path/../lib",
    ],
    "//conditions:default": [
        "-Wl,-rpath,$$ORIGIN/../../..",
        "-Wl,-rpath,$$ORIGIN/../lib",
    ],
})
PXR_LIBRARY_RPATHS = select({
    "@platforms//os:macos": ["-Wl,-rpath,@loader_path"],
    "//conditions:default": ["-Wl,-rpath,$$ORIGIN"],
})

def _module_deps_cpp_impl(ctx):
    reqs = ",\n".join(['\t\tTfToken("%s")' % r for r in ctx.attr.reqs])
    ctx.actions.expand_template(
        template = ctx.file._template,
        output = ctx.outputs.out,
        substitutions = {
            "${reqLibs}": reqs,
            "${libraryName}": ctx.attr.library,
            "${moduleName}": ctx.attr.module,
            # string(CONFIGURE) unescapes these.
            "\\;": ";",
        },
    )

pxr_module_deps_cpp = rule(
    implementation = _module_deps_cpp_impl,
    doc = "Generates moduleDeps.cpp like cmake/macros/genModuleDepsCpp.cmake.",
    attrs = {
        "out": attr.output(mandatory = True),
        "library": attr.string(mandatory = True),
        "module": attr.string(mandatory = True),
        "reqs": attr.string_list(doc = "Names of the pxr libraries it links."),
        "_template": attr.label(
            default = "//cmake/macros:moduleDeps.cpp.in",
            allow_single_file = True,
        ),
    },
)

def _python_tree_impl(ctx):
    outputs = []
    root = ctx.attr.prefix + "/pxr/"
    init = ctx.actions.declare_file(root + "__init__.py")
    ctx.actions.write(init, "__all__ = [%s]\n" % ", ".join(
        ["'%s'" % m for m in ctx.attr.all_modules],
    ))
    outputs.append(init)
    for target, module in ctx.attr.pyfiles.items():
        package = target.label.package
        for f in target.files.to_list():
            if not f.short_path.startswith(package + "/"):
                fail("%s: %s is outside its package" % (target.label, f.path))
            relative = f.short_path[len(package) + 1:]
            out = ctx.actions.declare_file(root + module + "/" + relative)
            ctx.actions.symlink(output = out, target_file = f)
            outputs.append(out)
    return [DefaultInfo(files = depset(outputs))]

_pxr_python_tree = rule(
    implementation = _python_tree_impl,
    doc = "Lays out pxr/__init__.py and pxr/<Module>/ Python files.",
    attrs = {
        "prefix": attr.string(mandatory = True),
        "all_modules": attr.string_list(
            doc = "Modules listed in pxr.__all__, as pxr_setup_python does.",
        ),
        "pyfiles": attr.label_keyed_string_dict(
            doc = "<library>_pyfiles filegroup -> Python module name.",
        ),
    },
)

def pxr_python_package(
        name,
        prefix,
        modules,
        dynamic_deps,
        runtime = [],
        visibility = None):
    """The importable pxr package, at <prefix>/pxr in this package.

    Builds <prefix>/pxr/<Module>/_<library>.so for every module, linked
    against the shared libraries in dynamic_deps, next to the module's
    Python files.

    Args:
      name: name of the resulting py_library.
      prefix: directory of the pxr package, relative to this package.
      modules: structs with library, module and package fields
        (PXR_PYTHON_MODULES from pxr_libraries.bzl).
      dynamic_deps: cc_shared_library targets the extensions link against
        (the monolithic library, oneTBB, OpenSubdiv).
      runtime: other files the modules need at runtime (resources).
      visibility: visibility of the py_library.
    """
    extensions = []
    for m in modules:
        if m.target_compatible_with:
            # Needs select()s in the tree below; no such module exists yet.
            fail(("pxr_python_package: %s exists on some OSes only, which " +
                  "is not supported yet") % m.library)
        extension = "%s/pxr/%s/_%s" % (prefix, m.module, m.library)
        cc_shared_library(
            name = extension,
            shared_lib_name = extension + ".so",
            deps = ["%s:_%s" % (m.package, m.library)],
            dynamic_deps = dynamic_deps,
            user_link_flags = PXR_PY_SHARED_LINKOPTS + PXR_EXTENSION_RPATHS,
        )
        extensions.append(":" + extension)

    _pxr_python_tree(
        name = name + "_tree",
        prefix = prefix,
        all_modules = [m.module for m in modules],
        pyfiles = {
            "%s:%s_pyfiles" % (m.package, m.library): m.module
            for m in modules
        },
    )

    py_library(
        name = name,
        data = [":" + name + "_tree"] + dynamic_deps + extensions + runtime,
        imports = [prefix],
        visibility = visibility,
    )
