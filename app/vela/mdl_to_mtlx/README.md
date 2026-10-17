# mdl_to_mtlx

Adds MaterialX (OpenPBR Surface) versions of NVIDIA Omniverse MDL materials
to USD scenes, so Storm, and so Vela and usdview, can render NVIDIA's
assets. RTX shading is approximated, not reproduced.

```sh
bazel run //app/vela/mdl_to_mtlx -- SCENE.usd            # one scene
bazel run //app/vela/mdl_to_mtlx -- DIR [DIR...]         # every root layer
bazel run //app/vela/mdl_to_mtlx -- DIR --list           # just list them
bazel run //app/vela/mdl_to_mtlx:census -- DIR --inputs 40
bazel test //app/vela/mdl_to_mtlx:convert_test
```

For each root layer the converter writes `NAME_mtlx.usda` beside it; open
that file. It sublayers the original and adds, under each MDL material, a
`MaterialX` scope of nodes and an `outputs:mtlx:surface` output. The MDL
output stays, so RTX still uses the original. A directory's root layers are
the USD files no other file in the same tree references.

Materials that variant sets in the stage change (car paint colors, for
example) are converted in each variant of that set and written inside the
same variant of the output layer, so switching variants changes the
MaterialX network too. `--no-variants` converts only the current selections.

Remote textures (NVIDIA's are on `omniverse-content-production` S3) are
downloaded to `mdl_textures/` and referenced by relative path: beside the
output for a file argument, or at the top of a directory argument, shared by
its scenes. Relative texture paths that do not resolve locally are resolved
against the MDL module's URL, as MDL does; UDIM templates are anchored to
the layer that authored them. `--no-download` writes the paths without
fetching. Automotive textures are large: each Ragnarok's are 5-6 GB of 4K
PNGs. Roots with no MDL materials get no output.

Opinions beneath an instance are ignored, so materials inside instances
(IsaacWarehouse has 887 instances of 60 prototypes) are converted in the
referenced asset instead: it gets its own `ASSET_mtlx.usda`, converted the
same way (recursively, for nested instances), and the output layer points
the outermost instance's reference or payload at it. Instances of one asset
still share a prototype.

Older Omniverse exports bind materials without applying
`MaterialBindingAPI`, and current USD ignores such bindings; the output
layer applies the schema to those prims (in the default composition and in
each variant).

Storm does not load UDIM files in MaterialX networks, so a UDIM texture
becomes one image node per tile present, selected by the tile that
`floor(uv)` falls in.

Storm loads MaterialX textures raw and MaterialX applies the color
transform, so USD color spaces become MaterialX ones: `sRGB` is
`srgb_texture`, `raw` is none, and `auto` is `srgb_texture` for 8-bit
images and `lin_rec709` otherwise (the PNG header gives the bit depth).
MaterialX's `triplanarprojection` makes a cycle when its files have a
color space, so triplanar files are read raw and followed by
`srgb_texture_to_lin_rec709`.

## Coverage

`census` counts the MDL modules in a tree. In the Showcases and Residential
packs, four modules are 94% of the 2,227 MDL shaders, and the converter
handles them:

| Module | Shaders | Translation |
|---|---|---|
| OmniPBR (and _ClearCoat, _Opacity) | 1,216 | close: albedo adjustments, ORM, roughness and metallic influence, `specular_level`, emission, cutout, normal map, UV transform |
| OmniUber_Automotive | 700 | approximate: three blended layers, coat, flakes averaged into the base |
| OmniGlass | 155 | transmission with IOR, color, frosting roughness, cutout |
| OmniSurface | 29 | close: OmniSurface follows Standard Surface, which OpenPBR maps directly |

The rest are about 80 one-off library presets (`Chrome.mdl`,
`Carpaint_01.mdl`, ...) each used once or twice; the converter reports them
as unsupported and Storm draws those materials with its fallback.

## Approximations

- Emission: MDL emission is physical and RTX exposes it; Storm adds emission
  unexposed. Intensities are divided by `--emission-reference` (40, OmniPBR's
  default) and capped at 1, which keeps the hue of bright emitters.
- Normal maps: NVIDIA's defaults (`flip_tangent_v`, `normal_flip_green`)
  are read as OpenGL normal maps; only a non-default flag flips a channel.
- OmniUber_Automotive: triplanar lookups use MaterialX `triplanarprojection`
  at NVIDIA's scale (object position x `uv_scale` / 100) without the offset;
  triplanar normal maps, detail normals under a base normal, anisotropy
  direction and emission temperature are dropped; flakes become their
  average (coverage `flakes_density`^2 of `flake_color`, metallic,
  `flake_roughness`) where the coat is; axis blends use object space.
- OmniGlass: volume absorption (`depth`) is dropped.
- OmniPBR: detail normal maps and cubic projection (approximated as
  triplanar) are rare and simplified.

Each run prints the approximations it made, with counts, and textures it
could not find.
