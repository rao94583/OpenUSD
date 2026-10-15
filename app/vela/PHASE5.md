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
| Attribute editing | Actions for bool, integer, float/double, string, token, asset and vector attributes and arrays of these, at the default or a time sample. The inspector edits non-array values in place (double-click or click a selected Value): text parsed by USD type, at the current frame for animated attributes, one undo step. Arrays, half, quaternions, matrices, dictionaries and splines are not editable in the inspector yet. |
| Variants | Done: the Variants tab chooses or clears a selection from a pop-up (`set_variant`), one undo step. |
| Load / unload | Actions for a prim/subtree and the selection; Prim menu. |
| Activation | Actions for a prim and selection; Prim menu; undo across recomposition. |
| Visibility | Inherited/invisible authoring at default or time sample; Hide/Show Selected menu. This does not make an invisible ancestor visible. |
| Purposes | Authoring action for default/proxy/render/guide. View → Display shows proxy, render and guide purposes (`set_view_option`); snapshots match usdrecord's `--purposes`; framing and bounding boxes follow the shown purposes. |
| Model draw modes | Author model:drawMode; default/inherited/origin/bounds/cards. Inspector controls and applyDrawMode editing pending. |
| View draw modes | Done: View → Draw Mode (`set_draw_mode`): points, wireframe, wireframe on surface, flat and smooth shaded, geom only/flat/smooth. usdview's hidden-surface wireframe (a two-pass GL trick) is not reproduced. |
| Scene cameras | Done: View → Camera lists the stage's cameras (`set_view_camera`), fitted to the viewport as usdview does, following animation; navigating or framing detaches a free copy. The viewport's route matches usdrecord's images. Orthographic free cameras pending. |
| Render settings / AOVs | Done: the Renderer menu chooses the Hydra renderer (`set_renderer`; Storm and Embree, which the Bazel build now includes), the AOV shown (`set_aov`, any name, e.g. `primvars:st`), renderer settings (`set_renderer_setting`; flags toggle in the menu, other types by value, no form yet), the render settings prim (`set_render_settings_prim`, default from stage metadata) and render pass prim (`set_render_pass_prim`, which also activates its render source, as usdview). `get_renderer_info` returns the renderers, AOVs and settings as JSON. `--snapshot` takes usdrecord's `--renderer`, `--renderSettingsPrimPath`/`-rs`, `--renderPassPrimPath`/`-rp` (the settings prim's camera when no `--camera`) and `--aov`. A form for non-flag settings, and pause/stop for progressive renderers, pending. |
| Lighting / material | Done: View → Display toggles the camera light, usdview's default dome light, dome light textures, scene lights, scene materials and backface culling; lights apply to shaded draw modes, as in usdview. View → Background and Selection Highlighting as usdview's. Default material ambient/specular controls pending. |
| HUD | Done: usdview's four corners (renderer, Hgi and AOV; prim and geometry counts, off by default; render time, playback rate, optional GPU statistics; complexity and camera), toggled in View → Display (`set_view_option`). |
| Color management | Done: OpenColorIO is enabled in the USD build; View → Color Correction (`set_color_correction`: disabled, sRGB, openColorIO) and View → OCIO Display/View from the config (`set_ocio`, which also takes color space and look). `--snapshot --colorCorrectionMode` matches usdrecord for all three modes. Menus for OCIO color spaces and looks pending (actions accept them). |
| Bounding boxes | Done: dashed axis-aligned and oriented boxes around the selection (or the stage), not during playback by default, in usdview's line color; bounds follow the purposes shown (View → Display). |
| Validation | Validation panel, validators, severity, navigation to offending objects pending. |
| Settings | Done: Vela → Settings… (⌘,), saved as JSON in the user defaults (`core/app_settings.*`). Opening options (load payloads, look through main_cam, unloaded prims as boxes), view settings for new windows (View → Use as Default View Settings, or Use Current Window; Restore Defaults), thread limit at next launch, Reset All. Actions: `use_view_as_default`, `get_settings`, `set_settings`, `reset_settings`. Launch flags start from the settings; `--clearsettings` and `--defaultsettings` work as in usdview (tests use `--defaultsettings`). View → Default Material… (`set_default_material`, usdview's 0.2/0.1). |
| Saving | Done: File → Save (⌘S) writes the layer stack's edited layers in place, except the session layer; Export Flattened…, Export Session Layer… and Revert to Saved. The window shows unsaved edits and closing asks to save. In-memory layers can only be exported. |
| Launcher flags | Done: `--select`, `--camera` (path or name; default main_cam), `--mask`, `--unloaded`, `--sessionLayer`, `--mute`, `--complexity`, `--ff`/`--lf`/`--cf`, `--numThreads`, `--renderer`/`-r`, `--viewportSize`, `--norender`, `--bboxStandin`, `--timing`, `--quitAfterStartup`. Accepted and ignored until Vela has settings files: `--clearsettings`, `--defaultsettings`, `--config`, `--noplugins`. Rejected with a message: `--allow-async`, trace and memory statistics flags, `--dumpFirstImage` (use `--snapshot`), `--detachLayers*`. **To change (2026-10-15):** every flag also becomes a setting or a live command (PLAN.md, "Mac-native options"); the flags stay as a secondary path for scripts and tests. Done: Edit → Select Path… (`select_path`), Playback → Set/Reset Playback Range (`set_playback_range`, `reset_playback_range`), File → Muted Layers (`set_layer_muted`), View → Display → Pause Viewport Drawing and Unloaded Prims as Bounding Boxes (view settings `pauseDrawing`, `unloadedAsBounds`, which `--norender` and `--bboxStandin` now set), load times in the HUD's Info. File → Open with Options… and Reopen with Options… (payloads, population mask, session layer, muted layers; reopening keeps the view settings and asks before discarding edits), File → Replace Session Layer… (`replace_session_layer`). Every flag now has an in-app equivalent. |
| Command palette | ⌥Space palette over the whole action registry, with choices as entries, fuzzy search, recents, in-place parameters, and prims by path (PLAN.md, "Command palette"). Pending. |

Tests for the view settings, cameras, saving and launch options:
`actions_test`, `document_test`, `file_actions_test`,
`launch_options_test`, `saving_test` (through the AppKit document),
`launch_test` (a real launch) and `snapshot_test`, which compares Vela's
images with usdrecord's for purposes, complexity, the camera light, dome
light visibility, color correction modes, Embree and the viewport's scene
camera route, and checks that each draw mode and AOV changes the image.

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
