"""Tests: the counterpart of the test macros in cmake/macros/Public.cmake.

CMake installs test artifacts under <prefix>/tests and runs each test with
cmake/macros/testWrapper.py. Here every package lays out the same tree in its
own output directory (<package>/tests/...):

  tests/<program>                     pxr_build_test, add_executable
  tests/lib/lib<plugin>.dylib         pxr_build_test_plugin, add_library
  tests/<script>                      pxr_test_scripts
  tests/<prefix>/lib/<plugin>/Resources/plugInfo.json
  tests/<prefix>/lib/python/<module>/ pxr_create_test_module
  tests/ctest/<testenv>/              pxr_register_test(TESTENV)

and every pxr_register_test becomes a pxr_test: a py_test running
bazel/tools/pxr_test_runner.py, which assembles <prefix> (lib, plugin, bin,
and the needed tests trees) in the test's temporary directory and runs
testWrapper.py with the arguments pxr_register_test would pass.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_shared_library.bzl", "cc_shared_library")
load("@rules_python//python:py_test.bzl", "py_test")
load("//bazel:pxr_library.bzl", "PXR_COPTS", "PXR_DYNAMIC_DEPS", "PXR_LINKOPTS")
load("//bazel:pxr_python.bzl", "PXR_PY_SHARED_LINKOPTS")

_SHARED_SUFFIX = select({
    "@platforms//os:macos": ".dylib",
    "//conditions:default": ".so",
})

def _files_next_to(name, srcs):
    """A library declaring the files next to srcs, which test sources include
    by relative path ("x.h", even "module_tail.cpp"), as textual headers."""
    dirs = {s.rpartition("/")[0]: None for s in srcs}
    patterns = []
    for d in dirs:
        prefix = d + "/" if d else ""
        patterns += [prefix + "*." + ext for ext in ("h", "hpp", "inl", "cpp")]
    cc_library(
        name = name,
        textual_hdrs = native.glob(patterns, exclude = srcs, allow_empty = True),
        testonly = True,
    )
    return ":" + name

def pxr_test_program(name, srcs, libraries = []):
    """A test program installed as tests/<name> (pxr_build_test)."""
    cc_binary(
        name = "tests/" + name,
        srcs = srcs,
        copts = PXR_COPTS,
        linkopts = PXR_LINKOPTS,
        deps = ["//pxr", _files_next_to("_testfiles_" + name, srcs)] +
               libraries + [
            # Executables link libpython, as in CMake.
            "@rules_python//python/cc:current_py_cc_libs",
        ],
        dynamic_deps = PXR_DYNAMIC_DEPS,
        testonly = True,
    )

def pxr_test_library(name, srcs, libraries = [], prefix = "lib", suffix = None):
    """A shared library installed as tests/lib/<prefix><name><suffix>.

    Test plugins (pxr_build_test_plugin) and boost::python's test extension
    modules (add_library(SHARED) with PREFIX/SUFFIX).
    """
    cc_library(
        name = "_test_" + name,
        srcs = srcs,
        copts = PXR_COPTS,
        deps = ["//pxr", _files_next_to("_testfiles_" + name, srcs)] +
               libraries,
        alwayslink = True,
        testonly = True,
    )
    cc_shared_library(
        name = "_testlib_" + name,
        shared_lib_name = "tests/lib/" + prefix + name +
                          (suffix if suffix != None else _SHARED_SUFFIX),
        deps = [":_test_" + name],
        dynamic_deps = PXR_DYNAMIC_DEPS,
        user_link_flags = PXR_PY_SHARED_LINKOPTS,
        testonly = True,
    )

def _test_files_impl(ctx):
    package = ctx.label.package

    def source(path):
        for f in ctx.files.srcs:
            if f.short_path == package + "/" + path:
                return f
        fail("%s is not in srcs" % path)

    outputs = []

    def copy(src, dest):
        out = ctx.actions.declare_file("tests/" + dest)
        ctx.actions.run_shell(
            inputs = [src],
            outputs = [out],
            command = 'cp "$1" "$2"',
            arguments = [src.path, out.path],
            mnemonic = "PxrInstallTestFile",
            progress_message = "Installing %{output}",
        )
        outputs.append(out)

    for dest, src in ctx.attr.files.items():
        copy(source(src), dest)

    for dest, src_dir in ctx.attr.testenvs.items():
        prefix = package + "/" + src_dir + "/"
        found = False
        for f in ctx.files.srcs:
            if f.short_path.startswith(prefix):
                copy(f, "ctest/%s/%s" % (dest, f.short_path[len(prefix):]))
                found = True
        if not found:
            fail("testenv %s is empty or missing" % src_dir)

    # _install_test_resource_files: substitute test plugInfo.json files.
    for dest, src in ctx.attr.plug_infos.items():
        out = ctx.actions.declare_file("tests/" + dest)
        library_path = ctx.attr.library_paths[dest] + ctx.attr.shared_suffix
        ctx.actions.expand_template(
            template = source(src),
            output = out,
            substitutions = {
                "@TEST_PLUG_INFO_LIBRARY_PATH@": library_path,
                "@TEST_PLUG_INFO_RESOURCE_PATH@": "Resources",
                "@TEST_PLUG_INFO_ROOT@": "..",
                "@PLUG_INFO_LIBRARY_PATH@": library_path,
                "@PLUG_INFO_RESOURCE_PATH@": "Resources",
                "@PLUG_INFO_ROOT@": "..",
            },
        )
        outputs.append(out)

    return [DefaultInfo(files = depset(outputs))]

_test_files = rule(
    implementation = _test_files_impl,
    attrs = {
        "files": attr.string_dict(doc = "dest under tests/ -> source"),
        "testenvs": attr.string_dict(
            doc = "dest under tests/ctest -> source directory",
        ),
        "plug_infos": attr.string_dict(
            doc = "dest under tests/ -> source plugInfo.json template",
        ),
        "library_paths": attr.string_dict(
            doc = "plug_infos dest -> LibraryPath without the suffix",
        ),
        "shared_suffix": attr.string(),
        "srcs": attr.label_list(allow_files = True),
    },
)

def pxr_test_files(name, files = {}, testenvs = {}, plug_infos = {}, library_paths = {}):
    """Scripts, test modules, plugInfo files and testenvs under tests/."""
    srcs = list(files.values()) + list(plug_infos.values())
    for src_dir in testenvs.values():
        srcs += native.glob([src_dir + "/**"])
    _test_files(
        name = name,
        files = files,
        testenvs = testenvs,
        plug_infos = plug_infos,
        library_paths = library_paths,
        shared_suffix = _SHARED_SUFFIX,
        srcs = {s: None for s in srcs}.keys(),
        testonly = True,
    )

def pxr_test_tree(name, srcs):
    """Everything this package installs under tests/."""
    native.filegroup(
        name = name,
        srcs = srcs,
        testonly = True,
        visibility = ["//visibility:public"],
    )

def _test_spec_impl(ctx):
    trees = {}
    for target in ctx.attr.test_trees:
        trees[target.label.package + "/tests"] = None
    spec = {
        "name": ctx.attr.test_name,
        "arguments": json.decode(ctx.attr.arguments),
        "tests_dirs": trees.keys(),
        "tools": {
            name: target[DefaultInfo].files_to_run.executable.short_path
            for target, name in ctx.attr.tools.items()
        },
        "python_tools": {
            name: target.files.to_list()[0].short_path
            for target, name in ctx.attr.python_tools.items()
        },
    }
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(spec))
    return [DefaultInfo(files = depset([out]))]

_test_spec = rule(
    implementation = _test_spec_impl,
    attrs = {
        "test_name": attr.string(),
        "arguments": attr.string(doc = "pxr_register_test arguments as JSON"),
        "test_trees": attr.label_list(),
        "tools": attr.label_keyed_string_dict(cfg = "target"),
        "python_tools": attr.label_keyed_string_dict(allow_files = True),
    },
)

def pxr_test(
        name,
        arguments,
        test_trees,
        tools = {},
        python_tools = {},
        tags = [],
        size = "medium",
        target_compatible_with = []):
    """A pxr_register_test.

    Args:
      name: test name.
      arguments: pxr_register_test keyword arguments ({"COMMAND": "...",
        "PYTHON": True, "ENV": [...], ...}), with <prefix> for
        ${CMAKE_INSTALL_PREFIX} and <python> for ${PYTHON_EXECUTABLE}.
      test_trees: test_tree targets whose tests/ directories make up
        <prefix>/tests.
      tools: C++ programs available as <prefix>/bin/<name>.
      python_tools: Python programs' sources, installed as
        <prefix>/bin/<name> with their shebang rewritten, as CMake does.
      tags: test tags.
      size: test size.
      target_compatible_with: for tests CMake registers on some OSes only.
    """
    _test_spec(
        name = name + "_spec",
        test_name = name,
        arguments = json.encode(arguments),
        test_trees = test_trees,
        tools = tools,
        python_tools = python_tools,
        testonly = True,
        target_compatible_with = target_compatible_with,
    )
    py_test(
        name = name,
        srcs = ["//bazel/tools:pxr_test_runner.py"],
        main = "//bazel/tools:pxr_test_runner.py",
        args = ["$(rootpath :%s_spec)" % name],
        data = [
            ":%s_spec" % name,
            "//cmake/macros:testWrapper.py",
            "//bazel/install:python",
        ] + test_trees + tools.keys() + python_tools.keys(),
        deps = ["//vendor/jinja2"],
        tags = tags,
        size = size,
        target_compatible_with = target_compatible_with,
    )
