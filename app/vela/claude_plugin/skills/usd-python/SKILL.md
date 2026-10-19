---
name: usd-python
description: How to query and edit the open USD scene correctly with the pxr Python API in Vela (run_python, query_python). Use before writing any non-trivial pxr code - finding prims by type, editing attributes and time samples, transforms, lights, materials and bindings, visibility, variants, references, instancing, renaming or deleting prims - and to look up a schema's attributes, fallbacks and getters in this build's schema reference.
---

# USD Python in Vela

The code you pass to `run_python` and `query_python` runs inside Vela,
against the stage the window shows. `vela.stage` is that `Usd.Stage`;
`Usd`, `Sdf`, `Gf`, `Vt`, `Tf`, `Kind`, `UsdGeom`, `UsdShade`, `UsdLux` and
`UsdUtils` are already imported, and any other `pxr` module can be. The
namespace persists between calls. This build is USD 0.27.

- **Ask with `query_python`, edit with `run_python`.** A query that edits
  anything is reverted.
- **One undo step per request.** Everything you change while answering one
  user message undoes as one step, so make the edit rather than asking for
  confirmation, and don't try to clean up afterwards with your own undo.
- **Don't save** (`Save()`, `Export()`) unless the user asks; Vela tracks
  unsaved changes and asks when the window closes. Don't open another
  stage to make an edit: the window would not show it, and undo would not
  cover it.
- **Author to the edit target** (`vela.stage.GetEditTarget()`), which the
  user chooses; `get_app_state` reports it. Undo covers every layer the
  stage uses, but not a layer you create yourself with `Sdf.Layer`.
- **Print what you found**, not whole objects: the result is text, and the
  panel is small.

## Schema reference

`references/schemas/index.md` lists every schema in this build (typed and
API) with its file; each library's file (`UsdGeom.md`, `UsdLux.md`,
`UsdShade.md`, ...) gives a schema's own attributes with their types,
fallbacks, allowed tokens and Python getters. Read it (or Grep it for
`^## RectLight`) before guessing an attribute's name or type. Getters are
`Get<Name>Attr()`, creators `Create<Name>Attr(value)`; a schema's
inherited attributes are under its bases.

For shading networks (Preview Surface, MaterialX, textures) see
`references/shading.md`. For variants, references, payloads, instancing,
renaming and deleting see `references/composition.md`.

## Finding prims

```python
stage = vela.stage
# Typed schemas: IsA. Applied API schemas: HasAPI. Every light, including
# mesh lights, has LightAPI.
meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Gprim)]
lights = [p for p in stage.Traverse() if p.HasAPI(UsdLux.LightAPI)]
print(len(meshes), 'gprims;', [str(p.GetPath()) for p in lights])
```

- `stage.Traverse()` skips inactive, unloaded and abstract prims (classes)
  and doesn't enter instances. For instance proxies too, use
  `Usd.PrimRange.Stage(stage, Usd.TraverseInstanceProxies())`.
- `stage.GetPrimAtPath(path)` returns an invalid prim, not `None`, when
  nothing is there: test it with `if prim:` or `prim.IsValid()`.
- The user's selection is `vela.selection` (a list of path strings);
  `vela.time` is the current time. Assigning either changes the window.
- `prim.GetTypeName()` is the type's name (`'Mesh'`);
  `prim.GetAppliedSchemas()` lists applied APIs.

## Reading and writing values

```python
stage = vela.stage
light = UsdLux.SphereLight(stage.GetPrimAtPath('/Room/Lights/Ceiling'))
intensity = light.GetIntensityAttr()
print(intensity.Get(), intensity.GetTypeName(), intensity.HasAuthoredValue())
intensity.Set(intensity.Get() * 1.5)
# Values must match the attribute's type: Gf types for vectors and colors.
light.CreateColorAttr(Gf.Vec3f(1.0, 0.9, 0.8))
```

- `attr.Get()` with no time reads the default value, or at the earliest
  sample. Pass `vela.time` (or a `Usd.TimeCode`) to read what the user sees.
- **Time samples win over defaults.** If `attr.GetNumTimeSamples()` is
  non-zero, `attr.Set(v)` changes a default the user never sees. Set the
  value at the samples' times (`attr.Set(v, t)` for each of
  `attr.GetTimeSamples()`), or ask whether to replace the animation.
- **A stronger opinion can hide yours.** If a value didn't change, compare
  `attr.GetResolveInfo().GetSource()` and `attr.GetPropertyStack()`: a
  stronger layer (the session layer, a stronger sublayer) or a variant may
  hold the opinion, or the edit target may sit below it.
- Arrays (`Vt.Vec3fArray`, `Vt.IntArray`, ...) come back as copies: change
  the copy and `Set` it back.
- `attr.Clear()` removes the opinion in the edit target, returning to
  weaker opinions or the fallback; `attr.Block()` blocks every weaker one.
- Create attributes that aren't in a schema with
  `prim.CreateAttribute('name', Sdf.ValueTypeNames.Float)`; tokens are
  plain strings.

## Transforms and bounds

```python
stage = vela.stage
chair = stage.GetPrimAtPath('/Room/Furniture/Chair')
xform = UsdGeom.XformCommonAPI(chair)
translate, rotate, scale, pivot, order = xform.GetXformVectors(Usd.TimeCode.Default())
xform.SetTranslate(translate + Gf.Vec3d(0, 0, 20))
xform.SetRotate(Gf.Vec3f(0, 45, 0))
world = UsdGeom.Xformable(chair).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.render])
box = cache.ComputeWorldBound(chair).ComputeAlignedRange()
print(world.ExtractTranslation(), box.GetMin(), box.GetMax())
```

- `XformCommonAPI` works only when the prim's existing ops fit its
  translate-pivot-rotate-scale pattern (`GetXformVectors` fails otherwise).
  If it fails, use `UsdGeom.Xformable(prim).GetOrderedXformOps()` and edit
  those ops, or add one: `AddTranslateOp(opSuffix='offset')`.
- Units and axes vary: check `UsdGeom.GetStageUpAxis(stage)` and
  `UsdGeom.GetStageMetersPerUnit(stage)` before using sizes like "2
  meters".
- Bounds need purposes: pass `default_` and `render` (or `proxy`) as above.

## Lights

```python
stage = vela.stage
# "Warmer": use color temperature, or tint the color toward orange.
for prim in stage.Traverse():
    if prim.HasAPI(UsdLux.LightAPI):
        light = UsdLux.LightAPI(prim)
        light.CreateEnableColorTemperatureAttr(True)
        kelvin = light.GetColorTemperatureAttr().Get() or 6500
        light.CreateColorTemperatureAttr(kelvin * 0.8)
print(UsdLux.BlackbodyTemperatureAsRgb(5200))
```

- Brightness is `intensity × 2^exposure`. Prefer `exposure` for "a stop
  brighter".
- Color temperature multiplies `color`; it applies only while
  `enableColorTemperature` is on.
- `UsdLux.DomeLight` lights the environment (`texture:file` is a lat-long
  image); `RectLight`, `DiskLight`, `SphereLight`, `CylinderLight` and
  `DistantLight` have shapes in their own attributes.

## Visibility, activation and purpose

```python
stage = vela.stage
plant = stage.GetPrimAtPath('/Room/Decor/Shelf/Plant')
imageable = UsdGeom.Imageable(plant)
print(imageable.ComputeVisibility(), imageable.ComputePurpose())
# Why is it hidden? Visibility inherits: find the invisible ancestor.
prim = plant
while prim and not prim.IsPseudoRoot():
    if UsdGeom.Imageable(prim).GetVisibilityAttr().Get() == UsdGeom.Tokens.invisible:
        print('invisible:', prim.GetPath())
    prim = prim.GetParent()
imageable.MakeVisible()       # also makes its ancestors visible
```

- `MakeVisible()`/`MakeInvisible()` are the right way to toggle; setting
  `visibility` to `inherited` on the prim alone doesn't help when an
  ancestor is `invisible`.
- `prim.SetActive(False)` removes a prim and its subtree from the composed
  scene without deleting anything; it's the non-destructive "delete".
- Purposes `proxy`, `render` and `guide` show only when the viewport's
  matching purpose is on; `default` always shows.
- A prim can be hidden by other means: inactive, unloaded payload, a
  `guide` purpose, zero scale, or a material with zero opacity.

## Primvars

```python
stage = vela.stage
seat = stage.GetPrimAtPath('/Room/Furniture/Chair/Seat')
primvars = UsdGeom.PrimvarsAPI(seat)
color = primvars.CreatePrimvar('displayColor', Sdf.ValueTypeNames.Color3fArray, UsdGeom.Tokens.constant)
color.Set(Vt.Vec3fArray([Gf.Vec3f(0.8, 0.2, 0.2)]))
print([p.GetName() for p in primvars.GetPrimvarsWithValues()])
```

- Interpolation (`constant`, `uniform`, `varying`, `vertex`,
  `faceVarying`) decides how many values a primvar needs.
- `displayColor` shows only when no material is bound (or in the
  viewport's no-material modes).

## Many edits at once

`Sdf.ChangeBlock` batches notices, which makes thousands of edits fast. Use
only `Sdf` API inside it (layers, specs), not `Usd` API, which may read
composed state that isn't updated yet:

```python
stage = vela.stage
UsdGeom.Xform.Define(stage, '/Room/Scatter')   # parents first, with Usd API
layer = stage.GetEditTarget().GetLayer()
with Sdf.ChangeBlock():
    for i in range(100):
        spec = Sdf.CreatePrimInLayer(layer, '/Room/Scatter/Pebble%d' % i)
        spec.specifier = Sdf.SpecifierDef
        spec.typeName = 'Sphere'
print(len(stage.GetPrimAtPath('/Room/Scatter').GetChildren()))
```

`Sdf.CreatePrimInLayer` writes to the layer directly, ignoring the edit
target's path mapping (for example inside a variant), so use it with the
root layer stack's targets. It makes missing parents `over`s, and a prim
under an `over` with no `def` anywhere isn't defined and doesn't render:
define parents first, or set each new spec's `specifier`.

## Pitfalls

- Prim and property names must be valid identifiers (`Tf.MakeValidIdentifier`).
- `Sdf.Path` vs strings: most APIs take either; compare with `str(path)`.
- Edits to instance proxies fail: edit the prototype's source (the
  referenced asset or class) or make the instance uninstanceable
  (`prim.SetInstanceable(False)`), which may cost memory.
- `UsdShade` and `UsdLux` inputs are attributes named `inputs:<name>`; the
  `UsdShade.Input` API (`shader.GetInput('diffuseColor')`) handles
  connections too.
- Don't call `stage.Reload()` or `vela.stage.GetRootLayer().Clear()`;
  reverting is the user's choice (File → Revert).
