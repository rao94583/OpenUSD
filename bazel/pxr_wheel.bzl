"""The pxr wheel: the install tree (//bazel/install) as a Python package.

The wheel holds the install tree under pxr/, with the Python package from
lib/python/pxr at its root:

  pxr/__init__.py, pxr/<Module>/...   from lib/python/pxr/
  pxr/lib/lib*.dylib                  from lib/ (.so on Linux)
  pxr/lib/usd/..., pxr/plugin/usd/... resources, from lib/usd and plugin/usd

Plug finds resources relative to libusd_ms exactly as in the install tree,
so nothing is rewritten. Extensions find the libraries through the rpaths in
//bazel:pxr_python.bzl. This differs from Pixar's usd-core wheel
(build_scripts/pypi), which moves resources to pxr/pluginfo and builds with
PXR_INSTALL_LOCATION to match, and relocates libraries with delocate or
auditwheel.
"""

def _wheel_tree_impl(ctx):
    prefix = ctx.attr.install_package + "/"
    outputs = {}
    for target in ctx.attr.srcs:
        info = target[DefaultInfo]
        files = depset(transitive = [info.files, info.default_runfiles.files])
        for f in files.to_list():
            if not f.short_path.startswith(prefix):
                continue  # e.g. bazel-out copies under _solib_*
            relative = f.short_path[len(prefix):]
            if relative.endswith(".params"):
                continue  # linker parameter files
            if relative.startswith("lib/python/"):
                dest = relative[len("lib/python/"):]
            else:
                dest = "pxr/" + relative
            if dest in outputs:
                continue
            out = ctx.actions.declare_file(dest)
            ctx.actions.symlink(output = out, target_file = f)
            outputs[dest] = out
    return [DefaultInfo(files = depset(outputs.values()))]

pxr_wheel_tree = rule(
    implementation = _wheel_tree_impl,
    doc = "Lays out the install tree files of srcs as the wheel's contents.",
    attrs = {
        "srcs": attr.label_list(doc = "Targets of the install tree; their " +
                                      "files and runfiles under " +
                                      "install_package are included."),
        "install_package": attr.string(default = "bazel/install"),
    },
)

def pxr_wheel_scripts(name, programs):
    """Python programs as wheel scripts, with pip's "#!python" shebang.

    CMake replaces /pxrpythonsubst with the configured interpreter; pip
    replaces "#!python" with the installing one.

    Args:
      name: prefix of the generated targets.
      programs: {source label: program name} (PXR_PYTHON_PROGRAMS).

    Returns:
      {script target: "scripts/<program>"}, for py_wheel's data_files.
    """
    data_files = {}
    for source, program in programs.items():
        target = "%s_%s" % (name, program)
        native.genrule(
            name = target,
            srcs = [source],
            outs = ["%s/%s" % (name, program)],
            cmd = "sed '1s|^#!/pxrpythonsubst|#!python|' $< > $@",
        )
        data_files[":" + target] = "scripts/" + program
    return data_files
