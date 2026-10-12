"""cmake_configure_file: CMake's configure_file() for vendored dependencies.

Vendored libraries generate config headers from templates (e.g.
ImathConfig.h.in). Keeping the upstream template and listing the values
CMake would use keeps upgrades mechanical.
"""

def _cmake_configure_file_impl(ctx):
    values = ctx.actions.declare_file(ctx.label.name + ".values.json")
    ctx.actions.write(values, json.encode(ctx.attr.values))
    args = ctx.actions.args()
    args.add(ctx.file.src)
    args.add(ctx.outputs.out)
    args.add(values)
    if ctx.attr.at_only:
        args.add("--at-only")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.src, values],
        outputs = [ctx.outputs.out],
        mnemonic = "CMakeConfigureFile",
        progress_message = "Configuring %{output}",
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

cmake_configure_file = rule(
    implementation = _cmake_configure_file_impl,
    doc = "Generates out from the CMake template src.",
    attrs = {
        "src": attr.label(allow_single_file = True, mandatory = True),
        "out": attr.output(mandatory = True),
        "values": attr.string_dict(
            doc = "CMake variables; undefined ones are false/empty.",
        ),
        "at_only": attr.bool(doc = "configure_file(... @ONLY)"),
        "_tool": attr.label(
            default = "//bazel/tools:cmake_configure_file",
            executable = True,
            cfg = "exec",
        ),
    },
)
