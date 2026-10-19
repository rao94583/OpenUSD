# Materials and shading

A `UsdShade.Material` holds a network of `UsdShade.Shader` prims. Its
`outputs:surface` (UsdPreviewSurface, Storm's native model) or
`outputs:mtlx:surface` (MaterialX) connects to the network's surface shader.
Vela renders both in Storm; Embree shows base colors only.

## Inspecting

```python
stage = vela.stage
seat = stage.GetPrimAtPath('/Room/Furniture/Chair/Seat')
material, relationship = UsdShade.MaterialBindingAPI(seat).ComputeBoundMaterial()
print(material.GetPath() if material else 'no material')
if material:
    surface = material.ComputeSurfaceSource()[0]   # (shader, output name, type)
    if surface:
        print(surface.GetShaderId(), [(i.GetBaseName(), i.Get()) for i in surface.GetInputs()])
```

- `ComputeBoundMaterial()` resolves inherited and collection bindings, as
  renderers do; `GetDirectBinding()` reads only the prim's own binding.
- Bindings are inherited: a binding on a parent covers its children unless
  they have their own. "Which prims have no material" means gprims whose
  `ComputeBoundMaterial()` returns no material.
- `ComputeBoundMaterial(UsdShade.Tokens.preview)` asks for a purpose
  (`full` and `preview`); with none, the all-purpose binding.
- A shader's id is `info:id` (e.g. `UsdPreviewSurface`, `ND_standard_surface_surfaceshader`).
  An input that is connected has its value at the source:
  `input.GetConnectedSources()`, or `input.GetValueProducingAttributes()`.

## A Preview Surface material

```python
stage = vela.stage
material = UsdShade.Material.Define(stage, '/Room/Looks/Velvet')
shader = UsdShade.Shader.Define(stage, '/Room/Looks/Velvet/Surface')
shader.CreateIdAttr('UsdPreviewSurface')
shader.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.35, 0.05, 0.1))
shader.CreateInput('roughness', Sdf.ValueTypeNames.Float).Set(0.8)
material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), 'surface')

seat = stage.GetPrimAtPath('/Room/Furniture/Chair/Seat')
UsdShade.MaterialBindingAPI.Apply(seat).Bind(material)
```

- Always `Apply` the binding API before `Bind`: binding without it
  authors a relationship that renderers may ignore.
- Bind on the prim (or an ancestor) the user means: binding the chair's
  Xform covers its seat, back and legs, unless they have their own.
- UsdPreviewSurface inputs: `diffuseColor`, `emissiveColor`,
  `specularColor` (with `useSpecularWorkflow` 1), `metallic`, `roughness`,
  `clearcoat`, `clearcoatRoughness`, `opacity`, `opacityThreshold`, `ior`,
  `normal`, `displacement`, `occlusion`. Colors are `color3f`, the rest
  `float` (and `normal` a `normal3f`, `useSpecularWorkflow` an `int`).

## Textures

```python
stage = vela.stage
material = UsdShade.Material.Define(stage, '/Room/Looks/Printed')
surface = UsdShade.Shader.Define(stage, '/Room/Looks/Printed/Surface')
surface.CreateIdAttr('UsdPreviewSurface')
material.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), 'surface')

reader = UsdShade.Shader.Define(stage, '/Room/Looks/Printed/UV')
reader.CreateIdAttr('UsdPrimvarReader_float2')
reader.CreateInput('varname', Sdf.ValueTypeNames.Token).Set('st')

texture = UsdShade.Shader.Define(stage, '/Room/Looks/Printed/Color')
texture.CreateIdAttr('UsdUVTexture')
texture.CreateInput('file', Sdf.ValueTypeNames.Asset).Set('./textures/print.png')
texture.CreateInput('sourceColorSpace', Sdf.ValueTypeNames.Token).Set('sRGB')
texture.CreateInput('st', Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), 'result')
texture.CreateOutput('rgb', Sdf.ValueTypeNames.Float3)
surface.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).ConnectToSource(texture.ConnectableAPI(), 'rgb')
```

- The mesh needs texture coordinates: a `texCoord2f[]` primvar, usually
  `primvars:st`. Check `UsdGeom.PrimvarsAPI(mesh).HasPrimvar('st')`;
  Vela's example cubes and spheres have none, so textures on them show as
  one color.
- `sourceColorSpace` is `sRGB` for color images and `raw` for data
  (normals, roughness). Normal maps also need `scale` (2,2,2,1) and `bias`
  (-1,-1,-1,0).
- Asset paths are relative to the layer they're authored in. Prefer a path
  relative to the edit target's layer, next to the scene, over an absolute
  one.

## MaterialX

A MaterialX material connects `outputs:mtlx:surface` to a shader whose id
is a MaterialX node definition, such as `ND_standard_surface_surfaceshader`
or `ND_open_pbr_surface_surfaceshader`:

```python
stage = vela.stage
material = UsdShade.Material.Define(stage, '/Room/Looks/Lacquer')
shader = UsdShade.Shader.Define(stage, '/Room/Looks/Lacquer/Surface')
shader.CreateIdAttr('ND_standard_surface_surfaceshader')
shader.CreateInput('base_color', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.6, 0.1, 0.05))
shader.CreateInput('specular_roughness', Sdf.ValueTypeNames.Float).Set(0.15)
material.CreateSurfaceOutput('mtlx').ConnectToSource(shader.ConnectableAPI(), 'out')
```

The node definitions and their inputs are in Sdr:
`Sdr.Registry().GetShaderNodeByIdentifier('ND_standard_surface_surfaceshader').GetShaderInputNames()`
(after `from pxr import Sdr`). Keep the shading model a material already
uses when editing it, rather than replacing a MaterialX network with a
Preview Surface.

## Changing an existing material

Edit the shader's inputs, or the input's source if it's connected. To give
some prims a different look without touching others that share the
material, make a new material (or `Sdf.CopySpec` the old one to a new path
in the edit target's layer) and bind that.
