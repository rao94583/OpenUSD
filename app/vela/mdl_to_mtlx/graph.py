"""MaterialX node graphs written as expressions, with constant folding.

An Expr is either a constant or the output of a node. Operations on
constants are evaluated in Python, so a material without textures becomes a
single surface node with literal inputs. Identical nodes are shared.
"""

import math

from pxr import Gf, Sdf, UsdShade

# MaterialX type -> (Sdf type, number of components).
TYPES = {
    "float": (Sdf.ValueTypeNames.Float, 1),
    "integer": (Sdf.ValueTypeNames.Int, 1),
    "boolean": (Sdf.ValueTypeNames.Bool, 1),
    "string": (Sdf.ValueTypeNames.String, 1),
    "filename": (Sdf.ValueTypeNames.Asset, 1),
    "vector2": (Sdf.ValueTypeNames.Float2, 2),
    "vector3": (Sdf.ValueTypeNames.Float3, 3),
    "vector4": (Sdf.ValueTypeNames.Float4, 4),
    "color3": (Sdf.ValueTypeNames.Color3f, 3),
    "color4": (Sdf.ValueTypeNames.Color4f, 4),
}
VECTORS = ("vector2", "vector3", "vector4", "color3", "color4")
UDIM = "<UDIM>"


def size(type_name):
    return TYPES[type_name][1]


class Expr(object):
    """A constant (node is None) or a node output."""

    __slots__ = ("type", "value", "node")

    def __init__(self, type_name, value=None, node=None):
        self.type = type_name
        self.value = value
        self.node = node

    @property
    def const(self):
        return self.node is None

    def key(self):
        return ("c", self.type, self.value) if self.const else ("n", self.node)


class Raw(object):
    """A literal input that is not an Expr: a file, string, int or bool,
    with optional attribute metadata (colorSpace)."""

    __slots__ = ("type", "value", "color_space")

    def __init__(self, type_name, value, color_space=None):
        self.type = type_name
        self.value = value
        self.color_space = color_space

    def key(self):
        return ("r", self.type, self.value, self.color_space)


def _tuple(value, n):
    if n == 1:
        return float(value)
    if isinstance(value, (int, float)):
        return (float(value),) * n
    value = tuple(float(v) for v in value)
    if len(value) != n:
        raise ValueError("expected %d components, got %r" % (n, value))
    return value


def _zip(fn, a, b):
    if isinstance(a, tuple) or isinstance(b, tuple):
        n = len(a) if isinstance(a, tuple) else len(b)
        a = a if isinstance(a, tuple) else (a,) * n
        b = b if isinstance(b, tuple) else (b,) * n
        return tuple(fn(x, y) for x, y in zip(a, b))
    return fn(a, b)


def _map(fn, a):
    return tuple(fn(x) for x in a) if isinstance(a, tuple) else fn(a)


def _clamp01(x):
    return min(max(x, 0.0), 1.0)


class Graph(object):
    def __init__(self):
        # name -> (nodedef id, output type, {input: Expr|Raw})
        self.nodes = {}
        self._by_key = {}
        self._names = {}

    # Constants

    def const(self, value, type_name="float"):
        return Expr(type_name, _tuple(value, size(type_name)))

    def lift(self, x, type_name="float"):
        return x if isinstance(x, Expr) else self.const(x, type_name)

    # Nodes

    def node(self, nodedef, out_type, prefix, **inputs):
        key = (nodedef, tuple(sorted((k, v.key()) for k, v in inputs.items())))
        name = self._by_key.get(key)
        if name is None:
            n = self._names.get(prefix, 0)
            self._names[prefix] = n + 1
            name = prefix if n == 0 else "%s_%d" % (prefix, n)
            self._by_key[key] = name
            self.nodes[name] = (nodedef, out_type, inputs)
        return Expr(out_type, node=name)

    def _binary(self, op, fn, a, b):
        a, b = self.lift(a), self.lift(b)
        if a.type == "float" and b.type != "float":
            if op in ("multiply", "add"):
                a, b = b, a
            else:
                a = self.convert(a, b.type)
        if a.const and b.const:
            return Expr(a.type, _zip(fn, a.value, b.value))
        suffix = "FA" if b.type == "float" and a.type != "float" else ""
        return self.node("ND_%s_%s%s" % (op, a.type, suffix), a.type, op,
                         in1=a, in2=b)

    def mul(self, a, b):
        a, b = self.lift(a), self.lift(b)
        for x, y in ((a, b), (b, a)):
            ones = x.value == 1.0 or x.value == (1.0,) * size(x.type)
            if x.const and ones and x.type in ("float", y.type):
                return y
        return self._binary("multiply", lambda x, y: x * y, a, b)

    def add(self, a, b):
        a, b = self.lift(a), self.lift(b)
        if b.const and b.type == "float" and b.value == 0.0:
            return a
        return self._binary("add", lambda x, y: x + y, a, b)

    def sub(self, a, b):
        return self._binary("subtract", lambda x, y: x - y, a, b)

    def min(self, a, b):
        return self._binary("min", min, a, b)

    def max(self, a, b):
        return self._binary("max", max, a, b)

    def mix(self, bg, fg, t):
        """bg * (1 - t) + fg * t, as MaterialX mix(fg, bg, mix)."""
        t = self.lift(t)
        type_name = bg.type if isinstance(bg, Expr) else (
            fg.type if isinstance(fg, Expr) else "float")
        bg, fg = self.lift(bg, type_name), self.lift(fg, type_name)
        if bg.type != fg.type:
            bg, fg = self.convert(bg, type_name), self.convert(fg, type_name)
        if t.const and t.value == 0.0:
            return bg
        if t.const and t.value == 1.0:
            return fg
        if bg.const and fg.const and t.const:
            return Expr(type_name, _zip(lambda x, y: x + (y - x) * t.value,
                                        bg.value, fg.value))
        return self.node("ND_mix_%s" % type_name, type_name, "mix",
                         fg=fg, bg=bg, mix=t)

    def clamp01(self, a):
        a = self.lift(a)
        if a.const:
            return Expr(a.type, _map(_clamp01, a.value))
        suffix = "FA" if a.type != "float" else ""
        return self.node("ND_clamp_%s%s" % (a.type, suffix), a.type, "clamp",
                         **{"in": a})

    def smoothstep(self, x, low, high):
        x, low, high = self.lift(x), self.lift(low), self.lift(high)
        if x.const and low.const and high.const:
            if high.value <= low.value:
                return self.const(1.0 if x.value >= high.value else 0.0)
            t = _clamp01((x.value - low.value) / (high.value - low.value))
            return self.const(t * t * (3.0 - 2.0 * t))
        return self.node("ND_smoothstep_float", "float", "smoothstep",
                         **{"in": x, "low": low, "high": high})

    def ifge(self, value, threshold, if_true, if_false):
        """value >= threshold ? if_true : if_false, for floats."""
        value, threshold = self.lift(value), self.lift(threshold)
        if_true, if_false = self.lift(if_true), self.lift(if_false)
        if value.const and threshold.const:
            return if_true if value.value >= threshold.value else if_false
        return self.node("ND_ifgreatereq_float", "float", "ifgreatereq",
                         value1=value, value2=threshold,
                         in1=if_true, in2=if_false)

    def extract(self, v, index):
        if v.type == "float":
            return v
        if v.const:
            return self.const(v.value[index])
        return self.node("ND_extract_%s" % v.type, "float", "extract",
                         **{"in": v, "index": Raw("integer", index)})

    def luminance(self, c):
        """Rec. 709 luminance of a color3 as a float."""
        weights = (0.2126, 0.7152, 0.0722)
        if c.const:
            return self.const(sum(w * x for w, x in zip(weights, c.value)))
        lum = self.node("ND_luminance_color3", "color3", "luminance",
                        **{"in": c})
        return self.extract(lum, 0)

    def convert(self, a, type_name):
        a = self.lift(a)
        if a.type == type_name:
            return a
        if a.const:
            if a.type == "float":
                return self.const(a.value, type_name)
            return self.const(a.value[:size(type_name)], type_name)
        return self.node("ND_convert_%s_%s" % (a.type, type_name), type_name,
                         "convert", **{"in": a})

    def normalize(self, v):
        if v.const:
            n = math.sqrt(sum(x * x for x in v.value)) or 1.0
            return Expr(v.type, tuple(x / n for x in v.value))
        return self.node("ND_normalize_%s" % v.type, v.type, "normalize",
                         **{"in": v})

    def dot(self, a, b):
        a, b = self.lift(a, "vector3"), self.lift(b, "vector3")
        if a.const and b.const:
            return self.const(sum(x * y for x, y in zip(a.value, b.value)))
        return self.node("ND_dotproduct_vector3", "float", "dot",
                         in1=a, in2=b)

    # Geometry

    def texcoord(self, index=0):
        if index == 0:
            return self.node("ND_texcoord_vector2", "vector2", "texcoord")
        # Storm reads texcoord index 0 as "st"; other sets by name.
        return self.node("ND_geompropvalue_vector2", "vector2", "uv",
                         geomprop=Raw("string", "st%d" % index))

    def position(self, space="object"):
        return self.node("ND_position_vector3", "vector3", "position",
                         space=Raw("string", space))

    def normal(self, space="world"):
        return self.node("ND_normal_vector3", "vector3", "normal",
                         space=Raw("string", space))

    def primvar_float(self, name):
        return self.node("ND_geompropvalue_float", "float", "primvar",
                         geomprop=Raw("string", name))

    def rotate2d(self, v, degrees):
        return self.node("ND_rotate2d_vector2", "vector2", "rotate",
                         **{"in": v, "amount": self.lift(degrees)})

    # Textures

    def image(self, texture, type_name, texcoord=None, default=None):
        if getattr(texture, "tiles", None):
            return self._udim(texture, type_name, texcoord, default)
        inputs = {"file": Raw("filename", texture.path, texture.color_space)}
        if texcoord is not None:
            inputs["texcoord"] = texcoord
        if default is not None:
            inputs["default"] = self.lift(default, type_name)
        return self.node("ND_image_%s" % type_name, type_name, "image",
                         **inputs)

    def _udim(self, texture, type_name, texcoord, default):
        """A UDIM texture as one image per tile, chosen by the tile the
        texture coordinate is in. Storm does not load UDIM files in
        MaterialX networks."""
        uv = texcoord if texcoord is not None else self.texcoord()

        def tile(n):
            return self.image(texture._replace(
                path=texture.path.replace(UDIM, str(n)), tiles=None),
                type_name, uv, default)

        tiles = sorted(texture.tiles)
        result = tile(tiles[0])
        if len(tiles) == 1:
            return result
        cell = self.node("ND_floor_vector2", "vector2", "floor", **{"in": uv})
        tile_id = self.add(self.add(self.extract(cell, 0),
                                    self.mul(self.extract(cell, 1), 10.0)),
                           1001.0)
        for n in tiles[1:]:
            result = self.node("ND_ifequal_%s" % type_name, type_name,
                               "udim", value1=tile_id,
                               value2=self.const(float(n)),
                               in1=tile(n), in2=result)
        return result

    def triplanar(self, texture, type_name, position, blend=None):
        if getattr(texture, "tiles", None):
            texture = texture._replace(path=texture.path.replace(
                UDIM, str(min(texture.tiles))), tiles=None)
        f = Raw("filename", texture.path, texture.color_space)
        inputs = {"filex": f, "filey": f, "filez": f, "position": position}
        if blend is not None:
            inputs["blend"] = self.lift(blend)
        return self.node("ND_triplanarprojection_%s" % type_name, type_name,
                         "triplanar", **inputs)

    def normalmap(self, tangent_normal, scale=1.0):
        return self.node("ND_normalmap_float", "vector3", "normalmap",
                         **{"in": tangent_normal, "scale": self.lift(scale)})

    # Output

    def signature(self, surface_inputs):
        """A hashable description, to compare graphs."""
        return (tuple(sorted((name, d, t, tuple(sorted(
            (k, v.key()) for k, v in i.items())))
            for name, (d, t, i) in self.nodes.items())),
            tuple(sorted((k, v.key()) for k, v in surface_inputs.items())))


def _value(type_name, value):
    if type_name in ("vector2",):
        return Gf.Vec2f(*value)
    if type_name in ("vector3", "color3"):
        return Gf.Vec3f(*value)
    if type_name in ("vector4", "color4"):
        return Gf.Vec4f(*value)
    if type_name == "filename":
        return Sdf.AssetPath(value)
    return value


def _set_input(shader, name, x, stage, scope, color_space, redirect,
               raw_files=False):
    if isinstance(x, Raw):
        attr = shader.CreateInput(name, TYPES[x.type][0])
        attr.Set(_value(x.type, x.value))
        if x.type == "filename" and not raw_files:
            # MaterialX reads a file without a colorspace (Storm loads it
            # raw) as already in the working space; it does not know "raw".
            cs = color_space(x.value, x.color_space)
            if cs and cs != "raw":
                attr.GetAttr().SetColorSpace(cs)
    elif x.const:
        shader.CreateInput(name, TYPES[x.type][0]).Set(_value(x.type, x.value))
    else:
        node = redirect.get(x.node, x.node)
        source = UsdShade.Shader(stage.GetPrimAtPath(scope.AppendChild(node)))
        shader.CreateInput(name, TYPES[x.type][0]).ConnectToSource(
            source.ConnectableAPI(), "out")


TRIPLANAR_COLOR = "ND_triplanarprojection_color3"
SRGB_TO_LINEAR = "ND_srgb_texture_to_lin_rec709_color3"


def author(stage, material_path, graph, surface_id, surface_inputs,
           color_space=lambda path, cs: cs, scope_name="MaterialX"):
    """Defines the graph's nodes under material_path/scope_name, a surface
    shader, and the material's mtlx surface output, at the stage's current
    edit target. color_space(path, cs) maps a file's colorSpace."""
    scope = material_path.AppendChild(scope_name)
    stage.DefinePrim(scope, "Scope")

    def define(name, nodedef, out_type):
        shader = UsdShade.Shader.Define(stage, scope.AppendChild(name))
        shader.SetShaderId(nodedef)
        shader.CreateOutput("out", TYPES[out_type][0])
        return shader

    # MaterialX inserts a color transform for a triplanar node's files
    # inside its implementation graph, where it makes a cycle. Read those
    # files raw and convert after the node instead.
    redirect = {}
    for name, (nodedef, out_type, inputs) in graph.nodes.items():
        define(name, nodedef, out_type)
        if nodedef == TRIPLANAR_COLOR:
            f = inputs["filex"]
            if color_space(f.value, f.color_space) == "srgb_texture":
                linear = define(name + "_linear", SRGB_TO_LINEAR, out_type)
                linear.CreateInput("in", TYPES[out_type][0]).ConnectToSource(
                    UsdShade.Shader(stage.GetPrimAtPath(
                        scope.AppendChild(name))).ConnectableAPI(), "out")
                redirect[name] = name + "_linear"
    for name, (nodedef, _, inputs) in graph.nodes.items():
        shader = UsdShade.Shader(stage.GetPrimAtPath(scope.AppendChild(name)))
        for input_name, x in sorted(inputs.items()):
            _set_input(shader, input_name, x, stage, scope, color_space,
                       redirect, raw_files=nodedef == TRIPLANAR_COLOR)
    surface = UsdShade.Shader.Define(stage, scope.AppendChild("Surface"))
    surface.SetShaderId(surface_id)
    for name, x in sorted(surface_inputs.items()):
        _set_input(surface, name, x, stage, scope, color_space, redirect)
    out = surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material = UsdShade.Material(stage.GetPrimAtPath(material_path))
    material.CreateSurfaceOutput("mtlx").ConnectToSource(out)
