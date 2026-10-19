---
name: ui
description: How to drive the Vela app itself with its actions (ui_action) - selection, framing, cameras, draw modes, view options, renderers and AOVs, time and playback, exporting images and movies, validation, layers, settings - and how to check the result with viewport_screenshot. Use when the user asks to show, frame, look through, render, export or change how the scene is viewed, rather than to change the scene's data.
---

# Driving Vela

Every menu command in Vela is an action with a name and a JSON parameter
schema. `ui_action` runs one, as its menu item would; `vela.run(name,
**params)` does the same from Python. `references/actions.md` lists every
action in this copy of Vela, including the user's plugins' commands, with
its parameters, under a heading per action: Grep it (`^### export_image`,
with a few lines after) or read it, instead of calling `list_actions`,
which returns much more text.

## Actions or Python?

- **The view** (camera, draw mode, lights in the viewport, renderer,
  selection, time, HUD, bounding boxes): actions. These change the window,
  not the scene, so they aren't undo steps and never mark the document
  edited.
- **The scene's data**: `run_python`, or the editing actions
  (`set_attribute`, `set_variant`, `set_visibility`, `set_active`,
  `set_purpose`, `hide_selection`, ...). Both are undoable and author to
  the edit target. An editing action is simplest for one value; Python for
  anything that needs a loop or a lookup.
- **Questions**: `query_python` for the scene, `get_app_state` for the
  window (open documents, selection, time and range, edit target, layer
  stack, view and free cameras, renderer, undo state), and actions marked *query* in the
  reference (`get_renderer_info`, `get_settings`).

## Seeing the result

`viewport_screenshot` renders the viewport as it is now and shows it to
you. Use it after changing the view or the look of the scene when the
request is visual ("make the room cozier", "is the lamp in shot?"), and
say what you see; skip it for data edits whose result is plain from a
query. Pass a small `width` (512) unless detail matters.

## Common tasks

- **Select and frame**: `select` with `paths` (and `mode` replace, add,
  remove, toggle), then `frame_selection`; `frame_all` for everything.
  Finding the paths is a `query_python` job.
- **Look through a scene camera**: `set_view_camera` with the camera's
  path; an empty path returns to the free camera. Navigating detaches a
  free copy, so the scene camera never moves.
- **Place the free camera**: `set_camera` with `eye` and `target` in world
  coordinates (and `fov`, vertical degrees). Compute them from a bounding
  box (see the usd-python skill) and the stage's up axis.
- **How the viewport draws**: `set_draw_mode` (wireframe, shadedSmooth,
  ...), `set_complexity`, `set_view_option` for lights (`cameraLight`,
  `domeLight`, `sceneLights`), `sceneMaterials`, purposes (`proxy`,
  `render`, `guide`), bounding boxes and the HUD.
- **Renderers**: `get_renderer_info` lists them, the AOVs and the
  settings; `set_renderer` (Storm is "GL", or "Embree"), `set_aov`,
  `set_renderer_setting`.
- **Time**: `set_time`, `set_playback_range`, `toggle_playback`.
  `vela.time` reads and sets it from Python too.
- **Images and movies**: `export_image` and `export_movie` render
  offscreen at a standard `resolution` (1080p, 2160p, square, ...) or
  `width`/`height`, to `path`. Choose a path next to the scene unless the
  user gave one, and tell them where the file is. Exporting is not image
  generation; it renders the actual scene.
- **Checking a scene**: `validate` runs USD's validators and returns the
  issues; the Validation panel shows them too.
- **Layers**: `set_edit_target` sends edits to a layer in the layer stack;
  `set_layer_muted` mutes one without editing anything. Change the edit
  target only when the user asks, and mention it.
- **Settings**: `get_settings` and `set_settings` (a partial object
  changes only the keys it names); `use_view_as_default` makes the current
  view settings the default for files opened later.

## Things to know

- Actions whose reference entry says *asks the user* show Vela's own
  confirmation (installing Python packages, for example). Run them when the
  request calls for it; don't ask in chat as well.
- `save`, `revert` and the export actions write or discard files: run them
  only when the user asked for that.
- Plugin commands are actions too, named as their plugins registered them;
  they run on the current document.
- An action that fails returns its error: report it rather than retrying
  the same call, and check the parameters against the reference.
- `ask_claude` and `stop_claude` are for scripts and the palette to talk to
  this panel; never call them yourself.
