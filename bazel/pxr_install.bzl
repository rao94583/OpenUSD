"""Resources and the install tree.

//bazel/install mirrors CMake's install layout, so that Plug finds resources
relative to the monolithic library exactly as in an installed build:

  lib/libusd_ms.dylib              all pxr libraries (.so on Linux)
  lib/usd/plugInfo.json            {"Includes": ["*/resources/"]}
  lib/usd/<lib>/resources/...      resources of each library
  plugin/usd/plugInfo.json
  plugin/usd/<plugin>/resources/...
  lib/python/pxr/...               the pxr Python package

The counterpart of _install_resource_files, _plugInfo_subst and
pxr_setup_plugins in cmake/macros.
"""

PxrResourcesInfo = provider(
    doc = "Resource files of one pxr library.",
    fields = {
        "library": "library name",
        "plugin": "True for pxr_plugin() libraries (installed under plugin/usd)",
        "files": "list of struct(src = File, dest = path under resources/, " +
                 "subst = whether to substitute plugInfo.json placeholders)",
    },
)

def _resources_impl(ctx):
    package = ctx.label.package
    by_path = {}
    for f in ctx.files.srcs:
        by_path[f.short_path[len(package) + 1:]] = f

    files = []
    for entry in ctx.attr.entries:
        # <src>, <src>:<dest> or <src>:no_subst, as in _install_resource_files.
        parts = entry.split(":")
        if len(parts) > 2:
            fail("cannot parse resource path %r" % entry)
        src, dest, no_subst = parts[0], parts[0], False
        if len(parts) == 2:
            if parts[1] == "no_subst":
                no_subst = True
            else:
                dest = parts[1]
        if src not in by_path:
            fail("resource %r is not in srcs" % src)
        files.append(struct(
            src = by_path[src],
            dest = dest,
            subst = dest.split("/")[-1] == "plugInfo.json" and not no_subst,
        ))
    return [PxrResourcesInfo(
        library = ctx.attr.library,
        plugin = ctx.attr.plugin,
        files = files,
    )]

pxr_resources = rule(
    implementation = _resources_impl,
    doc = "Collects a library's RESOURCE_FILES for pxr_install_resources.",
    attrs = {
        "library": attr.string(mandatory = True),
        "plugin": attr.bool(),
        "entries": attr.string_list(doc = "RESOURCE_FILES entries, verbatim."),
        "srcs": attr.label_list(allow_files = True),
    },
)

# Top-level plugInfo.json files written by pxr_setup_plugins.
_LIB_PLUGINFO = """\
{
    "Includes": [
        "*/resources/"
    ]
}
"""
_PLUGIN_PLUGINFO = """\
{
    "Includes": [ "*/resources/" ]
}
"""

def _install_resources_impl(ctx):
    outputs = []
    for path, content in [
        ("lib/usd/plugInfo.json", _LIB_PLUGINFO),
        ("plugin/usd/plugInfo.json", _PLUGIN_PLUGINFO),
    ]:
        out = ctx.actions.declare_file(path)
        ctx.actions.write(out, content)
        outputs.append(out)

    for target in ctx.attr.resources:
        info = target[PxrResourcesInfo]
        prefix = "%s/%s/resources/" % (
            "plugin/usd" if info.plugin else "lib/usd",
            info.library,
        )
        for f in info.files:
            out = ctx.actions.declare_file(prefix + f.dest)
            if f.subst:
                # Every pxr library is part of the monolithic library, which
                # is not separately loadable, so LibraryPath stays empty.
                ctx.actions.expand_template(
                    template = f.src,
                    output = out,
                    substitutions = {
                        "@PLUG_INFO_LIBRARY_PATH@": "",
                        "@PLUG_INFO_PLUGIN_NAME@": "pxr." + info.library,
                        "@PLUG_INFO_RESOURCE_PATH@": "resources",
                        "@PLUG_INFO_ROOT@": "..",
                    },
                )
            else:
                # Copied rather than symlinked: Plug resolves paths relative
                # to the plugInfo.json it reads.
                ctx.actions.run_shell(
                    inputs = [f.src],
                    outputs = [out],
                    command = 'cp "$1" "$2"',
                    arguments = [f.src.path, out.path],
                    mnemonic = "PxrInstallResource",
                    progress_message = "Installing %{output}",
                )
            outputs.append(out)
    return [DefaultInfo(
        files = depset(outputs),
        runfiles = ctx.runfiles(files = outputs),
    )]

pxr_install_resources = rule(
    implementation = _install_resources_impl,
    doc = "Places resources under lib/usd and plugin/usd of this package.",
    attrs = {
        "resources": attr.label_list(providers = [PxrResourcesInfo]),
    },
)
