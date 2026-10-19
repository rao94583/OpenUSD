# Composition: variants, references, instancing, namespace edits

## Variants

```python
stage = vela.stage
chair = stage.GetPrimAtPath('/Room/Furniture/Chair')
fabric = chair.GetVariantSets().AddVariantSet('fabric')
looks = {'linen': '/Room/Looks/Linen', 'velvet': '/Room/Looks/Fabric'}
parts = [stage.GetPrimAtPath(chair.GetPath().AppendChild(n)) for n in ('Seat', 'Back')]
for prim in parts:
    # A direct binding outside the variants would beat them: remove it.
    # (UnbindDirectBinding authors an empty binding, which beats them too.)
    UsdShade.MaterialBindingAPI(prim).GetDirectBindingRel().ClearTargets(True)
for name, path in looks.items():
    fabric.AddVariant(name)
    fabric.SetVariantSelection(name)
    with fabric.GetVariantEditContext():
        # Authored inside the variant: only applies while it's selected.
        for prim in parts:
            UsdShade.MaterialBindingAPI.Apply(prim).Bind(UsdShade.Material(stage.GetPrimAtPath(path)))
fabric.SetVariantSelection('linen')
bound = UsdShade.MaterialBindingAPI(parts[0]).ComputeBoundMaterial()[0]
assert bound.GetPath() == '/Room/Looks/Linen', bound.GetPath()
```

- Opinions outside the variant (in the edit target directly) are stronger
  than the variant's: a direct binding on the seat would win over every
  variant. Move such opinions into the variants, as above, and say so.
  Clearing removes the edit target's opinion only; check
  `GetPropertyStack()` for opinions in other layers.
- `GetVariantEditContext()` targets the variant in the current edit
  target's layer; the selection must be set to the variant you're
  authoring.
- The selection is itself an opinion in the edit target. Querying each
  variant by changing the selection is an edit (`query_python` reverts
  it); to compare variants without editing, read the variant's specs in the
  layer (`layer.GetPrimAtPath(prim.GetPath().AppendVariantSelection('fabric', 'linen'))`).
- The `set_variant` Vela action changes a selection as one undo step too.

## References and payloads

```python
stage = vela.stage
prop = stage.DefinePrim('/Room/Decor/Lamp2', 'Xform')
prop.GetReferences().AddInternalReference('/Room/Furniture/FloorLamp')
# A file: prop.GetReferences().AddReference('./props/lamp.usda', '/Lamp')
print([c.GetName() for c in prop.GetChildren()])
```

- Asset paths are relative to the layer that holds the reference; prefer
  relative paths next to the scene.
- Payloads (`GetPayloads().AddPayload(...)`) behave like references but
  can be unloaded; `prim.Load()`/`Unload()`, or Vela's `load_selection`
  and `unload_selection` actions. Unloaded prims are skipped by
  `Traverse()`.
- What a prim is composed from: `prim.GetPrimStack()` (strongest first) or
  `prim.GetPrimIndex().DumpToString()` for the full index.
- `inherits` (`prim.GetInherits().AddInherit('/_class_Chair')`) and
  `specializes` broadcast opinions from a class prim (`stage.CreateClassPrim`).

## Instancing

- `prim.IsInstance()` marks an instanced prim; its descendants are
  instance proxies (`IsInstanceProxy()`), read-only and skipped by plain
  `Traverse()`.
- `prim.GetPrototype()` is the shared prototype; edits go to the source
  that it's composed from (the referenced asset or the class), and affect
  every instance.
- `prim.SetInstanceable(False)` breaks one instance out for local edits.
- `UsdGeom.PointInstancer` instances by index: `GetProtoIndicesAttr()`,
  `GetPositionsAttr()`, `GetPrototypesRel()`; `ComputeInstanceTransformsAtTime`.

## Renaming, moving and deleting

```python
stage = vela.stage
editor = Usd.NamespaceEditor(stage)
editor.RenamePrim(stage.GetPrimAtPath('/Room/Decor/Book'), 'Novel')
result = editor.CanApplyEdits()
if result:
    editor.ApplyEdits()
else:
    print('cannot rename:', result.whyNot)
print(bool(stage.GetPrimAtPath('/Room/Decor/Novel')))
```

- `Usd.NamespaceEditor` renames, reparents (`ReparentPrim`,
  `MovePrimAtPath`) and deletes (`DeletePrimAtPath`) across the layer
  stack and fixes targets and connections. Check `CanApplyEdits()` and
  report its reason when it fails.
- `stage.RemovePrim(path)` deletes only the edit target's spec; a prim
  defined in a weaker layer or a reference survives. To hide a prim from
  any source, deactivate it: `prim.SetActive(False)`.

## Layers and the edit target

```python
stage = vela.stage
for layer in stage.GetLayerStack(includeSessionLayers=False):
    print(layer.identifier, layer.dirty)
print('edit target:', stage.GetEditTarget().GetLayer().identifier)
```

- The user picks the edit target (in the Layers tab, or Vela's
  `set_edit_target` action). Don't change it to make an edit go through
  unless they asked; explain where the edit went instead.
- `Usd.EditContext(stage, target)` changes it for a `with` block only, for
  example to author to the session layer (`stage.GetSessionLayer()`) for a
  view-only change.
- `stage.GetMutedLayers()`: a muted layer's opinions don't apply.
