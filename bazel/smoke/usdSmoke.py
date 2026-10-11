"""Opens, edits and saves stages; checks plugins found through plugInfo.json."""

import os
import tempfile
import unittest


class TestUsd(unittest.TestCase):
    def test_in_memory_stage(self):
        from pxr import Sdf, Usd, UsdGeom
        stage = Usd.Stage.CreateInMemory()
        sphere = UsdGeom.Sphere.Define(stage, "/hello/world")
        self.assertTrue(sphere)
        # The fallback comes from usdGeom's generatedSchema.usda under
        # lib/usd/usdGeom/resources.
        self.assertEqual(sphere.GetRadiusAttr().Get(), 1.0)
        sphere.GetRadiusAttr().Set(2.0)
        text = stage.GetRootLayer().ExportToString()
        self.assertIn("double radius = 2", text)
        self.assertEqual(
            stage.GetPrimAtPath("/hello/world").GetTypeName(), "Sphere")
        self.assertEqual(Sdf.Path("/hello/world").name, "world")

    def test_file_formats(self):
        from pxr import Sdf, Usd, UsdGeom
        with tempfile.TemporaryDirectory() as tmp:
            for ext in ("usda", "usdc", "usd"):
                path = os.path.join(tmp, "stage." + ext)
                stage = Usd.Stage.CreateNew(path)
                UsdGeom.Xform.Define(stage, "/root")
                stage.GetRootLayer().Save()
                reopened = Usd.Stage.Open(path)
                self.assertTrue(reopened.GetPrimAtPath("/root"))
            self.assertIn("usdz", Sdf.FileFormat.FindAllFileFormatExtensions())

    def test_core_plugin(self):
        # usdShaders is a pxr_plugin; its resources live under plugin/usd.
        from pxr import Plug, Sdr
        self.assertTrue(Plug.Registry().GetPluginWithName("usdShaders"))
        node = Sdr.Registry().GetShaderNodeByIdentifier("UsdPreviewSurface")
        self.assertTrue(node)
        self.assertIn("diffuseColor", node.GetShaderInputNames())

    def test_schema_libraries(self):
        from pxr import Usd, UsdLux, UsdShade, UsdSkel
        stage = Usd.Stage.CreateInMemory()
        light = UsdLux.SphereLight.Define(stage, "/light")
        self.assertTrue(light.GetPrim().HasAPI(UsdLux.LightAPI))
        self.assertTrue(UsdShade.Material.Define(stage, "/mat"))
        self.assertTrue(UsdSkel.Root.Define(stage, "/skel"))


if __name__ == "__main__":
    unittest.main()
