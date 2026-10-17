"""Counts the MDL materials authored in a tree of USD files.

    bazel run //app/vela/mdl_to_mtlx:census -- DIR [DIR...] [--inputs N]

Reads each layer without composing it, so remote references are not
fetched. Reports each MDL module (with its subIdentifier) by shader count
and file count, the inputs authored on its shaders, and any non-MDL shader
ids, so converters can be written for the modules that matter most.
"""

import argparse
import collections
import os
import sys

from pxr import Sdf

USD_EXTENSIONS = (".usd", ".usda", ".usdc")


def usd_files(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            for name in filenames:
                if name.lower().endswith(USD_EXTENSIONS):
                    yield os.path.join(dirpath, name)


def module_name(asset):
    """The module file name: remote and relative paths of the same module
    are counted together."""
    return asset.path.replace("\\", "/").rsplit("/", 1)[-1]


def shader_key(prim):
    attrs = prim.attributes
    source = attrs.get("info:mdl:sourceAsset")
    if source is not None and source.default:
        sub = attrs.get("info:mdl:sourceAsset:subIdentifier")
        sub = sub.default if sub is not None and sub.default else ""
        return "mdl", "%s:%s" % (module_name(source.default), sub)
    shader_id = attrs.get("info:id")
    if shader_id is not None and shader_id.default:
        return "id", shader_id.default
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("roots", nargs="+")
    parser.add_argument("--inputs", type=int, default=0,
                        help="list the N most authored inputs per module")
    args = parser.parse_args()

    shaders = collections.Counter()
    files_by_key = collections.defaultdict(set)
    inputs_by_key = collections.defaultdict(collections.Counter)
    failures = []
    count = 0

    for path in usd_files(args.roots):
        count += 1
        layer = Sdf.Layer.FindOrOpen(path)
        if not layer:
            failures.append(path)
            continue

        def visit(spec_path):
            if not spec_path.IsPrimPath():
                return
            prim = layer.GetPrimAtPath(spec_path)
            if prim is None or prim.typeName != "Shader":
                return
            key = shader_key(prim)
            if key is None:
                return
            shaders[key] += 1
            files_by_key[key].add(path)
            for attr in prim.attributes:
                if attr.name.startswith("inputs:"):
                    inputs_by_key[key][attr.name[len("inputs:"):]] += 1

        layer.Traverse(Sdf.Path.absoluteRootPath, visit)

    print("%d USD files, %d unreadable" % (count, len(failures)))
    for path in failures:
        print("  unreadable: %s" % path)
    print()
    print("%8s %6s  %s" % ("shaders", "files", "shader"))
    for key, n in shaders.most_common():
        kind, name = key
        print("%8d %6d  %s %s" % (n, len(files_by_key[key]), kind, name))
        if args.inputs:
            for name, m in inputs_by_key[key].most_common(args.inputs):
                print("%24d  %s" % (m, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
