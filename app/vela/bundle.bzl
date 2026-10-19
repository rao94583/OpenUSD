"""Relocatable runtime trees and the shared USD compilation interface."""
load("@rules_apple//apple:providers.bzl", "AppleBundleInfo")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _headers_impl(ctx):
    merged = cc_common.merge_cc_infos(cc_infos = [d[CcInfo] for d in ctx.attr.deps])
    return [CcInfo(compilation_context = merged.compilation_context)]

usd_headers = rule(
    implementation = _headers_impl,
    attrs = {"deps": attr.label_list(providers = [CcInfo])},
)

def _runtime_impl(ctx):
    runtime = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
    files = depset(transitive = [d[DefaultInfo].default_runfiles.files for d in ctx.attr.usd])
    entries = []
    inputs = []
    for f in files.to_list():
        marker = "bazel/install/"
        if f.short_path.startswith(marker):
            entries.append([f.path, "usd/" + f.short_path[len(marker):]])
            inputs.append(f)
    # Python packages from PyPI (PyObjC), as Runtime/site-packages.
    packages = depset(transitive = [d[DefaultInfo].default_runfiles.files for d in ctx.attr.site_packages])
    # Packages may share files (PyObjCTools/__init__.py): keep the first.
    copied = {}
    for f in packages.to_list():
        marker = "/site-packages/"
        at = f.path.find(marker)
        if at >= 0 and "/__pycache__/" not in f.path:
            destination = "site-packages/" + f.path[at + len(marker):]
            if destination in copied:
                continue
            copied[destination] = True
            entries.append([f.path, destination])
            inputs.append(f)
    root = runtime.interpreter.dirname.rsplit("/", 1)[0] + "/"
    for f in runtime.files.to_list() + [runtime.interpreter]:
        if f.path.startswith(root):
            entries.append([f.path, "python/" + f.path[len(root):]])
            inputs.append(f)
    entries.append([ctx.file.smoke.path, "bundle_smoke.py"])
    inputs.append(ctx.file.smoke)
    manifest = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(manifest, json.encode(entries))
    tree = ctx.actions.declare_directory("Runtime")
    ctx.actions.run(
        executable = ctx.executable._copy,
        arguments = [manifest.path, tree.path],
        inputs = depset([manifest] + inputs),
        outputs = [tree],
    )
    return [DefaultInfo(files = depset([tree]))]

vela_runtime = rule(
    implementation = _runtime_impl,
    attrs = {
        "usd": attr.label_list(),
        "site_packages": attr.label_list(),
        "smoke": attr.label(default = ":bundle_smoke.py", allow_single_file = True),
        "_copy": attr.label(default = ":copy_runtime", executable = True, cfg = "exec"),
    },
    toolchains = ["@rules_python//python:toolchain_type"],
)

# The executable target's $(location) is its launch wrapper, not its archive.
def _archive_impl(ctx):
    archive = ctx.actions.declare_file(ctx.label.name + ".zip")
    ctx.actions.symlink(output = archive, target_file = ctx.attr.app[AppleBundleInfo].archive)
    return [DefaultInfo(files = depset([archive]), runfiles = ctx.runfiles(files = [archive]))]

vela_archive = rule(
    implementation = _archive_impl,
    attrs = {"app": attr.label(providers = [AppleBundleInfo])},
)

# The Claude Code plugin with Vela's skills, as Resources/ClaudePlugin: the
# skill files under prefix, plugin.json, and the usd-python skill's schema
# reference, generated from this build's schema registry.
def _claude_plugin_impl(ctx):
    plugin_json = ctx.actions.declare_file(ctx.label.name + "_plugin.json")
    ctx.actions.write(plugin_json, json.encode({
        "name": "vela",
        "description": "Vela's skills for the Claude panel: USD Python, driving the app, plugins and image generation.",
    }))
    entries = [[plugin_json.path, ".claude-plugin/plugin.json"]]
    for f in ctx.files.srcs:
        at = f.short_path.find(ctx.attr.prefix + "/")
        if at < 0:
            fail("%s is not under %s" % (f.short_path, ctx.attr.prefix))
        entries.append([f.path, f.short_path[at + len(ctx.attr.prefix) + 1:]])
    manifest = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(manifest, json.encode(entries))
    tree = ctx.actions.declare_directory("ClaudePlugin")
    ctx.actions.run_shell(
        command = '"$1" "$2" "$3" && "$4" "$3/skills/usd-python/references/schemas"',
        arguments = [
            ctx.executable._copy.path, manifest.path, tree.path, ctx.executable.generator.path,
        ],
        inputs = [manifest, plugin_json] + ctx.files.srcs,
        tools = [ctx.attr._copy[DefaultInfo].files_to_run, ctx.attr.generator[DefaultInfo].files_to_run],
        outputs = [tree],
        mnemonic = "VelaClaudePlugin",
        progress_message = "Generating Vela's Claude skills",
    )
    return [DefaultInfo(files = depset([tree]))]

vela_claude_plugin = rule(
    implementation = _claude_plugin_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "prefix": attr.string(mandatory = True),
        # Runs pxr, so it is built for the target (this Mac), not again for
        # the exec configuration.
        "generator": attr.label(executable = True, cfg = "target"),
        "_copy": attr.label(default = ":copy_runtime", executable = True, cfg = "exec"),
    },
)
