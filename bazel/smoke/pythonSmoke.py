"""Imports every pxr/base Python module and exercises Tf, Gf and Vt."""

import unittest


class TestPxrBase(unittest.TestCase):
    def test_imports(self):
        import pxr
        from pxr import Tf, Gf, Vt, Work, Plug, Trace, Ts
        # pxr/base modules come first, in CMake build order.
        self.assertEqual(
            pxr.__all__[:7], ["Tf", "Gf", "Trace", "Work", "Plug", "Vt", "Ts"])
        for module in (Tf, Gf, Vt, Work, Plug, Trace, Ts):
            self.assertTrue(module.__file__.endswith("__init__.py"))

    def test_tf(self):
        from pxr import Tf
        self.assertEqual(Tf.MakeValidIdentifier("a b"), "a_b")
        self.assertEqual(Tf.Type.FindByName("TfType").typeName, "TfType")
        with self.assertRaises(Tf.ErrorException):
            Tf.RaiseCodingError("expected")

    def test_gf(self):
        from pxr import Gf
        v = Gf.Vec3f(1, 2, 3)
        self.assertEqual(v * 2, Gf.Vec3f(2, 4, 6))
        self.assertAlmostEqual(Gf.Vec3d(3, 4, 0).GetLength(), 5.0)
        m = Gf.Matrix4d().SetTranslate(Gf.Vec3d(1, 2, 3))
        self.assertEqual(m.Transform(Gf.Vec3d(0, 0, 0)), Gf.Vec3d(1, 2, 3))

    def test_vt(self):
        from pxr import Gf, Vt
        a = Vt.FloatArray([1.0, 2.0, 3.0])
        self.assertEqual(len(a), 3)
        self.assertEqual(list(a * 2), [2.0, 4.0, 6.0])
        points = Vt.Vec3fArray([Gf.Vec3f(1, 2, 3)])
        self.assertEqual(points[0], Gf.Vec3f(1, 2, 3))

    def test_others(self):
        from pxr import Plug, Trace, Ts, Work
        self.assertGreater(Work.GetConcurrencyLimit(), 0)
        self.assertIsNotNone(Plug.Registry())
        self.assertIsNotNone(Trace.Collector())
        spline = Ts.Spline()
        spline.SetKnot(Ts.Knot(time=1.0, value=2.0))
        self.assertEqual(spline.Eval(1.0), 2.0)

    def test_single_registry(self):
        # Types registered by the library must be visible to every module:
        # one copy of each registry, shared through the monolithic library.
        from pxr import Tf, Gf, Vt
        self.assertTrue(Tf.Type.FindByName("GfVec3f"))
        self.assertTrue(Tf.Type.FindByName("VtValue"))


if __name__ == "__main__":
    unittest.main()
