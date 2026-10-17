"""Converts synthetic Omniverse MDL materials and checks the OpenPBR networks."""

import os
import tempfile
import unittest

from pxr import Gf, Sdf, Usd, UsdShade

import convert
import graph
import omni


def mdl_material(stage, path, module, inputs):
    material = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/Shader")
    shader.SetSourceAsset(Sdf.AssetPath(module), "mdl")
    shader.SetSourceAssetSubIdentifier(
        os.path.splitext(os.path.basename(module))[0], "mdl")
    for name, (type_name, value) in inputs.items():
        shader.CreateInput(name, type_name).Set(value)
    material.CreateSurfaceOutput("mdl").ConnectToSource(
        shader.CreateOutput("out", Sdf.ValueTypeNames.Token))
    return material


def surface(stage, path):
    material = UsdShade.Material(stage.GetPrimAtPath(path))
    source, _, _ = material.ComputeSurfaceSource("mtlx")
    return source


def value(shader, name):
    return shader.GetInput(name).Get()


def connected(shader, name):
    source = shader.GetInput(name).GetConnectedSources()[0]
    return UsdShade.Shader(source[0].source.GetPrim())


V = Sdf.ValueTypeNames


class TestGraph(unittest.TestCase):
    def test_constants_fold(self):
        g = graph.Graph()
        x = g.mix(0.2, 0.8, g.const(0.5))
        self.assertTrue(x.const)
        self.assertAlmostEqual(x.value, 0.5)
        c = g.mul(g.const((1.0, 0.5, 0.25), "color3"), 2.0)
        self.assertEqual(c.value, (2.0, 1.0, 0.5))
        self.assertEqual(g.nodes, {})

    def test_nodes_are_shared(self):
        g = graph.Graph()
        t = omni.Texture("a.png", "raw")
        a = g.image(t, "vector3", g.texcoord())
        b = g.image(t, "vector3", g.texcoord())
        self.assertEqual(a.node, b.node)
        self.assertEqual(len(g.nodes), 2)


class TestConvert(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.source = os.path.join(self.dir, "scene.usda")
        stage = Usd.Stage.CreateNew(self.source)
        stage.SetMetadata("metersPerUnit", 0.01)
        stage.SetMetadata("upAxis", "Y")
        open(os.path.join(self.dir, "./albedo.png"), "wb").close()
        mdl_material(stage, "/World/Looks/Painted", "OmniPBR.mdl", {
            "diffuse_texture": (V.Asset, Sdf.AssetPath("./albedo.png")),
            "diffuse_tint": (V.Color3f, Gf.Vec3f(1, 0.5, 0.5)),
            "reflection_roughness_constant": (V.Float, 0.25),
            "metallic_constant": (V.Float, 1.0),
            "specular_level": (V.Float, 0.25),
            "texture_scale": (V.Float2, Gf.Vec2f(2, 2)),
        })
        open(os.path.join(self.dir, "tiles.1001.png"), "wb").close()
        open(os.path.join(self.dir, "tiles.1002.png"), "wb").close()
        mdl_material(stage, "/World/Looks/Tiled", "OmniPBR.mdl", {
            "diffuse_texture": (V.Asset, Sdf.AssetPath("./tiles.<UDIM>.png")),
        })
        mdl_material(stage, "/World/Looks/Glass", "OmniGlass.mdl", {
            "glass_color": (V.Color3f, Gf.Vec3f(0.5, 0.9, 0.5)),
            "glass_ior": (V.Float, 1.5),
            "thin_walled": (V.Bool, True),
        })
        paint = mdl_material(
            stage, "/World/Looks/Paint",
            "https://example.com/Templates/OmniUber_Automotive.mdl", {
                "uvs_mode_01": (V.Int, 0),
            })
        mdl_material(stage, "/World/Looks/Unknown", "Chrome.mdl", {})
        mesh = stage.DefinePrim("/World/Mesh", "Mesh")
        mesh.CreateRelationship("material:binding").SetTargets(
            ["/World/Looks/Painted"])
        looks = stage.GetPrimAtPath("/World/Looks")
        vset = looks.GetVariantSets().AddVariantSet("Color")
        for name, color in (("Red", (0.8, 0.05, 0.05)), ("Blue", (0, 0, 0.8))):
            vset.AddVariant(name)
            vset.SetVariantSelection(name)
            with vset.GetVariantEditContext():
                shader = UsdShade.Shader(paint.GetPrim().GetChild("Shader"))
                shader.CreateInput("diffuse_reflection_color",
                                   V.Color3f).Set(Gf.Vec3f(*color))
        vset.SetVariantSelection("Red")
        stage.Save()
        self.output = os.path.join(self.dir, "scene_mtlx.usda")
        converter = convert.Converter(
            self.source, self.output, os.path.join(self.dir, "textures"),
            {"download": False}, lambda message: None)
        self.stats = converter.run()
        self.converter = converter
        self.stage = Usd.Stage.Open(self.output)

    def test_root_metadata_and_sublayer(self):
        layer = self.stage.GetRootLayer()
        self.assertEqual(list(layer.subLayerPaths), ["./scene.usda"])
        self.assertEqual(self.stage.GetMetadata("metersPerUnit"), 0.01)
        self.assertEqual(self.stage.GetMetadata("upAxis"), "Y")

    def test_omni_pbr(self):
        s = surface(self.stage, "/World/Looks/Painted")
        self.assertEqual(s.GetShaderId(), omni.OPEN_PBR)
        self.assertAlmostEqual(value(s, "specular_roughness"), 0.25)
        self.assertAlmostEqual(value(s, "base_metalness"), 1.0)
        self.assertAlmostEqual(value(s, "specular_weight"), 0.5)
        tint = connected(s, "base_color")
        self.assertEqual(tint.GetShaderId(), "ND_multiply_color3")
        image = connected(tint, "in1")
        self.assertEqual(image.GetShaderId(), "ND_image_color3")
        self.assertEqual(value(image, "file").path, "./albedo.png")
        self.assertEqual(image.GetInput("file").GetAttr().GetColorSpace(),
                         "srgb_texture")
        place = connected(image, "texcoord")
        self.assertEqual(place.GetShaderId(), "ND_multiply_vector2")
        self.assertEqual(value(place, "in2"), Gf.Vec2f(2, 2))
        # The MDL network is kept.
        material = UsdShade.Material(self.stage.GetPrimAtPath(
            "/World/Looks/Painted"))
        self.assertTrue(material.GetSurfaceOutput("mdl").HasConnectedSource())

    def test_udim(self):
        select = connected(surface(self.stage, "/World/Looks/Tiled"),
                           "base_color")
        self.assertEqual(select.GetShaderId(), "ND_ifequal_color3")
        self.assertEqual(value(select, "value2"), 1002.0)
        self.assertEqual(value(connected(select, "in1"), "file").path,
                         "./tiles.1002.png")
        self.assertEqual(value(connected(select, "in2"), "file").path,
                         "./tiles.1001.png")
        self.assertFalse(self.converter.textures.missing)

    def test_binding_api_applied(self):
        mesh = self.stage.GetPrimAtPath("/World/Mesh")
        self.assertTrue(mesh.HasAPI(UsdShade.MaterialBindingAPI))
        bound, _ = UsdShade.MaterialBindingAPI(mesh).ComputeBoundMaterial()
        self.assertEqual(bound.GetPath(), "/World/Looks/Painted")

    def test_omni_glass(self):
        s = surface(self.stage, "/World/Looks/Glass")
        self.assertEqual(value(s, "transmission_weight"), 1.0)
        self.assertEqual(value(s, "transmission_color"), Gf.Vec3f(0.5, 0.9, 0.5))
        self.assertTrue(value(s, "geometry_thin_walled"))

    def test_uber_follows_variants(self):
        def base_color():
            return value(surface(self.stage, "/World/Looks/Paint"),
                         "base_color")
        red = base_color()
        self.assertGreater(red[0], red[2])
        vset = self.stage.GetPrimAtPath("/World/Looks").GetVariantSet("Color")
        vset.SetVariantSelection("Blue")
        blue = base_color()
        self.assertGreater(blue[2], blue[0])
        self.assertEqual(self.stats["variant networks"], 2)

    def test_unsupported_modules_are_reported(self):
        self.assertEqual(self.converter.unsupported["Chrome.mdl:Chrome"], 1)
        self.assertFalse(surface(self.stage, "/World/Looks/Unknown"))

    def test_roots(self):
        roots = convert.find_roots([self.dir])
        self.assertEqual(roots, [(os.path.normpath(self.source),
                                  os.path.abspath(self.dir))])


class TestInstances(unittest.TestCase):
    def test_instanced_assets_are_redirected(self):
        tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(tmp, "props"))
        asset_path = os.path.join(tmp, "props", "crate.usda")
        asset = Usd.Stage.CreateNew(asset_path)
        root = asset.DefinePrim("/Crate", "Xform")
        asset.SetDefaultPrim(root)
        mdl_material(asset, "/Crate/Looks/Wood", "OmniPBR.mdl", {
            "diffuse_color_constant": (V.Color3f, Gf.Vec3f(0.5, 0.3, 0.1)),
        })
        mesh = asset.DefinePrim("/Crate/Mesh", "Mesh")
        mesh.CreateRelationship("material:binding").SetTargets(
            ["/Crate/Looks/Wood"])
        asset.Save()

        scene_path = os.path.join(tmp, "scene.usda")
        scene = Usd.Stage.CreateNew(scene_path)
        for name in ("A", "B"):
            prim = scene.DefinePrim("/World/" + name)
            prim.GetReferences().AddReference("./props/crate.usda")
            prim.SetInstanceable(True)
        scene.Save()

        output = os.path.join(tmp, "scene_mtlx.usda")
        converter = convert.Converter(scene_path, output,
                                      os.path.join(tmp, "textures"),
                                      {"download": False}, lambda m: None)
        stats = converter.run()
        self.assertEqual(stats["instanced assets redirected"], 2)
        self.assertTrue(os.path.exists(
            os.path.join(tmp, "props", "crate_mtlx.usda")))

        stage = Usd.Stage.Open(output)
        self.assertEqual(len(stage.GetPrototypes()), 1)
        for name in ("A", "B"):
            prim = stage.GetPrimAtPath("/World/%s/Looks/Wood" % name)
            self.assertTrue(prim.IsInstanceProxy())
            s = surface(stage, prim.GetPath())
            self.assertEqual(s.GetShaderId(), omni.OPEN_PBR)
            mesh = stage.GetPrimAtPath("/World/%s/Mesh" % name)
            self.assertTrue(mesh.HasAPI(UsdShade.MaterialBindingAPI))


if __name__ == "__main__":
    unittest.main()
