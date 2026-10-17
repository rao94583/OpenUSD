"""Adds MaterialX versions of NVIDIA Omniverse MDL materials to USD scenes.

    bazel run //app/vela/mdl_to_mtlx -- PATH [PATH...] [options]

For each root layer (a file, or each USD file in a directory that no other
file in it references), writes NAME_mtlx.usda beside it. That layer
sublayers the original and gives each MDL material an OpenPBR network in the
"mtlx" render context, which Storm (and so Vela) renders; the MDL network is
kept for RTX. Materials changed by variant sets in the stage are converted
in each variant, so variant switching works. Remote textures are downloaded
to a mdl_textures directory beside the output.
"""

import argparse
import collections
import concurrent.futures
import glob
import os
import sys
import urllib.parse
import urllib.request

from pxr import Gf, Pcp, Sdf, Usd, UsdGeom, UsdShade

import graph
import omni

USD_EXTENSIONS = (".usd", ".usda", ".usdc")
SUFFIX = "_mtlx"

# Root layer metadata copied to the output, which becomes the root.
ROOT_METADATA = ("defaultPrim", "upAxis", "metersPerUnit", "startTimeCode",
                 "endTimeCode", "timeCodesPerSecond", "framesPerSecond",
                 "customLayerData")


def _is_url(path):
    return path.startswith(("http://", "https://"))


def _python(value):
    if isinstance(value, (Gf.Vec2f, Gf.Vec3f, Gf.Vec4f, Gf.Vec2d, Gf.Vec3d,
                          Gf.Vec4d)):
        return tuple(float(x) for x in value)
    if isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Sdf.AssetPath):
        return value
    return None


UDIM = "<UDIM>"


def _udim(attr, path):
    """The absolute UDIM template for path, anchored to the layer that
    authored it, if a tile exists. Ar does not resolve UDIM templates."""
    for spec in attr.GetPropertyStack(Usd.TimeCode.Default()):
        if spec.default is None:
            continue
        # A path without "./" is a search path, which Ar's default resolver
        # also tries beside the layer.
        for template in (spec.layer.ComputeAbsolutePath(path),
                         os.path.join(os.path.dirname(spec.layer.realPath),
                                      path)):
            pattern = glob.escape(template).replace(glob.escape(UDIM),
                                                    "1[0-9][0-9][0-9]")
            if glob.glob(pattern):
                return os.path.normpath(template)
        return None
    return None


def _tiles(template):
    """The UDIM tile numbers present for a template."""
    pattern = glob.escape(template).replace(glob.escape(UDIM),
                                            "1[0-9][0-9][0-9]")
    prefix, suffix = template.split(UDIM, 1)
    return sorted(int(f[len(prefix):len(f) - len(suffix)])
                  for f in glob.glob(pattern))


class Textures(object):
    """Maps authored texture paths to paths written in the output layer,
    downloading remote textures into a local directory."""

    def __init__(self, output_dir, texture_dir, download=True):
        self.output_dir = output_dir
        self.texture_dir = texture_dir
        self.download = download
        self.urls = {}         # url -> local file
        self.missing = collections.Counter()

    def _local(self, url):
        parts = urllib.parse.urlsplit(url)
        relative = urllib.parse.unquote(parts.path).lstrip("/")
        local = os.path.join(self.texture_dir, parts.netloc, relative)
        self.urls[url] = local
        return local

    def resolve(self, attr, module):
        value = attr.Get()
        if not isinstance(value, Sdf.AssetPath) or not value.path:
            return None
        path = value.path
        if value.resolvedPath and os.path.exists(value.resolvedPath):
            local = value.resolvedPath
        elif UDIM in path and not _is_url(path) and _udim(attr, path):
            local = _udim(attr, path)
        elif _is_url(path):
            local = self._local(path)
        elif _is_url(module):
            # MDL resolves relative textures against the module.
            local = self._local(urllib.parse.urljoin(module, path))
        else:
            self.missing[path] += 1
            return None
        color_space = attr.GetColorSpace() or None
        relative = os.path.relpath(local, self.output_dir).replace(os.sep, "/")
        if not relative.startswith("../"):
            # Anchored: "dir/file" would be a search path.
            relative = "./" + relative
        return omni.Texture(relative, color_space,
                            _tiles(local) if UDIM in local else None)

    def material_x_color_space(self, path, color_space):
        """The MaterialX color space for a texture written as path, from
        its USD colorSpace: Storm reads MaterialX textures raw and leaves
        the transform to MaterialX, which does not know "auto" or "sRGB".
        "auto" is sRGB for 8-bit images and linear otherwise, as in Hydra."""
        if color_space == "raw":
            return "raw"
        if color_space and color_space.lower() == "srgb":
            return "srgb_texture"
        if color_space and color_space != "auto":
            return color_space
        ext = os.path.splitext(path)[1].lower()
        if ext in (".exr", ".hdr"):
            return "lin_rec709"
        if ext == ".png":
            local = os.path.join(self.output_dir, path)
            if UDIM in local:
                tiles = glob.glob(glob.escape(local).replace(
                    glob.escape(UDIM), "1[0-9][0-9][0-9]"))
                local = tiles[0] if tiles else local
            try:
                with open(local, "rb") as f:
                    header = f.read(25)
                # The IHDR chunk's bit depth.
                if len(header) == 25 and header[24] > 8:
                    return "lin_rec709"
            except OSError:
                pass
        return "srgb_texture"

    def fetch(self, log):
        """Downloads textures not already present. Returns the failures."""
        todo = [(u, f) for u, f in sorted(self.urls.items())
                if not os.path.exists(f)]
        if not todo or not self.download:
            return [u for u, _ in todo]
        log("downloading %d textures to %s" % (len(todo), self.texture_dir))

        def get(item):
            url, local = item
            os.makedirs(os.path.dirname(local), exist_ok=True)
            request = urllib.request.Request(
                url, headers={"User-Agent": "vela-mdl-to-mtlx"})
            # Automotive textures are often 100MB; retry slow transfers.
            for attempt in range(3):
                try:
                    with urllib.request.urlopen(request,
                                                timeout=300) as response:
                        data = response.read()
                    break
                except Exception as error:
                    if attempt == 2:
                        return url, str(error)
            with open(local + ".part", "wb") as f:
                f.write(data)
            os.replace(local + ".part", local)
            return url, None

        failures = []
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            for url, error in pool.map(get, todo):
                if error:
                    failures.append(url)
                    log("  failed: %s (%s)" % (url, error))
        return failures


Converted = collections.namedtuple(
    "Converted", "signature graph inputs module notes")


class Converter(object):
    def __init__(self, source, output, texture_dir, options, log):
        self.source = os.path.abspath(source)
        self.output = os.path.abspath(output)
        self.texture_dir = texture_dir
        self._base_options = options
        # Referenced assets converted for instances, shared by the run:
        # source -> output, or None when the asset has nothing to convert.
        self.assets = options.setdefault("assets", {})
        self.options = dict(options)
        self.log = log
        self.textures = Textures(os.path.dirname(self.output), texture_dir,
                                 options.get("download", True))
        self.stats = collections.Counter()
        self._unsupported = {}  # material path -> module
        self._instanced = set()
        # Prims whose bindings USD ignores without MaterialBindingAPI, which
        # older Omniverse exports do not apply.
        self._needs_binding_api = set()
        self.notes = collections.Counter()

    def _open(self):
        src = Sdf.Layer.FindOrOpen(self.source)
        if not src:
            raise RuntimeError("cannot open %s" % self.source)
        if os.path.exists(self.output):
            os.remove(self.output)
        layer = Sdf.Layer.CreateNew(self.output)
        for key in ROOT_METADATA:
            if src.pseudoRoot.HasInfo(key):
                layer.pseudoRoot.SetInfo(key, src.pseudoRoot.GetInfo(key))
        relative = os.path.relpath(self.source, os.path.dirname(self.output))
        layer.subLayerPaths.append(
            relative if relative.startswith(".") else "./" + relative)
        layer.comment = ("MaterialX (OpenPBR) versions of the MDL materials "
                         "in %s, by mdl_to_mtlx." % os.path.basename(
                             self.source))
        self.layer = layer
        self.stage = Usd.Stage.Open(layer, load=Usd.Stage.LoadAll)
        self.options.setdefault("meters_per_unit",
                                UsdGeom.GetStageMetersPerUnit(self.stage))

    # Reading

    def _convert(self, material):
        source, _, _ = material.ComputeSurfaceSource("mdl")
        if not source or source.GetImplementationSource() != "sourceAsset":
            return None
        asset = source.GetSourceAsset("mdl")
        module = asset.path if asset else ""
        sub = source.GetSourceAssetSubIdentifier("mdl") or ""
        translate = omni.translator(module, sub)
        name = "%s:%s" % (module.rsplit("/", 1)[-1], sub)
        if translate is None:
            self._unsupported[material.GetPath()] = name
            return None
        values, textures = {}, {}
        for shader_input in source.GetInputs():
            attrs = shader_input.GetValueProducingAttributes()
            attr = attrs[0] if attrs else shader_input.GetAttr()
            value = _python(attr.Get())
            if isinstance(value, Sdf.AssetPath):
                texture = self.textures.resolve(attr, module)
                if texture is not None:
                    textures[shader_input.GetBaseName()] = texture
            elif value is not None:
                values[shader_input.GetBaseName()] = value
        params = omni.Params(values, textures, self.options)
        g = graph.Graph()
        inputs = translate(g, params)
        for key, value in list(inputs.items()):
            if isinstance(value, bool):
                inputs[key] = graph.Raw("boolean", value)
        return Converted(g.signature(inputs), g, inputs, name,
                         frozenset(params.notes))

    def _collect(self, root):
        """Converts the MDL materials at and below root."""
        found = {}
        it = iter(Usd.PrimRange(root, Usd.TraverseInstanceProxies()))
        for prim in it:
            if not prim.IsA(UsdShade.Material):
                if not prim.IsInstanceProxy() and \
                        not prim.HasAPI(UsdShade.MaterialBindingAPI) and \
                        any(name.startswith("material:binding")
                            for name in prim.GetPropertyNames()):
                    self._needs_binding_api.add(prim.GetPath())
                continue
            it.PruneChildren()
            material = UsdShade.Material(prim)
            if prim.IsInstanceProxy():
                if material.GetSurfaceOutput("mdl"):
                    self._instanced.add(prim.GetPath())
                continue
            mtlx = material.GetSurfaceOutput("mtlx")
            if mtlx and mtlx.HasConnectedSource() and \
                    not self.options.get("force"):
                self.stats["already MaterialX"] += 1
                continue
            converted = self._convert(material)
            if converted is not None:
                found[prim.GetPath()] = converted
        return found

    def _variant_sets(self):
        for prim in self.stage.Traverse():
            names = prim.GetVariantSets().GetNames()
            for name in names:
                yield prim.GetPath(), name

    def _select(self, path, set_name, variant):
        with Usd.EditContext(self.stage, self.stage.GetSessionLayer()):
            vset = self.stage.GetPrimAtPath(path).GetVariantSet(set_name)
            if variant is None:
                vset.ClearVariantSelection()
            else:
                vset.SetVariantSelection(variant)

    # Writing

    def _author(self, path, converted):
        graph.author(self.stage, path, converted.graph, omni.OPEN_PBR,
                     converted.inputs, self.textures.material_x_color_space)

    def run(self):
        self._open()
        root = self.stage.GetPseudoRoot()
        base = self._collect(root)

        # Materials whose conversion differs by variant: (prim, set) ->
        # {variant: {material: Converted}}.
        by_variant = {}
        owner = {}
        if self.options.get("variants", True):
            for path, set_name in list(self._variant_sets()):
                prim = self.stage.GetPrimAtPath(path)
                if not prim:
                    continue
                vset = prim.GetVariantSet(set_name)
                results = {}
                for variant in vset.GetVariantNames():
                    self._select(path, set_name, variant)
                    results[variant] = self._collect(
                        self.stage.GetPrimAtPath(path))
                self._select(path, set_name, None)
                changed = set()
                for found in results.values():
                    for material, converted in found.items():
                        old = base.get(material)
                        if old is None or old.signature != converted.signature:
                            changed.add(material)
                changed -= set(owner)
                if changed:
                    by_variant[(path, set_name)] = results
                    for material in changed:
                        owner[material] = (path, set_name)
                    self.stats["variant sets with material changes"] += 1

        # Textures first: "auto" color spaces depend on the files.
        self.stats["textures"] = len(self.textures.urls)
        failures = self.textures.fetch(self.log)
        self.stats["texture downloads failed"] = len(failures)

        if self._instanced:
            self._redirect_instances()

        for path in sorted(self._needs_binding_api):
            spec = Sdf.CreatePrimInLayer(self.layer, path)
            spec.SetInfo("apiSchemas", Sdf.TokenListOp.Create(
                prependedItems=["MaterialBindingAPI"]))
        self.stats["MaterialBindingAPI applied"] = len(self._needs_binding_api)
        self.stats["instance proxy materials"] = len(self._instanced)

        # Unchanged materials at the top level.
        for path, converted in sorted(base.items()):
            if path in owner:
                continue
            self._author(path, converted)
            self._count(converted)

        # The rest in each variant of the set that changes them.
        for (path, set_name), results in sorted(by_variant.items()):
            prim = self.stage.GetPrimAtPath(path)
            vset = prim.GetVariantSets().AddVariantSet(set_name)
            for variant, found in sorted(results.items()):
                mine = [m for m in found if owner.get(m) == (path, set_name)]
                if not mine:
                    continue
                vset.AddVariant(variant)
                self._select(path, set_name, variant)
                with vset.GetVariantEditContext():
                    for material in sorted(mine):
                        self._author(material, found[material])
                        self._count(found[material])
                        self.stats["variant networks"] += 1
                self._select(path, set_name, None)

        self.layer.Save()
        return self.stats

    # Instances

    def _convert_asset(self, source):
        """Converts a referenced asset to ASSET_mtlx.usda beside it, once
        per run. Returns the output path, or None."""
        if source in self.assets:
            return self.assets[source]
        self.assets[source] = None  # also guards against cycles
        stem, _ = os.path.splitext(source)
        output = stem + SUFFIX + ".usda"
        child = Converter(source, output, self.texture_dir,
                          self._base_options, self.log)
        stats = child.run()
        if stats["materials"] or stats["instanced assets redirected"]:
            self.assets[source] = output
            self.stats["instanced asset layers written"] += 1
            self.stats["materials in instanced assets"] += \
                stats["materials"] + stats["materials in instanced assets"]
            self.stats["instanced asset layers written"] += \
                stats["instanced asset layers written"]
        else:
            os.remove(output)
        return self.assets[source]

    def _redirect_instances(self):
        """Opinions under an instance are ignored, so materials in
        instances are converted in the referenced asset instead, and the
        reference (or payload) that brings the asset in is pointed at the
        converted layer. Instances of one asset still share a prototype."""
        # The outermost instance: nested instances are handled when the
        # asset it references is converted.
        instances = set()
        for path in self._instanced:
            prim = self.stage.GetPrimAtPath(path)
            outer = None
            while prim:
                if prim.IsInstance() and not prim.IsInstanceProxy():
                    outer = prim
                prim = prim.GetParent()
            if outer:
                instances.add(outer)
        root_stack = set(self.stage.GetLayerStack(includeSessionLayers=False))
        redirects = collections.defaultdict(dict)  # (path, payload) -> map
        for prim in sorted(instances, key=lambda p: p.GetPath()):
            query = Usd.PrimCompositionQuery(prim)
            for arc in query.GetCompositionArcs():
                payload = arc.GetArcType() == Pcp.ArcTypePayload
                if arc.GetArcType() != Pcp.ArcTypeReference and not payload:
                    continue
                layer = arc.GetIntroducingLayer()
                intro = arc.GetIntroducingPrimPath()
                if layer not in root_stack or intro.ContainsPrimVariantSelection():
                    self.stats["instances not redirected"] += 1
                    break
                target = arc.GetTargetNode().layerStack.identifier.rootLayer
                source = target.realPath
                if not source or not os.path.exists(source):
                    break
                output = self._convert_asset(os.path.normpath(source))
                if output:
                    redirects[(intro, payload)][os.path.normpath(source)] = output
                break  # the outermost arc brings in the instance
        for (path, payload), mapping in sorted(redirects.items()):
            self._author_redirect(path, payload, mapping)
            self.stats["instanced assets redirected"] += 1

    def _absolute(self, item, layer, payload):
        path = item.assetPath
        if path and not _is_url(path):
            path = layer.ComputeAbsolutePath(path)
        cls = Sdf.Payload if payload else Sdf.Reference
        if payload:
            return cls(path, item.primPath, item.layerOffset)
        return cls(path, item.primPath, item.layerOffset, item.customData)

    def _author_redirect(self, path, payload, mapping):
        """Writes an explicit reference (or payload) list at path: the root
        layer stack's composed list, with mapped assets replaced, and asset
        paths re-anchored to the output layer."""
        items = []
        field = "payloadList" if payload else "referenceList"
        list_type = Sdf.PayloadListOp if payload else Sdf.ReferenceListOp
        for layer in reversed(self.stage.GetLayerStack(
                includeSessionLayers=False)):
            if layer == self.layer:
                continue
            spec = layer.GetPrimAtPath(path)
            if not spec:
                continue
            op = getattr(spec, field)

            def fix(xs):
                return [self._absolute(x, layer, payload) for x in xs]
            if op.isExplicit:
                absolute = list_type.CreateExplicit(fix(op.explicitItems))
            else:
                absolute = list_type.Create(
                    prependedItems=fix(op.prependedItems),
                    appendedItems=fix(op.appendedItems),
                    deletedItems=fix(op.deletedItems))
            items = absolute.ApplyOperations(items)
        out_dir = os.path.dirname(self.output)
        result = []
        for item in items:
            asset = item.assetPath
            if asset and not _is_url(asset):
                asset = os.path.normpath(asset)
                asset = mapping.get(asset, asset)
                relative = os.path.relpath(asset, out_dir).replace(os.sep, "/")
                asset = relative if relative.startswith("../") \
                    else "./" + relative
            if payload:
                result.append(Sdf.Payload(asset, item.primPath,
                                          item.layerOffset))
            else:
                result.append(Sdf.Reference(asset, item.primPath,
                                            item.layerOffset, item.customData))
        spec = Sdf.CreatePrimInLayer(self.layer, path)
        if payload:
            spec.payloadList.explicitItems = result
        else:
            spec.referenceList.explicitItems = result

    @property
    def unsupported(self):
        return collections.Counter(self._unsupported.values())

    def _count(self, converted):
        self.stats["materials"] += 1
        self.stats[converted.module] += 1
        for note in converted.notes:
            self.notes[note] += 1


# Finding root layers

def _usd_files(directory):
    for dirpath, dirnames, filenames in os.walk(directory):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            stem, ext = os.path.splitext(name)
            if ext.lower() in USD_EXTENSIONS and not stem.endswith(SUFFIX):
                yield os.path.join(dirpath, name)


def _dependencies(layer):
    if hasattr(layer, "GetCompositionAssetDependencies"):
        paths = layer.GetCompositionAssetDependencies()
    else:
        paths = layer.GetExternalReferences()
    for path in paths:
        if path and not _is_url(path):
            yield os.path.normpath(layer.ComputeAbsolutePath(path))


def find_roots(paths):
    """(root layer, directory given) pairs: files as given, with None; for
    directories, USD files no other file in the same tree references."""
    roots = []
    for path in paths:
        if os.path.isfile(path):
            roots.append((os.path.abspath(path), None))
            continue
        files = [os.path.normpath(os.path.abspath(f))
                 for f in _usd_files(path)]
        referenced = set()
        for f in files:
            layer = Sdf.Layer.FindOrOpen(f)
            if layer:
                referenced.update(_dependencies(layer))
        directory = os.path.abspath(path)
        roots.extend((f, directory) for f in files if f not in referenced)
    return roots


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="Approximations are described in omni.py.")
    parser.add_argument("paths", nargs="+", help="USD files or directories")
    parser.add_argument("-o", "--output",
                        help="output layer (for a single input file)")
    parser.add_argument("--textures",
                        help="directory for downloaded textures "
                             "(default: mdl_textures beside each output)")
    parser.add_argument("--no-download", action="store_true",
                        help="write remote texture paths without fetching")
    parser.add_argument("--no-variants", action="store_true",
                        help="convert only the default variant selections")
    parser.add_argument("--force", action="store_true",
                        help="convert materials that already have MaterialX")
    parser.add_argument("--emission-reference", type=float,
                        default=omni.DEFAULT_EMISSION_REFERENCE,
                        help="MDL emission intensity shown at full brightness")
    parser.add_argument("--list", action="store_true",
                        help="list the root layers that would be converted")
    args = parser.parse_args(argv)

    roots = find_roots(args.paths)
    if args.output and len(roots) != 1:
        parser.error("--output needs exactly one root layer")
    if args.list:
        print("\n".join(root for root, _ in roots))
        return 0

    def log(message):
        print(message, flush=True)

    options = {
        "download": not args.no_download,
        "variants": not args.no_variants,
        "force": args.force,
        "emission_reference": args.emission_reference,
    }
    failed = 0
    for root, directory in roots:
        stem, _ = os.path.splitext(root)
        output = args.output or stem + SUFFIX + ".usda"
        # Scenes in a directory share its textures; NVIDIA's packs reuse
        # the same multi-gigabyte texture sets across scenes.
        texture_dir = args.textures or os.path.join(
            directory or os.path.dirname(os.path.abspath(output)),
            "mdl_textures")
        log("%s\n  -> %s" % (root, output))
        converter = Converter(root, output, texture_dir, options, log)
        try:
            stats = converter.run()
        except Exception as error:  # keep going through a directory
            failed += 1
            log("  error: %s" % error)
            continue
        if not (stats["materials"] or stats["instanced assets redirected"]):
            os.remove(output)
            log("  no MDL materials converted; nothing written")
        for key, n in sorted(stats.items()):
            if n:
                log("  %6d  %s" % (n, key))
        for name, n in converter.unsupported.most_common():
            log("  %6d  unsupported %s" % (n, name))
        for note, n in converter.notes.most_common():
            log("  %6d  approximation: %s" % (n, note))
        for path, n in converter.textures.missing.most_common():
            log("  %6d  missing texture %s" % (n, path))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
