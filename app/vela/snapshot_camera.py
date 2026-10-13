"""Write a layer that adds a camera framing a scene, for scenes without one.

Run with the bundled interpreter: snapshot_camera.py RUNTIME SCENE OUTPUT
"""
import sys

runtime, scene, output = sys.argv[1:4]
sys.path.insert(0, runtime + '/usd/lib/python')
from pxr import Gf, Sdf, Usd, UsdGeom

stage = Usd.Stage.Open(scene)
zUp = UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z
bounds = UsdGeom.BBoxCache(Usd.TimeCode.Default(), ['default', 'proxy'], True)
bbox = bounds.ComputeWorldBound(stage.GetPseudoRoot())
center = bbox.ComputeCentroid()
radius = bbox.ComputeAlignedRange().GetSize().GetLength() / 2

# A three-quarter view from above, far enough for a 50mm lens to fit it all.
direction = Gf.Vec3d(1, -1.3, 0.8) if zUp else Gf.Vec3d(1, 0.8, 1.3)
up = Gf.Vec3d(0, 0, 1) if zUp else Gf.Vec3d(0, 1, 0)
distance = 3 * radius
eye = center + direction.GetNormalized() * distance

layer = Sdf.Layer.CreateNew(output)
layer.subLayerPaths.append(scene)
wrapper = Usd.Stage.Open(layer)
UsdGeom.SetStageUpAxis(wrapper, UsdGeom.GetStageUpAxis(stage))
camera = UsdGeom.Camera.Define(wrapper, '/SnapshotCamera')
camera.AddTransformOp().Set(
    Gf.Matrix4d().SetLookAt(eye, center, up).GetInverse())
camera.CreateClippingRangeAttr(
    Gf.Vec2f(max(distance - radius, 0.01) * 0.5, (distance + radius) * 1.5))
layer.Save()
