"""Translations of NVIDIA Omniverse MDL materials to OpenPBR Surface.

Each translator reads an MDL shader's composed inputs (Params) and returns
OpenPBR inputs as graph expressions. The MDL defaults and formulas follow
the modules' sources:

    OmniPBR, OmniPBR_ClearCoat, OmniPBR_Opacity  core library (via OmniPBRBase)
    OmniGlass                                     OmniGlass_Opacity
    OmniSurface, OmniSurfaceLite                  OmniSurfaceBase
    OmniUber_Automotive                           Materials/2023_1/Automotive

Approximations are noted where they occur. In general: triplanar normal
maps, detail normals, volume absorption, flakes (averaged into the base
layer), anisotropy direction and emission temperature are simplified or
dropped, and emission is scaled to display range (see emission_luminance).
"""

import collections
import math

# tiles: the UDIM tile numbers when path contains <UDIM>.
Texture = collections.namedtuple("Texture", "path color_space tiles",
                                 defaults=(None,))

OPEN_PBR = "ND_open_pbr_surface_surfaceshader"

# MDL emission in RTX is physical and exposed by the renderer. Storm adds
# emission to the displayed color unexposed, so intensities are divided by
# this reference and capped at 1, which keeps the hue of bright emitters.
DEFAULT_EMISSION_REFERENCE = 40.0


class Params(object):
    """Composed MDL inputs: plain values, plus textures resolved to paths."""

    def __init__(self, values, textures, options=None):
        self.values = values
        self.textures = textures
        self.options = options or {}
        self.notes = set()

    def get(self, name, default):
        value = self.values.get(name)
        if value is None:
            return default
        if isinstance(default, tuple) and not isinstance(value, tuple):
            return (value,) * len(default)
        return value

    def flag(self, name, default):
        return bool(self.get(name, default))

    def int(self, name, default):
        return int(self.get(name, default))

    def tex(self, name):
        return self.textures.get(name)

    def note(self, text):
        self.notes.add(text)


def _raw(texture):
    """Data textures are read without color conversion."""
    return texture._replace(color_space="raw")


def _flip(g, v, flip_r, flip_g):
    """Inverts the red and/or green channel of a tangent-space normal."""
    if not (flip_r or flip_g):
        return v
    scale = (-1.0 if flip_r else 1.0, -1.0 if flip_g else 1.0, 1.0)
    offset = (1.0 if flip_r else 0.0, 1.0 if flip_g else 0.0, 0.0)
    return g.add(g.mul(v, g.const(scale, "vector3")),
                 g.const(offset, "vector3"))


def emission_luminance(p, intensity):
    reference = p.options.get("emission_reference", DEFAULT_EMISSION_REFERENCE)
    return min(max(intensity, 0.0) / reference, 1.0)


def _cutout(g, opacity, threshold):
    if threshold == 0.0:
        return opacity
    return g.ifge(opacity, threshold, 1.0, 0.0)


# OmniPBR and OmniGlass coordinates

class _Coords(object):
    """Texture coordinates of OmniPBR and OmniGlass: a UV set transformed
    by texture_scale, texture_rotate and texture_translate, or a cubic
    projection (approximated as triplanar)."""

    def __init__(self, g, p):
        self.g = g
        scale = p.get("texture_scale", (1.0, 1.0))
        rotate = p.get("texture_rotate", 0.0)
        translate = p.get("texture_translate", (0.0, 0.0))
        if p.flag("project_uvw", False):
            space = "world" if p.flag("world_or_object", False) else "object"
            self.position = g.mul(g.position(space),
                                  g.const((scale[0], scale[1], scale[0]),
                                          "vector3"))
            self.uv = None
            p.note("cubic projection approximated as triplanar")
        else:
            uv = g.texcoord(p.int("uv_space_index", 0))
            # base::transform_coordinate: scale * rotate(-angle) * uv + t.
            if rotate:
                uv = g.rotate2d(uv, -rotate)
            if tuple(scale) != (1.0, 1.0):
                uv = g.mul(uv, g.const(scale, "vector2"))
            if tuple(translate) != (0.0, 0.0):
                uv = g.add(uv, g.const(translate, "vector2"))
            self.uv = uv
            self.position = None

    def lookup(self, texture, type_name):
        if self.uv is not None:
            return self.g.image(texture, type_name, self.uv)
        return self.g.triplanar(texture, type_name, self.position)

    def normal(self, texture, strength, flip_u, flip_v):
        """OmniPBRBase::tangent_space_normal_roughness_texture. flip_v
        defaults to true for OpenGL normal maps, so only a non-default flag
        inverts a channel here."""
        if texture is None or self.uv is None or strength == 0.0:
            return None
        g = self.g
        n = g.image(_raw(texture), "vector3", self.uv)
        return g.normalmap(_flip(g, n, flip_u, not flip_v), strength)


def _data(coords, texture):
    """A raw float3 lookup; MDL returns zero for a missing texture."""
    if texture is None:
        return coords.g.const((0.0, 0.0, 0.0), "vector3")
    return coords.lookup(_raw(texture), "vector3")


def omni_pbr(g, p):
    c = _Coords(g, p)
    out = {}

    # Albedo
    diffuse = p.tex("diffuse_texture")
    if diffuse is not None:
        base = g.add(g.mul(c.lookup(diffuse, "color3"),
                           p.get("albedo_brightness", 1.0)),
                     p.get("albedo_add", 0.0))
        desat = p.get("albedo_desaturation", 0.0)
        if desat:
            base = g.mix(base, g.convert(g.luminance(base), "color3"), desat)
    else:
        base = g.const(p.get("diffuse_color_constant", (0.2, 0.2, 0.2)),
                       "color3")
    base = g.mul(base, g.const(p.get("diffuse_tint", (1.0, 1.0, 1.0)),
                               "color3"))

    orm_enabled = p.flag("enable_ORM_texture", False)
    orm = _data(c, p.tex("ORM_texture")) if orm_enabled else None

    ao_to_diffuse = p.get("ao_to_diffuse", 0.0)
    if ao_to_diffuse:
        if orm is not None:
            ao = g.extract(orm, 0)
        elif p.tex("ao_texture") is not None:
            ao = g.extract(_data(c, p.tex("ao_texture")), 0)
        else:
            ao = g.const(1.0)
        base = g.mix(base, g.mul(base, ao), ao_to_diffuse)
    out["base_color"] = base

    # Reflectivity
    rough = orm if orm is not None else _data(
        c, p.tex("reflectionroughness_texture"))
    out["specular_roughness"] = g.mix(
        p.get("reflection_roughness_constant", 0.5),
        g.extract(rough, 1 if orm is not None else 0),
        p.get("reflection_roughness_texture_influence", 0.0))
    metal = orm if orm is not None else _data(c, p.tex("metallic_texture"))
    out["base_metalness"] = g.mix(
        p.get("metallic_constant", 0.0),
        g.extract(metal, 2 if orm is not None else 0),
        p.get("metallic_texture_influence", 0.0))
    # custom_curve_layer(normal_reflectivity: 0.08, weight: specular_level)
    # gives F0 = 0.08 * specular_level; OpenPBR's IOR 1.5 gives F0 = 0.04.
    out["specular_weight"] = g.const(p.get("specular_level", 0.5) / 0.5)

    # Emission
    if p.flag("enable_emission", False):
        color = (c.lookup(p.tex("emissive_color_texture"), "color3")
                 if p.tex("emissive_color_texture") is not None else
                 g.const(p.get("emissive_color", (1.0, 0.1, 0.1)), "color3"))
        mask = p.tex("emissive_mask_texture")
        if mask is not None:
            color = g.mul(color, c.lookup(mask, "color3"))
        out["emission_color"] = color
        out["emission_luminance"] = g.const(
            emission_luminance(p, p.get("emissive_intensity", 40.0)))

    # Cutout opacity
    if p.flag("enable_opacity", False):
        texture = p.tex("opacity_texture")
        if p.flag("enable_opacity_texture", False) and texture is not None:
            v = c.lookup(_raw(texture), "vector4")
            mode = p.int("opacity_mode", 1)  # base::mono_average
            if mode == 0:    # mono_alpha
                opacity = g.extract(v, 3)
            elif mode == 3:  # mono_maximum
                opacity = g.max(g.max(g.extract(v, 0), g.extract(v, 1)),
                                g.extract(v, 2))
            else:
                opacity = g.mul(g.add(g.add(g.extract(v, 0), g.extract(v, 1)),
                                      g.extract(v, 2)), 1.0 / 3.0)
        else:
            opacity = g.const(p.get("opacity_constant", 1.0))
        out["geometry_opacity"] = _cutout(g, opacity,
                                          p.get("opacity_threshold", 0.0))

    normal = c.normal(p.tex("normalmap_texture"), p.get("bump_factor", 1.0),
                      p.flag("flip_tangent_u", False),
                      p.flag("flip_tangent_v", True))
    if normal is not None:
        out["geometry_normal"] = normal
    if p.tex("detail_normalmap_texture") is not None:
        p.note("detail normal map dropped")

    # OmniPBR_ClearCoat
    if p.flag("enable_clearcoat", False):
        out["coat_weight"] = g.const(p.get("clearcoat_weight", 1.0))
        out["coat_color"] = g.const(p.get("clearcoat_tint", (1.0, 1.0, 1.0)),
                                    "color3")
        out["coat_roughness"] = g.const(
            p.get("clearcoat_reflection_roughness", 0.0))
        out["coat_ior"] = g.const(p.get("clearcoat_ior", 1.56))
        out["coat_darkening"] = g.const(0.0)
    return out


def omni_glass(g, p):
    c = _Coords(g, p)
    out = {"transmission_weight": g.const(1.0), "base_metalness": g.const(0.0)}

    def color(texture_name, value_name):
        texture = p.tex(texture_name)
        if texture is not None:
            return c.lookup(texture, "color3")
        return g.const(p.get(value_name, (1.0, 1.0, 1.0)), "color3")

    out["transmission_color"] = color("glass_color_texture", "glass_color")
    out["specular_color"] = color("reflection_color_texture",
                                  "reflection_color")
    out["specular_ior"] = g.const(p.get("glass_ior", 1.491))
    frosting = p.get("frosting_roughness", 0.0)
    texture = p.tex("roughness_texture")
    if texture is not None:
        out["specular_roughness"] = g.mix(
            frosting, g.extract(_data(c, texture), 0),
            p.get("roughness_texture_influence", 1.0))
    else:
        out["specular_roughness"] = g.const(frosting)

    cutout = p.get("cutout_opacity", 1.0)
    cutout_texture = p.tex("cutout_opacity_texture")
    thin = p.flag("thin_walled", False) or cutout_texture is not None or \
        cutout != 1.0
    if thin:
        out["geometry_thin_walled"] = True
    if p.flag("enable_opacity", False):
        if cutout_texture is not None:
            v = c.lookup(_raw(cutout_texture), "vector4")
            opacity = g.extract(v, 3) if p.int(
                "cutout_opacity_mono_source", 0) == 0 else g.extract(v, 0)
        else:
            opacity = g.const(cutout)
        out["geometry_opacity"] = _cutout(g, opacity,
                                          p.get("opacity_threshold", 0.0))

    normal = c.normal(p.tex("normal_map_texture"),
                      p.get("normal_map_strength", 1.0),
                      p.flag("flip_tangent_u", False),
                      p.flag("flip_tangent_v", True))
    if normal is not None:
        out["geometry_normal"] = normal
    return out


# OmniSurface

def omni_surface(g, p):
    """OmniSurface follows Autodesk Standard Surface, which OpenPBR maps
    almost directly. An X_image texture replaces the constant X (colors)
    or scales it (scalars)."""
    if p.int("uvw_texture_coordinate_system", 0) != 0 or \
            p.get("uvw_s_scale", 1.0) != 1.0 or p.get("uvw_t_scale", 1.0) != 1.0:
        p.note("OmniSurface UVW options ignored")
    uv = g.texcoord(p.int("uvw_uv_set", 0))

    def color(name, default):
        texture = p.tex(name + "_image")
        if texture is not None:
            return g.image(texture, "color3", uv)
        return g.const(p.get(name, default), "color3")

    def scalar(name, default):
        value = p.get(name, default)
        texture = p.tex(name + "_image")
        if texture is not None:
            return g.mul(g.extract(g.image(_raw(texture), "vector3", uv), 0),
                         value)
        return g.const(value)

    def normal(name, strength_name, flip_prefix):
        texture = p.tex(name)
        if texture is None:
            return None
        n = g.image(_raw(texture), "vector3", uv)
        n = _flip(g, n, p.flag(flip_prefix + "_flip_r_channel", False),
                  p.flag(flip_prefix + "_flip_g_channel", False))
        return g.normalmap(n, p.get(strength_name, 1.0))

    out = {
        "base_weight": scalar("diffuse_reflection_weight", 0.8),
        "base_color": color("diffuse_reflection_color", (1.0, 1.0, 1.0)),
        "base_diffuse_roughness": scalar("diffuse_reflection_roughness", 0.0),
        "base_metalness": scalar("metalness", 0.0),
        "specular_weight": scalar("specular_reflection_weight", 1.0),
        "specular_color": color("specular_reflection_color", (1.0, 1.0, 1.0)),
        "specular_roughness": scalar("specular_reflection_roughness", 0.2),
        "specular_ior": g.const(p.get("specular_reflection_ior", 1.5)),
        "specular_roughness_anisotropy": scalar(
            "specular_reflection_anisotropy", 0.0),
        "coat_weight": scalar("coat_weight", 0.0),
        "coat_color": color("coat_color", (1.0, 1.0, 1.0)),
        "coat_roughness": scalar("coat_roughness", 0.1),
        "coat_ior": g.const(p.get("coat_ior", 1.5)),
        "coat_darkening": g.const(p.get("coat_affect_color", 0.0)),
    }
    if p.flag("enable_specular_transmission", False):
        out["transmission_weight"] = scalar("specular_transmission_weight", 0.0)
        out["transmission_color"] = color("specular_transmission_color",
                                          (1.0, 1.0, 1.0))
        out["transmission_depth"] = scalar(
            "specular_transmission_scattering_depth", 0.0)
    if p.flag("enable_diffuse_transmission", False):
        out["subsurface_weight"] = scalar("subsurface_weight", 0.0)
        out["subsurface_color"] = color("subsurface_transmission_color",
                                        (1.0, 1.0, 1.0))
        out["subsurface_radius"] = g.const(p.get("subsurface_scale", 1.0))
    if p.get("sheen_weight", 0.0):
        out["fuzz_weight"] = scalar("sheen_weight", 0.0)
        out["fuzz_color"] = color("sheen_color", (1.0, 1.0, 1.0))
        out["fuzz_roughness"] = scalar("sheen_roughness", 0.3)
    if p.flag("enable_thin_film", False):
        out["thin_film_weight"] = g.const(1.0)
        # OmniSurface thickness is in nanometers, OpenPBR's in micrometers.
        out["thin_film_thickness"] = g.const(
            p.get("thin_film_thickness", 400.0) / 1000.0)
        out["thin_film_ior"] = g.const(p.get("thin_film_ior", 1.52))
    weight = p.get("emission_weight", 0.0)
    if weight:
        unit = math.pi * 1000.0 if p.int("emission_mode", 0) == 0 else 1.0
        out["emission_color"] = color("emission_color", (1.0, 1.0, 1.0))
        out["emission_luminance"] = g.const(emission_luminance(
            p, weight * p.get("emission_intensity", 1.0) * unit))
        if p.flag("emission_use_temprature", False) or \
                p.flag("emission_use_temperature", False):
            p.note("emission temperature ignored")
    if p.flag("thin_walled", False):
        out["geometry_thin_walled"] = True
    if p.flag("enable_opacity", False):
        out["geometry_opacity"] = _cutout(
            g, scalar("geometry_opacity", 1.0),
            p.get("geometry_opacity_threshold", 0.0))
    n = normal("geometry_normal_image", "geometry_normal_strength",
               "geometry_normal_image")
    if n is not None:
        out["geometry_normal"] = n
    n = normal("coat_normal_image", "coat_normal_strength", "coat_normal_image")
    if n is not None:
        out["geometry_coat_normal"] = n
    return out


# OmniUber_Automotive

TRIPLANAR = 1           # uvs_mode
ABOVE = 1               # layer_order
CHANNELS = "r g b a rgb".split()


class _Streams(object):
    """The four UV sets of OmniUber_Automotive, each texture_coord_0 (UV
    set 0, scaled and offset) or triplanar (object space, scaled by
    uv_scale / 100)."""

    def __init__(self, g, p):
        self.g = g
        self.p = p
        self.sets = []
        for i in range(1, 5):
            self.sets.append((
                p.int("uvs_mode_%02d" % i, TRIPLANAR),
                p.get("uv_scale_%02d" % i, (1.0, 1.0)),
                p.get("uv_offset_%02d" % i, (0.0, 0.0)),
                p.get("triplanar_blend_range_%02d" % i, 0.5)))

    def _coords(self, selection):
        mode, scale, offset, blend = self.sets[selection]
        g = self.g
        if mode == TRIPLANAR:
            s = scale[0] * 0.01
            return None, g.mul(g.position("object"),
                               g.const((s, s, s), "vector3")), blend
        uv = g.texcoord(0)
        if tuple(scale) != (1.0, 1.0):
            uv = g.mul(uv, g.const(scale, "vector2"))
        if tuple(offset) != (0.0, 0.0):
            uv = g.add(uv, g.const(offset, "vector2"))
        return uv, None, blend

    def lookup(self, selection, texture, type_name):
        uv, position, blend = self._coords(selection)
        if uv is not None:
            return self.g.image(texture, type_name, uv)
        return self.g.triplanar(texture, type_name, position, blend)

    def channel(self, selection, texture, channel, default):
        """lookup_float4_channel / lookup_triplanar_channel: a channel times
        default, or default without a texture."""
        g = self.g
        if texture is None:
            return g.const(default)
        v = self.lookup(selection, _raw(texture), "vector4")
        if CHANNELS[channel] == "rgb":
            value = g.dot(g.convert(v, "vector3"), (0.2126, 0.7152, 0.0722))
        elif CHANNELS[channel] == "a" and self.sets[selection][0] == TRIPLANAR:
            value = g.extract(v, 0)  # as in lookup_triplanar_channel
        else:
            value = g.extract(v, channel)
        return g.mul(value, default)

    def normal(self, selection, texture, scale, flip_green):
        """OmniImage::tangent_space_normal_lookup_2. Normal maps default to
        flip_green true, as OmniPBR defaults to flip_tangent_v, so only a
        false flag inverts green here."""
        if texture is None or scale == 0.0:
            return None
        if self.sets[selection][0] == TRIPLANAR:
            self.p.note("triplanar normal map dropped")
            return None
        g = self.g
        n = self.lookup(selection, _raw(texture), "vector3")
        return g.normalmap(_flip(g, n, False, not flip_green), scale)


def _uber_layer(g, s, p, prefix, defaults):
    """initialize_material_layer for the base layer (prefix "") or a
    blend layer ("layer_1_", "layer_2_")."""
    def get(name, default=None):
        return p.get(prefix + name, defaults.get(name, default))

    selection = p.int("uv_set_layer_%s" % (prefix[6] if prefix else "0"), 0)
    texture = p.tex(prefix + "base_color_image")
    if texture is not None:
        color = s.lookup(selection, texture, "color3")
    else:
        color = g.const((1.0, 1.0, 1.0), "color3")
    color = g.add(g.mul(color, get("base_color_scale", 1.0)),
                  get("base_color_offset", 0.0))
    desat = get("base_color_desaturation", 0.0)
    if desat > 0.0:
        color = g.mix(color, g.convert(g.luminance(color), "color3"), desat)
    color = g.mul(color, g.const(get("diffuse_reflection_color"), "color3"))

    # histogram_range(roughness, range, position)
    rng, pos = get("roughness_range_position", (1.0, 0.5))
    low = 1.0 - min(rng * 0.5 + (1.0 - pos), (1.0 - pos) * 2.0)
    high = min(rng * 0.5 + pos, pos * 2.0)
    rough = s.channel(selection, p.tex(prefix + "roughness_image"),
                      p.int(prefix + "roughness_channel", 1),
                      get("roughness_u", 1.0))
    return {
        "base_color": color,
        "metallic": s.channel(selection, p.tex(prefix + "metalness_image"),
                              p.int(prefix + "metalness_channel", 2),
                              get("metalness")),
        "cavity": s.channel(selection, p.tex(prefix + "cavity_image"),
                            p.int(prefix + "cavity_channel", 0), 1.0),
        "specular_weight": g.const(get("specular_reflection_weight", 1.0)),
        "roughness": g.mix(low, high, g.clamp01(rough)),
        "normal": s.normal(selection, p.tex(prefix + "normal_image"),
                           get("normal_scale", 1.0),
                           p.flag(prefix + "normal_flip_green", True)),
    }


def _threshold_blend(g, value, height, threshold):
    height = g.sub(1.0, height)
    low = g.clamp01(g.sub(height, threshold + 0.0001))
    return g.clamp01(g.smoothstep(value, low, height))


AXES = [(0, 1.0), (0, -1.0), (1, 1.0), (1, -1.0), (2, 1.0), (2, -1.0)]


def _uber_blend_factor(g, s, p, n):
    prefix = "layer_%d_" % n
    if not p.flag("enable_layer_%d" % n, False):
        return g.const(0.0)
    override = p.get(prefix + "blend_factor_override", 0.0)
    if override > 0.0:
        return g.const(override)
    default_set = 0 if n == 1 else 1
    detail_texture = p.tex(prefix + "blend_detail_image")
    detail = g.const(0.5) if detail_texture is None else s.channel(
        p.int(prefix + "uv_set_detail", default_set), detail_texture,
        p.int(prefix + "blend_detail_image_channel", 0), 1.0)
    threshold = p.get(prefix + "blend_threshold", 0.33)
    source = p.int(prefix + "blend_source", 0)
    if source == 1:    # direction
        value = g.dot(g.normal("world"),
                      g.const(p.get(prefix + "blend_direction",
                                    (0.0, 1.0, 0.0)), "vector3"))
    elif source == 2:  # axis
        index, sign = AXES[p.int(prefix + "blend_axis", 2)]
        # Object space stands in for world position minus the object's
        # origin; the two differ for rotated or scaled objects.
        space = "object" if p.flag(prefix + "blend_object_space", True) \
            else "world"
        coordinate = g.extract(g.position(space), index)
        value = g.mul(coordinate, sign * p.options.get("meters_per_unit", 0.01))
        value = g.clamp01(g.sub(
            g.mul(value, p.get(prefix + "blend_scale", 1.0)),
            p.get(prefix + "blend_offset", 0.0)))
        return g.clamp01(g.sub(1.0, _threshold_blend(g, value, detail,
                                                     threshold)))
    elif source == 3:  # primvar
        value = g.primvar_float(p.get(prefix + "blend_primvar_name", ""))
    else:              # texture
        value = s.channel(p.int(prefix + "uv_set_blend_factor", default_set),
                          p.tex(prefix + "blend_splatmap"),
                          p.int(prefix + "blend_splatmap_channel", 0), 1.0)
    return _threshold_blend(g, value, detail, threshold)


def _blend_layers(g, a, b, t):
    out = {}
    for key in ("base_color", "metallic", "cavity", "specular_weight",
                "roughness"):
        out[key] = g.mix(a[key], b[key], t)
    na, nb = a["normal"], b["normal"]
    if na is None and nb is None:
        out["normal"] = None
    elif t.const and t.value == 0.0:
        out["normal"] = na
    else:
        out["normal"] = g.normalize(g.mix(na or g.normal("world"),
                                          nb or g.normal("world"), t))
    return out


def omni_uber_automotive(g, p):
    s = _Streams(g, p)
    layer = _uber_layer(g, s, p, "", {
        "diffuse_reflection_color": (0.463, 0.725, 0.0), "metalness": 0.0})
    blend = {}
    for n in (1, 2):
        blend[n] = _uber_blend_factor(g, s, p, n)
        if blend[n].const and blend[n].value == 0.0:
            continue
        other = _uber_layer(g, s, p, "layer_%d_" % n, {
            "diffuse_reflection_color": (1.0, 1.0, 1.0), "metalness": 1.0})
        layer = _blend_layers(g, layer, other, blend[n])

    out = {}
    # accumulate_coat_weight: layers above the coat remove it.
    coat = g.const(0.0)
    if p.flag("enable_coat", True):
        coat = g.const(1.0)
        for n in (1, 2):
            if p.int("layer_%d_order" % n, ABOVE) == ABOVE:
                coat = g.sub(coat, blend[n])
        mask = p.tex("coat_mask_image")
        if mask is not None:
            coat = g.min(s.channel(p.int("uv_set_coat_mask", 0), mask,
                                   p.int("coat_mask_channel", 0), 1.0), coat)
    if not (coat.const and coat.value <= 0.0):
        selection = p.int("uv_set_coat_roughness_normal", 0)
        out["coat_weight"] = coat
        out["coat_color"] = g.const(p.get("coat_color", (1.0, 1.0, 1.0)),
                                    "color3")
        out["coat_roughness"] = s.channel(
            selection, p.tex("coat_roughness_image"),
            p.int("coat_roughness_channel", 0), p.get("coat_roughness", 0.0))
        out["coat_ior"] = g.const(p.get("coat_ior", 1.6))
        out["coat_darkening"] = g.const(p.get("coat_affect_color", 0.0))
        normal = s.normal(selection, p.tex("coat_normal_image"), 1.0,
                          p.flag("coat_normal_flip_green", True))
        if normal is not None:
            out["geometry_coat_normal"] = normal

    base_color = layer["base_color"]
    metallic = layer["metallic"]
    roughness = layer["roughness"]
    # Flakes are metallic micro-facets where the coat is present, with
    # probability flakes_density squared; average them into the base.
    if p.flag("enable_flakes", True) and "coat_weight" in out:
        density = p.get("flakes_density", 0.25)
        coverage = g.ifge(coat, 0.0001, density * density, 0.0)
        base_color = g.mix(base_color, g.const(
            p.get("flake_color", (1.0, 1.0, 1.0)), "color3"), coverage)
        metallic = g.mix(metallic, 1.0, coverage)
        roughness = g.mix(roughness, p.get("flake_roughness", 0.05), coverage)

    out["base_color"] = base_color
    out["base_metalness"] = metallic
    out["specular_roughness"] = roughness
    out["specular_weight"] = layer["specular_weight"]
    out["specular_color"] = g.mul(
        g.const(p.get("specular_reflection_color", (1.0, 1.0, 1.0)), "color3"),
        g.clamp01(g.mul(layer["cavity"], 2.0)))
    if p.flag("enable_anisotropy", False):
        u = p.get("anisotropic_roughness_u", 1.0)
        v = p.get("anisotropic_roughness_v", 0.25)
        out["specular_roughness_anisotropy"] = g.const(
            1.0 - min(u, v) / max(u, v, 1e-6))
        p.note("anisotropy direction ignored")

    normal = layer["normal"]
    if normal is None and p.flag("enable_detail_normal", False):
        normal = s.normal(p.int("uv_set_detail_normal", 0),
                          p.tex("detail_normal_image"),
                          p.get("detail_normal_scale", 1.0),
                          p.flag("detail_normal_flip_green", True))
    if normal is not None:
        out["geometry_normal"] = normal

    if p.flag("enable_emission", False):
        selection = p.int("uv_set_emission", 0)
        color = g.const(p.get("emission_color", (1.0, 1.0, 1.0)), "color3")
        texture = p.tex("emission_image")
        if texture is not None:
            color = g.mul(s.lookup(selection, texture, "color3"), color)
        mask = p.tex("emission_mask_image")
        if mask is not None:
            color = g.mul(color, s.channel(p.int("uv_set_emission_mask", 0),
                                           mask, 0, 1.0))
        if p.flag("emission_use_temperature", False):
            p.note("emission temperature ignored")
        unit = math.pi * 1000.0 if p.int("emission_mode", 0) == 0 else 1.0
        out["emission_color"] = color
        out["emission_luminance"] = g.const(emission_luminance(
            p, p.get("emission_intensity", 1.0) * unit))

    if p.flag("enable_opacity", False):
        texture = p.tex("opacity_image") or p.tex("base_color_image")
        opacity = s.channel(p.int("uv_set_opacity", 0), texture,
                            p.int("opacity_channel", 3), 1.0)
        out["geometry_opacity"] = _cutout(
            g, opacity, p.get("geometry_opacity_threshold", 0.15))
    return out


TRANSLATORS = {
    "OmniPBR": omni_pbr,
    "OmniPBR_ClearCoat": omni_pbr,
    "OmniPBR_Opacity": omni_pbr,
    "OmniPBR_ClearCoat_Opacity": omni_pbr,
    "OmniGlass": omni_glass,
    "OmniGlass_Opacity": omni_glass,
    "OmniSurface": omni_surface,
    "OmniSurfaceLite": omni_surface,
    "OmniUber_Automotive": omni_uber_automotive,
}


def translator(module, sub_identifier):
    """The translator for an MDL module file name and material name."""
    name = module.rsplit("/", 1)[-1]
    if name.endswith(".mdl"):
        name = name[:-4]
    return TRANSLATORS.get(sub_identifier) or TRANSLATORS.get(name)
