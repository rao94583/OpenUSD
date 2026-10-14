# Vela

Native macOS USD viewer (Phase 4 browsing is in progress). Requires macOS 13+, Xcode and
the repository's Bazel toolchain. It opens `.usd`, `.usda`, `.usdc` and
`.usdz` through `NSDocument` (File → Open, Finder) and shows the stage in a
Storm viewport rendered with Metal.

```sh
bazel build //app/vela:archive
bazel test //app/vela:all                  # core, menus, bundle, snapshots
bazel test //app/vela:benchmark_test --test_output=all   # manual
```

## Viewport

Navigation matches Autodesk Maya:

| Input | Action |
|---|---|
| Option + left drag | Tumble around the center of interest |
| Option + middle drag, or Option + Command + left drag | Track |
| Option + right drag, or Option + left + middle drag | Dolly |
| Scroll wheel, pinch | Dolly |
| Option + two-finger drag | Track |
| Click, Shift-click | Select, toggle selection |
| F / A | Frame selection / frame all |
| ← / → / Space | Previous frame / next frame / play |

The timeline bar (play, frame slider) appears for stages with an authored
time code range. The status line shows the prim count, the selection and
the last frame's time.

Rendering matches usdrecord's defaults (camera light, sRGB color correction,
default and proxy purposes), with usdview's grey background and visible dome
light textures in the window. The free camera's clipping planes are fitted
to the stage bounds.

## Browsing

The prim outline opens branches lazily and shows prim type, inherited
visibility and model draw mode. Search matches paths or types, keeping the
ancestors of matching prims. Inactive prims and instance proxies are included.
Tree selection and viewport picking share the same document selection.

The inspector tabs show Properties, Metadata, Composition, Layers and Variants.
Double-click a property for paginated array elements, relationship targets,
time samples or spline knots and tangent slopes; its Metadata and Opinions
tabs show property metadata and authoring across layers. Filter properties by
name or type in the inspector. Double-click Layers or Composition rows for
layer metadata and sublayer details. Large arrays show their count
in the property list. The inspector follows selection and time; multiple
selection inspects the last selected path. These views are read-only.

```sh
bazel test //app/vela:browser_test //app/vela:browser_ui_test
Vela --check-browser FILE           # controls/selection test; no shown window
```

Phase 4 still needs composition graph/navigation, graphical spline
curves, further filters and full array-type support. The layer tab currently
shows the local layer stack; referenced layer opinions appear in Composition.

## Actions and undo

Every menu item and shortcut runs a named action from the registry in
`core/standard_actions.cpp` and `core/editing_actions.cpp`; `--describe-actions` lists them with their
parameter schemas. Changes to the stage are undoable across all its
layers (Edit → Undo, ⌘Z), one step per action.

The Prim menu hides/shows, activates/deactivates and loads/unloads the
selection. Attribute, variant, purpose and model draw mode authoring actions
are available in the registry; the current inspector remains read-only.
Edits currently change in-memory layers; saving/export and close prompts are
pending. See [PHASE5.md](PHASE5.md) for the parity checklist and supported
attribute types.

## Command line

```sh
Vela --check-document FILE          # print the prim count
Vela --describe-actions             # the action registry, as JSON
Vela --describe-menus               # the main menu, as text
Vela --snapshot FILE OUT.png [--camera PATH] [--frame T] [--width W] [--height H]
Vela --benchmark FILE [--frames N] [--width W] [--height H]   # offscreen
Vela --benchmark FILE --window [--frames N]                   # on screen
```

`--snapshot` writes exactly what the viewport draws; with `--camera` it
sizes the image as usdrecord does. `snapshot_test` renders the same views
with usdrecord and compares them with `idiff`: AnimCube at three frames, a
natively instanced scene and three UsdPreviewSurface scenes (textures,
normal maps, dome light). `make_large_scene.py` writes the benchmark scene:
22,500 distinct meshes, 23M triangles.

## Bundle

Extract `bazel-bin/app/vela/archive.zip` with Archive Utility or `ditto -x -k`,
then open `Vela.app`. The app can be moved independently of the checkout.
The local bundle identifier is `org.openusd.vela`; release branding,
notarization and distribution signing remain open decisions.

The bundle contains the existing monolithic USD install layout and the
hermetic Python 3.11 runtime under `Contents/Resources/Runtime`. The native
app and Python extensions link the same bundled `libusd_ms.dylib`. A
compilation-only header interface prevents statically linking another copy
of USD into the app. Runtime copies dereference Bazel symlinks; nested
Mach-O files are ad-hoc signed before the outer app is signed.

The smoke test extracts the signed archive into a temporary directory with
spaces, removes Python/USD/dyld environment overrides, verifies signatures,
and runs the bundled interpreter in isolated mode. It checks each plugin
manifest against the registry, loads every plugin, and creates and opens all
four supported formats through Python and the native document reader. It
verifies the bundle signature again after execution. The native reader's
`--check-document PATH` mode uses the same document code as the window and
prints the prim count without starting the AppKit event loop.

Python is bundled and tested as an interpreter subprocess in this phase.
The in-process interpreter, live `vela.stage` API and console are Phase 6.
The smoke test does not automate Finder or visually inspect the window.
Offline builds require refreshing `bazel vendor` for the new Apple rules.
