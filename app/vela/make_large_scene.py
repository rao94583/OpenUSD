"""Write a large scene for viewport benchmarks: a grid of distinct meshes.

Run with the bundled interpreter:
    make_large_scene.py RUNTIME OUTPUT.usdc [--grid N] [--segments S]
Each of the N x N prims is a sphere with S x S/2 quads, each its own Mesh
(not instanced), with a display color.
"""
import argparse
import math
import sys

parser = argparse.ArgumentParser()
parser.add_argument('runtime')
parser.add_argument('output')
parser.add_argument('--grid', type=int, default=150)
parser.add_argument('--segments', type=int, default=32)
args = parser.parse_args()
sys.path.insert(0, args.runtime + '/usd/lib/python')
from pxr import Gf, Usd, UsdGeom, Vt

u, v = args.segments, args.segments // 2
points = []
for j in range(v + 1):
    phi = math.pi * j / v
    for i in range(u):
        theta = 2 * math.pi * i / u
        points.append(Gf.Vec3f(0.4 * math.sin(phi) * math.cos(theta),
                               0.4 * math.cos(phi),
                               0.4 * math.sin(phi) * math.sin(theta)))
counts, indices = [], []
for j in range(v):
    for i in range(u):
        a, b = j * u + i, j * u + (i + 1) % u
        counts.append(4)
        indices += [a, a + u, b + u, b]
points = Vt.Vec3fArray(points)
counts = Vt.IntArray(counts)
indices = Vt.IntArray(indices)
extent = Vt.Vec3fArray([Gf.Vec3f(-0.4), Gf.Vec3f(0.4)])

stage = Usd.Stage.CreateNew(args.output)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
stage.SetDefaultPrim(UsdGeom.Xform.Define(stage, '/World').GetPrim())
for x in range(args.grid):
    row = UsdGeom.Xform.Define(stage, f'/World/Row{x}')
    for z in range(args.grid):
        mesh = UsdGeom.Mesh.Define(stage, f'/World/Row{x}/Sphere{z}')
        # Distinct point data per prim, so nothing is shared or instanced.
        offset = Gf.Vec3f(x, 0.1 * math.sin(x * 0.3 + z * 0.2), z)
        mesh.CreatePointsAttr(Vt.Vec3fArray([p + offset for p in points]))
        mesh.CreateFaceVertexCountsAttr(counts)
        mesh.CreateFaceVertexIndicesAttr(indices)
        mesh.CreateExtentAttr(Vt.Vec3fArray([e + offset for e in extent]))
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.CreateDisplayColorAttr(Vt.Vec3fArray(
            [Gf.Vec3f(x / args.grid, 0.5, z / args.grid)]))
stage.GetRootLayer().Save()
quads = args.grid * args.grid * u * v
print(f'{args.grid * args.grid} meshes, {2 * quads / 1e6:.1f}M triangles')
