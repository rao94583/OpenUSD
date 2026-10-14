# Phase 5: usdview parity checklist

The initial authoring actions were checkpointed at `3aa373d80` before
starting Phase 4. The new browsing inspector is read-only; connect these
actions to editing controls when Phase 4 browsing is complete. Phase 5
remains in progress.

The reference is this repository's `pxr/usdImaging/usdviewq/`, especially
`appController.py`, `viewSettingsDataModel.py`, `stageView.py` and
`__init__.py` (launcher flags). The action registry describes parameter
schemas; authoring actions respect the current edit target and form one
undo step. Payload loading changes stage load rules, without layer edits.

| Area | Status / remaining work |
|---|---|
| Attribute editing | Actions for existing bool, integer, float/double, string, token, asset, float/double/int vector attributes and arrays of these; default values and numeric time samples; clear a default or sample. Inspector editors pending. Half, quaternions, matrices, dictionaries and splines pending. |
| Variants | Select or clear an existing variant selection with undo. Inspector controls pending. |
| Load / unload | Actions for a prim/subtree and the selection; Prim menu. |
| Activation | Actions for a prim and selection; Prim menu; undo across recomposition. |
| Visibility | Inherited/invisible authoring at default or time sample; Hide/Show Selected menu. This does not make an invisible ancestor visible. |
| Purposes | Authoring action for default/proxy/render/guide. View purpose toggles and bounds matching pending. |
| Model draw modes | Author model:drawMode; default/inherited/origin/bounds/cards. Inspector controls and applyDrawMode editing pending. |
| View draw modes | Points, wireframe, wireframe-on-surface, shaded flat/smooth, geometry-only/flat/smooth pending. |
| Scene cameras | Camera chooser, time-varying camera parameters, detach on navigation; orthographic free cameras pending. |
| Render settings / AOVs | Renderer choice, render-settings prim, renderer settings, AOV chooser and visualization pending. |
| Lighting / material | Scene lights, scene materials, camera light, dome background, default material controls pending. |
| HUD | Frame/render statistics, prim/geometry counts and toggles pending. |
| Color management | OCIO display/view/look/colorspace, raw/sRGB, consistent presentation pending. |
| Bounding boxes | World/selection bounds, oriented/aligned, visibility and purpose consistency pending. |
| Validation | Validation panel, validators, severity, navigation to offending objects pending. |
| Preferences | Persistence, reset, navigation/selection/display settings pending. |
| Saving | Explicit save/export, dirty-state/close prompts and layer-stack saving policy pending. Authoring currently changes in-memory layers only. |
| Launcher flags | `--select`, `--mask`, `--clearsettings`, `--config`, `--defaultsettings`, `--norender`, `--noplugins`, `--unloaded`, `--bboxStandin`, `--timing`, `--allow-async`, `--traceToFile`, `--traceFormat`, `--tracePython`, `--memstats`, `--dumpFirstImage`, `--viewportSize`, `--numThreads`, `--ff`, `--lf`, `--cf`, `--quitAfterStartup`, `--sessionLayer`, `--mute`, `--detachLayers`, `--detachLayersInclude`, `--detachLayersExclude`: audit and implement or document deliberate differences. Existing Vela snapshot/check/describe flags are distinct. |

Headless tests in `core/editing_actions_test.cpp` adapt the authored
visibility behavior from `testUsdviewInstancingEdits` and the parent/child
recomposition scenario from `testUsdviewDeactivate`. They also check
attribute type rejection, edit-target isolation, time samples, array values,
variant selection, read-only layers, payload load rules and undo/redo.
These are action-level ports; they do not claim viewport image parity.

Run without a display:

```sh
bazel test //app/vela:editing_actions_test //app/vela:actions_test \
  //app/vela:document_test //app/vela:undo_test
```

Bundle/menu checks can run unattended. Leave GPU snapshots and manual
window checks for an awake display; a sleeping display must not block core
progress or be mistaken for a rendering regression.
