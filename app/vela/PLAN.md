# Vela: plan

Vela is a native macOS replacement for usdview, built on the Bazel build of
OpenUSD in this repository. Its goals are in the README under
[Where this is going](../../README.md#where-this-is-going): everything
usdview does, a modern native UI, and a Claude Code panel for editing and
querying scenes in plain English, with skills for the USD Python API and for
driving the app.

This document records the decisions made so far, the architecture and the
phases. Phases 1 to 7 are done; see
[README.md](README.md) for building, using and testing the app.

## Decisions

| Decision | Choice |
|---|---|
| Name | **Vela** (pronounced VEH-la). Code in `app/vela/`. |
| UI toolkit | Objective-C++ and AppKit. A `WKWebView` may be used for the chat transcript only. |
| Core | Plain C++ with no AppKit: document model, action registry, undo, embedded Python, MCP bridge. Keeps the core testable without a UI and a Linux front end possible later. |
| Camera controls | Match Autodesk Maya (see [Camera controls](#camera-controls)). |
| Claude integration | The `claude` CLI as a subprocess in stream-JSON mode, plus an MCP server inside the app. |
| Still open (Phase 0) | Minimum macOS version (the build targets 13; 14 gives newer AppKit). Whether users install and log in to Claude Code themselves. Ad-hoc local signing or notarization. A name conflict check for "Vela". |

### Why Objective-C++ and AppKit

- `.mm` files include `pxr` headers directly and hold `UsdStageRefPtr`
  without a bridging layer. Swift's C++ interop with USD's templates,
  `TfRefPtr` and `VtValue` is still fragile and would add `rules_swift`.
- A view-based `NSOutlineView` with lazy children handles stages with
  millions of prims. SwiftUI's `List` and `OutlineGroup` slow down badly at
  that size.
- A `CAMetalLayer` view can display Storm's `HgiMetal` output texture
  directly, with no OpenGL interop (usdview needs GL).
- Bazel already supports it: `objc_library` compiles `.mm`, and
  `rules_apple`'s `macos_application` bundles the app.

## Architecture

```
┌──────────────── AppKit front end (ObjC++) ──────────────────────┐
│ Prim outline │ Metal viewport + timeline │ Inspector │ Claude panel │
└──────┬────────────────────┬───────────────────┬──────────┬──────┘
       ▼                    ▼                   ▼          │
┌───────────── Core (C++, no AppKit) ───────────────┐      │
│ Document: stage, edit target, selection, time,    │      │
│ view settings, cameras                            │      │
│ Action registry: every UI operation, JSON schema  │◄─────┤
│ Undo: SdfLayerStateDelegate → NSUndoManager groups│      │
│ Embedded CPython 3.11 sharing libusd_ms           │      │
└───────────────┬───────────────────────────────────┘      │
                ▼                                          ▼
     MCP bridge (Unix socket) ◄── vela-mcp helper ◄── claude CLI subprocess
```

### Document

One per window, through `NSDocument`. It owns the stage and all app state:
selection, current time, edit target, camera and view settings. Every change
goes through it and sends change notifications that the views observe.

### Action registry

This is the "UI manipulation API" from the README. Each operation is
registered once, with a name, a JSON schema for its parameters and a
handler. Menus, keyboard shortcuts, the ⌥Space command palette, the Python API and
Claude all call the same entries. Examples: `select`, `frame_selection`,
`set_time`, `set_camera`, `open_inspector`, `set_draw_mode`.

### Mac-native options, not command-line flags

A Mac user never types a command line, so nothing may be reachable only
through one (user decision, 2026-10-15). Each of usdview's launch flags
becomes either a **setting** in a Settings window (persisted, taking effect
when a file is next opened, or at next launch where noted) or a **live
command** (an action in a menu and the palette) that acts on the open
scene. The flags keep working as a secondary path for scripts, tests and
`open -a Vela --args`, and are implemented by running the same settings
and actions.

| Flag | Becomes |
|---|---|
| `--select PRIM` | Live: Edit → Select Path… (`select`), also from the palette |
| `--camera CAMERA` | Live: View → Camera (done). Setting: open through the primary camera (main_cam) when present (on by default, as usdview) |
| `--mask PRIM,...` | Per file: File → Open with Options… (population mask) and Reopen with Options… (reopens the stage) |
| `--unloaded` | Setting: load payloads when opening (on by default); also in Open with Options…; live: Prim → Load/Unload (done) |
| `--sessionLayer FILE` | Per file: Open with Options…; live: File → Replace Session Layer… |
| `--mute LAYER,...` | Live: File → Muted Layers (`set_layer_muted`); also in Open with Options… |
| `--complexity` | Live: View → Complexity (done). Setting: default complexity |
| `--ff`, `--lf`, `--cf` | Live: Playback → Set Playback Range… (a form) and the timeline (done for the current frame) |
| `--renderer` | Live: Renderer menu (done). Setting: default renderer |
| `--numThreads` | Setting: thread limit, at next launch |
| `--viewportSize` | Not needed: windows resize and remember their frames |
| `--norender` | Live: View → Display → Pause Viewport Drawing |
| `--bboxStandin` | Live: View → Display → Unloaded Prims as Bounding Boxes. Setting: its default |
| `--timing` | Live: the HUD's Info shows open and first-frame times (View → Display → HUD Prim Info) |
| `--clearsettings`, `--defaultsettings` | Settings → Reset to Defaults |
| `--config` | Not needed: settings persist (NSUserDefaults) |
| `--quitAfterStartup` | Tests only; stays a flag |

View settings that are not per-launch (draw mode, lights, HUD sections,
color correction and so on) get defaults in Settings too: the settings
apply to newly opened documents, and View changes apply to the current one.

### Command palette

⌥Space opens a palette (a floating, Spotlight-like panel) that reaches
everything the app can do, built entirely from the action registry, so
nothing needs registering twice:

- Every enabled action, and every choice of an action with choices (static
  and dynamic), as its own entry: "Draw Mode › Wireframe", "Camera ›
  /Cameras/main_cam", "Renderer › Embree". Checkmarks show the current
  state; shortcuts are shown.
- Fuzzy matching on title, menu path and description; recently used
  entries first.
- Actions with parameters ask for them in place: a form (the same form
  sheet menus use), a save panel, or a prim path completed from the stage.
- Also lists prims by path, to select and frame (usdview's search),
  documents and settings.
- ⌥Space works even in text fields (where it would type a non-breaking
  space). From Phase 7, text that matches nothing can go to Claude.

The registry is the single source of truth for the `vela-ui` skill and the
MCP tool schemas, which are generated from it at build time. It also serves
as the test harness for scripted UI tests.

### Undo

USD has no built-in undo. A custom `SdfLayerStateDelegate` records the
inverse of each edit (Maya USD works the same way) and groups the inverses
into `NSUndoManager` entries. Each Claude request becomes one undo step, so
anything Claude changes can be reverted with ⌘Z.

### Embedded Python

The app bundle contains the hermetic Python 3.11 and the `pxr` package. They
share `libusd_ms` with the app, so `vela.stage` in Python is the same stage
the window shows. A `vela` Python module exposes the document and the action
registry.

Scripts run on a worker thread under a document write lock. While a script
runs, the viewport keeps its last frame and the UI is read-only. Change
notifications are combined and applied on the main thread when the script
finishes.

### Viewport

`UsdImagingGLEngine` runs on an `HdDriver` holding an `HgiMetal` the app
creates. `GetAovTexture(HdAovTokens->color)` returns an `HgiTexture` whose
`id<MTLTexture>` is drawn into a `CAMetalLayer`. Picking uses
`TestIntersection`, and selection highlighting uses `SetSelected`.

### Camera controls

The defaults match Autodesk Maya:

| Input | Action |
|---|---|
| Option + left drag | **Tumble** around the center of interest, keeping the camera upright |
| Option + middle drag | **Track** (pan). Speed scales with distance to the center of interest, so the point under the cursor stays under it. |
| Option + right drag | **Dolly** toward or away from the center of interest. Moves the camera; doesn't change the field of view. |
| Option + left + middle drag | Dolly |
| Scroll wheel | Dolly |
| F / A | Frame the selection / frame everything. Either one resets the center of interest. |
| Navigating through a scene camera | Detaches to a free camera copy, so the scene's camera is never changed. |

Trackpad and no-middle-button equivalents (configurable in preferences, Maya
scheme as default):

- Option + two-finger drag: track
- Pinch: dolly
- Option + Command + left drag: stands in for the middle button

usdview's `freeCamera.py` already uses Maya-style Alt bindings, so porting it
is a good start. The camera math (tumble, track, dolly, frame) lives in the
C++ core with unit tests. The UI only translates input events.

### Claude panel

- **CLI subprocess.** The app starts the user's `claude` CLI in stream-JSON
  mode with a persistent session. The Agent SDKs do the same thing
  internally, and this avoids bundling Node or depending on the Python SDK.
  All CLI-specific code sits in one adapter class, pinned to a minimum CLI
  version. Check the current CLI flags when implementing.
- **App tools over MCP.** `vela-mcp`, a small helper binary in the bundle,
  connects the CLI's stdio to the running app over a Unix socket, so no TCP
  port is opened. A Claude Code session in a terminal can use it to drive
  the app too. Tools:
  - `run_python`: runs code against the live stage and returns the result
  - `ui_action`, `list_actions`
  - `get_app_state`: selection, time, edit target, camera
  - `viewport_screenshot`: lets Claude see the render
- **Approvals.** The CLI's permission-prompt hook routes approvals to the
  panel as in-app dialogs. Queries run freely. Edits either run with a
  summary afterwards or ask first, as a user setting.
- **Context.** Each prompt includes the current selection and edit target,
  the way Claude Code includes an editor selection.

### Skills

Image generation for the embedded Claude is described in
[IMAGE_GENERATION.md](IMAGE_GENERATION.md): Vela-owned asynchronous image
jobs with pluggable providers (GPT Image first), exposed through MCP, with
bundled instructions, native previews and
separate optional USD texture application. The initial implementation includes
OpenAI and Gemini adapters, Keychain setup, native job/preview controls and
MCP tools; see the document's implementation record for validation and limits.

- **`usd-python`**: curated idioms and pitfalls (change blocks, edit
  targets, UsdShade binding, UsdLux, variants, time samples), plus a schema
  reference generated from this build's schema registry.
- **`vela-ui`**: generated from the action registry.
- **Evals**: test scenes paired with prompts and automated checks (for
  example, that "make every light 20% warmer" changed every light's color).
  They run headless.

## Phases

| # | Phase | Done when |
|---|---|---|
| 0 | **Decisions:** the open items under [Decisions](#decisions) | Recorded in this file |
| 1 | **Skeleton and bundle:** `rules_apple`, `macos_application`. Embed the dylibs, `usd/` resources and Python so Plug finds every plugin inside the `.app`. `NSDocument` opens .usd, .usda, .usdc and .usdz. | Double-clicking a .usdz opens it and shows the prim count. A test confirms all plugins load from the bundle. The embedded Python runs from the signed bundle. |
| 2 | **Viewport:** Metal view, Storm, Maya camera controls, frame all and selection, basic time scrubbing | Interactive frame rate on a large scene. A snapshot matches `usdrecord` (compared with `idiff`). Camera math unit tests pass. |
| 3 | **Core:** document, selection, action registry, undo delegate. Menus go through the registry. | C++ tests for undo/redo, including round trips across layers |
| 4 | **Browsing:** lazy prim outline (filters; type, visibility and draw-mode columns), inspector tabs (properties with array and spline viewers, metadata, composition arcs, layer stack, variants), timeline with playback, picking synced with selection | Everything usdview shows can be browsed |
| 5 | **Editing and view settings to match usdview:** attribute editing, variants, load/unload, activation, visibility, purposes, draw modes, scene cameras, render settings and AOVs, HUD, OCIO, bounding boxes, validation panel, a Settings window, usdview's command-line flags as settings or live commands (see [Mac-native options](#mac-native-options-not-command-line-flags)), the ⌥Space command palette | A parity checklist built from `pxr/usdImaging/usdviewq` is complete. Selected `testusdview` scenarios are ported to the action API. Nothing is reachable only from the command line; the palette reaches every action. |
| 6 | **Python and MCP:** embedded interpreter, the `vela` module, a console panel replacing usdview's interpreter, `vela-mcp` and the socket | Claude Code in a terminal can query and edit the open scene |
| 7 | **Claude panel:** streaming chat, tool-call display, approvals, undo groups, screenshots, session history | The README's example requests work end to end |
| 8 | **Skills and evals:** both skills, generated references, eval suite | Evals pass at an agreed rate |
| 9 | **Polish:** performance on huge stages (background open, payloads), signing and notarization, docs. Optionally a compatibility layer for usdview plugins. | Ready to ship |

## Phase 1 progress

Implemented the AppKit `NSDocument` skeleton, Finder document type declarations,
prim count window, File menu, `rules_apple` bundle, relocatable USD resources and
Python 3.11 runtime, and local ad-hoc signing. Build instructions are in
[README.md](README.md). `bazel test //app/vela:bundle_smoke_test` passes: 48
installed plugins load from an extracted signed bundle, all four scene formats
open in Python and the native document reader, and signatures remain valid.
The test also confirms the native executable loads the runtime's `libusd_ms`.

Finder double-click dispatch and the visible window still require a manual
smoke check. In-process Python and the live document API remain in Phase 6.
Release signing/notarization and the other Phase 0 decisions remain open.

## Phase 2 progress

Done. Code: `core/free_camera.*` (plain C++, Maya camera math ported from
usdview's `freeCamera.py`), `viewport/renderer.*` (`UsdImagingGLEngine` on
an app-owned `HgiMetal`, presentation disabled, the color AOV drawn into a
`CAMetalLayer` drawable by a small Metal pass), `viewport/viewport_view.*`
(input, picking with `TestIntersection`, selection highlight) and
`document.mm` (window, timeline).

- `free_camera_test`: orientation for Y- and Z-up, tumble stays upright and
  on the sphere, tracking keeps the point under the cursor, dolly, framing
  fits rotated bounds, clipping range, scene camera round trip, drag modes.
- `snapshot_test`: Vela's images match usdrecord's for 7 views. 6 are
  bit-identical; niSpheres differs on under 0.07% of edge pixels, as much as
  usdrecord differs from itself on that scene.
- Frame rate (Apple M5 Pro): 22,500 distinct meshes, 23M triangles, a full
  tumble at 2400x1544 in the window: 33 fps (30 ms per frame); 35 fps
  offscreen at 1920x1080. First image 1.8 s after open. Small scenes run at
  the display's refresh rate.

Notes for later phases:

- The view schedules its own renders (a display link on macOS 14+, a main
  queue dispatch on 13): AppKit never calls `updateLayer` for a
  `CAMetalLayer` backing layer. No frames are drawn while the display
  sleeps.
- Looking through scene cameras in the window (and detaching to a free
  camera, already in `FreeCamera::FromGfCamera`) is left for the camera menu
  in Phase 5. Orthographic free cameras are not implemented yet.
- Framing fits the bounds' sphere to the vertical field of view; very tall,
  narrow windows can crop wide scenes horizontally.
- The F/A and arrow keys are handled by the view and window controller for
  now; Phase 3 moves them to the action registry with the menus.

## Phase 3 progress

Implemented; interactive checks pending (the Mac slept before they could
run). Code in `core/` (plain C++, no AppKit):

- `undo.*`: `UndoStack` installs a recording `SdfLayerStateDelegateBase` on
  each tracked layer. Each authoring primitive records its inverse (old
  field, dictionary key or time sample; delete for create; the whole saved
  subtree for delete; reverse move; pop for push). Inverses are applied
  through the delegate, so undoing records the redo step. Groups nest; edits
  outside a group form an implicit "Edit" step.
- `document.*`: `vela::Document` owns the stage, selection, time, playback
  state, free camera, edit target and undo stack, with change notifications
  for views. It tracks every used layer, including ones added later.
- `actions.*`, `standard_actions.cpp`: the registry. Parameters are JSON
  (`pxr/base/js`) validated against a JSON-schema subset. `Describe()` is
  the source for the vela-ui skill and MCP schemas. 14 actions: undo, redo,
  select, clear_selection, set_edit_target, frame_selection, frame_all,
  set_camera, toggle_playback, next/previous/first/last_frame, set_time.
- The front end builds the Edit, View and Playback menus from the registry
  (`menus.mm`); items validate and retitle from document state ("Undo
  Rename", "Pause"). Shortcuts, the timeline and click-to-select run
  actions. Camera drags edit the document's camera directly rather than
  running an action per mouse move.

Tests: `undo_test` (round trips compare every layer's text before and after:
attribute values, time samples, creating and deleting subtrees with
metadata, relationships and variants, rename and reparent, dictionaries,
one step across root, sublayer and session layers, layer `Clear` and
`TransferContent`, grouping and step sequences); `document_test`;
`actions_test`; `menus_test` (the menu matches the registry). Breaking an
inverse on purpose makes the round trips fail.

Decisions and limits:

- The core owns the undo stack, not `NSUndoManager`: Python, MCP and the
  menus share one history, testable without AppKit. `NSDocument`'s undo
  manager is off.
- Plain-key shortcuts (F, A, Space, arrows) are menu key equivalents; their
  items disable while a text field is first responder, so typing passes
  through.
- Layers whose data streams from disk (`.usdc`) don't report `Clear`,
  `Reload` or `TransferContent` to the state delegate, so those can't be
  undone on such layers. Edits to them otherwise undo normally.
- Selection, time and camera changes are not undo steps.

To check interactively: menus and their enabled states, F/A/Space/arrow
keys in the viewport, click and Shift-click selection, the timeline and
playback.

## Phase 4 progress

Started after checkpointing the initial Phase 5 actions (`3aa373d80`).
The window now places a lazy prim outline and a read-only inspector on either
side of the viewport; the existing timeline and playback remain below it.

- `core/browser.*`: direct-child enumeration (including inactive prims and
  instance proxies), a constant-space expander check without enumerating a
  whole branch, case-insensitive path/type search that retains ancestors,
  time-dependent visibility and model draw-mode summaries. Searches scan
  the composed hierarchy once per query or stage change; visible-only filters
  also refresh with time. Active, loaded, model and visible filters retain
  matching ancestors. Optional prototype roots and a Prototype navigation
  button support inspecting generated prims; authoring rejects those prims.
  Unfiltered browsing
  does not traverse unopened branches.
- `browser_view.*`: view-based `NSOutlineView` with Prim, Type, Visibility and
  Draw Mode columns. Canonical path items preserve identity. Tree selections
  run the action registry; viewport selections reveal their ancestors and
  update the tree and inspector. Filtering preserves the document selection.
  Stage notices coalesce before refreshing AppKit controls.
- Inspector tabs: composed Properties, nested Metadata previews, Composition
  arcs and prim specs, the local Layer stack with edit target/dirty state,
  and Variant sets with choices and selection. Property search matches names
  or types. Property detail tabs include composed metadata and authored
  opinions with layer offsets, strongest to weakest. Double-click Layers or
  Composition rows for layer metadata and sublayer details. Multiple selection inspects
  the last path in document selection order.
- Double-click a property for a read-only detail window: paginated arrays,
  relationship targets, numeric time samples or spline knots with interpolation
  and tangent slopes. Pages are bounded, and property summaries avoid dumping
  large arrays. Array previews use the USD value visitor, including half,
  quaternion, matrix, asset and path-expression arrays; a registry-wide test
  exercises every built-in Sdf array type. A Curve tab samples scalar
  splines through the USD attribute evaluator (at most 1024 samples and 256
  knot markers) and draws with AppKit, without Metal. Knot tables stay paged.
- `browser_test`: ancestor-preserving search, search invalidation after edits,
  inactive prims and instance proxies, time-varying values, nested metadata,
  layers, variants, composition, schema-wide array paging, spline knots and
  bounded curve evaluation. Tests also cover time-dependent filters, prototype
  navigation and refusal to edit generated prototype prims.
- `browser_ui_test` / `--check-browser FILE`: instantiate controls without
  opening a window; check both selection directions, ancestor reveal,
  filter preservation, inspector population and property detail tabs.
  `window_layout.*` pins the panes and their contents to their allocated
  split-view regions; a resize regression check widens and narrows the
  document layout without constructing the renderer. No rendering/display wake
  is required.

Phase 4 is not complete. Still pending: advanced spline information
(extrapolation/loops/tangent algorithms) and composition graph/navigation.
The Curve tab is a sampled preview over the knot interval, not an exact
rendering of every discontinuity or extrapolation. Manual layout and picking
checks remain for an awake display. The existing playback/timeline already
uses the shared document and actions.

Review fixes (2026-10-14):

- Relationship details always start with a summary row ("No targets" or
  the count), so an untargeted relationship no longer opens a blank window.
  `browser_ui_test` also runs `--check-browser` on the snapshot test's real
  scenes, where that case failed.
- Change notifications distinguish resyncs (`Document::Resync`). The prim
  count, the outline tree and most filters rebuild only on a resync; value
  edits and time changes redraw the outline rows on screen. The Visible
  filter rescans on time changes only when some visibility is animated.
- During playback the outline and the inspector's property values refresh
  at most four times a second; time changes no longer rebuild metadata,
  composition, layer or variant tabs.

Window check (2026-10-14), driven through real input with screenshots:
selection sync both ways, Maya camera, frame all/selected, playback,
typing in search fields, property windows and Prim → Hide with undo all
work. Fixed:

- Selection highlights were never drawn (`highlight` render parameter).
  They now draw as usdview's half-transparent yellow, except during
  playback.
- Framing fits the bounds' projected corners exactly in both directions
  for the viewport's aspect ratio, instead of a vertical-only sphere fit
  that cropped scenes in the narrow viewport between the sidebars.
- Layout: 1440 × 880 default window with 340 / 380 pt sidebars that keep
  their width when the window resizes. The inspector shows Name and Value
  first; the outline's columns fit, with usdview's compact V / I
  visibility.
- Booleans show as true / false; counts read "1 element".

## Phase 5 progress

Started with the independent authoring actions in `core/editing_actions.cpp`.
This work was checkpointed before starting Phase 4; connecting these actions
to inspector editing controls is pending. See [PHASE5.md](PHASE5.md) for the usdview parity checklist and
remaining work; Phase 5 is not complete.

- `set_attribute` and `clear_attribute`: existing typed attributes, scalar
  and array values, default values and numeric time samples. Invalid types,
  integer overflow and uniform time samples are rejected before authoring.
- `set_variant`, `set_active`, `set_visibility`, `set_purpose` and
  `set_model_draw_mode`: author to the document's current edit target and
  share its undo history. Instance proxies and read-only targets are rejected.
- `set_payload_loaded`: load rules without layer authoring or undo entries.
- Prim menu: hide/show, activate/deactivate and load/unload the selection.
  Authoring batches are one undo step and validate all prims before edits;
  descendants are deactivated before ancestors. The status prim count now
  refreshes after recomposition.
- `editing_actions_test`: typed values, 64-bit limits, root/session target
  isolation, time samples, variants, inherited visibility, parent/child
  activation and payloads, including undo/redo layer-text round trips.
  It adapts action scenarios from `testUsdviewDeactivate` and
  `testUsdviewInstancingEdits` without requiring a display.

Checks: editing, action, document, undo, camera, menu and signed-bundle
smoke tests. GPU snapshots and manual UI checks are deferred while the
display may sleep. Edits remain in-memory; saving/export and dirty-state
handling are still pending.

Phase 5, continued (2026-10-14): view settings (`core/view_settings.*`,
`core/view_actions.cpp`), scene cameras (in `Document`), saving
(`core/file_actions.cpp`, `VelaDocument`) and usdview's launch flags
(`core/launch_options.*`); see [PHASE5.md](PHASE5.md). Actions with an
enum parameter show as submenus with checkmarks (static choices, or
dynamic ones such as the camera list); actions that write a file ask for
it with a save panel. The background now uses usdview's linear Dark Grey
(0.07), where Vela had 0.3. The inspector edits attribute values
(`core/value_text.*`) and variant selections. Found and fixed in a window
check: a selection made before the first frame (e.g. `--select`) was not
highlighted, because the engine drops selections set before it populates
the scene. Still pending: inspector editing for arrays and other types,
HUD, bounding boxes, OCIO, render settings and AOVs, the validation panel
and preferences.

Phase 5, continued (2026-10-14, later): bounding boxes, the HUD
(`core/hud.*`, `viewport/hud_view.*`), color management (OpenColorIO turned
on in the USD build; `core/color_management.*`) and the Renderer menu
(renderer, AOV, renderer settings, render settings and pass prims;
`RendererInfo`, which the viewport publishes to the document for dynamic
menus and `get_renderer_info`). The Bazel build gained Embree
(`vendor/embree`) and the hdEmbree delegate, so Vela has a second renderer.
Fixed along the way: with color correction disabled the image was blank,
because HgiMetal had not yet committed Storm's render pass when Vela
presented (it commits graphics work only after a blit); switching renderer
plugins re-enabled the engine's OpenGL presentation (a crash in
`glGetString`); and the HUD's per-frame font lookup could return nil while
Embree kept every core busy. Still pending: inspector editing for arrays
and other types, the validation panel, preferences, and a settings form.

## Phase 6 progress

Done (2026-10-15). A Claude Code session in a terminal, with `vela-mcp`
as an MCP server, listed the prims of the open scene, read the cube's
size, changed its size and color, and described a viewport screenshot;
the window showed the change.

- **Interpreter** (`core/python.*`): the bundle's Python 3.11 starts at
  launch in this process, isolated (`PyConfig_InitIsolatedConfig`, home
  `Runtime/python`, `Runtime/usd/lib/python` first on `sys.path`, no
  signal handlers). The GIL is released after start-up and taken per run.
- **`vela` module** (`core/vela_module.py`, compiled into the core as a
  string and imported on first use, so pxr's import doesn't slow launch):
  `stage`, `selection`, `time` (module properties, assignable), `document`,
  `documents()`, `current()`, `run()`, `actions()`, `open()`, `Document`.
  It needs no boost.python: the app puts each document's stage in
  `UsdUtilsStageCache`, and Python finds the same stage by id, through the
  shared `libusd_ms`. A small C module (`_vela`) passes JSON.
- **Session** (`core/session.*`): open documents with ids; the current one
  is a scoped one (a console's), else the front window's, else the newest.
  `DocumentState` is `get_app_state`'s report.
- **Threading:** scripts run on the main thread for now, holding the GIL,
  under one `UndoBlock` ("Python"); MCP messages are handed to the main
  thread (`dispatch_sync`, which also runs during menus and sheets). The
  worker thread with a write lock remains future work: Document observers
  and AppKit are main-thread only.
- **Console** (`python_console.*`): Window → Python Console (⌥⌘P), a
  window controller of the document, so ⌘Z and the menus act on its
  document while it is key.
- **MCP** (`core/mcp.*`, `core/mcp_socket.*`, `mcp_bridge.cpp`): the app
  serves JSON-RPC lines on a Unix socket in Application Support (0700
  folder, 0600 socket, peer uid checked); `vela-mcp` in `Contents/MacOS`
  moves lines between stdio and the socket and launches Vela when needed.
  Both protocol eras: Claude Code 2.1.296 probes with `server/discover`
  and then speaks 2026-07-28, whose `tools/list` and `server/discover`
  results must carry `ttlMs` and `cacheScope` (it rejected the list
  without them). Messages must be one line: `JsWriteToString`
  pretty-prints, so replies use a compact `JsWriter`.
- **Settings:** Claude and MCP: a switch for the server (on by default),
  its status, and the `claude mcp add` command with a Copy button.
- `Describe()` includes `query`; MCP marks read-only tools.
- Fixed: `Renderer::WriteImage` left Embree non-interactive (it now
  restores `enableInteractive`); `--defaultsettings` now also ignores
  saved window state, after a killed test run restored its window into
  the next test.
- Tests: `mcp_test` (protocol, tools, socket), `python_test`
  (`--check-python`), `mcp_ui_test` (a launched app driven through
  `vela-mcp`: prims, attributes, undo, selection, screenshot, both eras).

Left for later: scripts on a worker thread with batched notifications, a
way to interrupt a long script, and the console's Clear command.

## UI changes after Phase 6 (2026-10-16)

- Panes: `window_layout.*` puts the prim browser and inspector in an
  `NSSplitViewController` (sidebar and inspector items, collapsible) and
  the bottom panel in a plain `NSSplitView` below them. A split view
  controller resizes the window to fit an item it expands, which shrank the
  window to its narrowest when the panel opened; and stack views hug above
  the window's size priority (500), so bars that span a pane set their
  horizontal hugging low. `VelaCheckDocumentLayout` checks sizes, collapsing
  and that no panel tab resizes the window.
- Bottom panel (`bottom_panel.*`): Python (`python_console.*`, now a view
  controller), Log (`log_view.*`, `core/log.*`: a TfDiagnosticMgr delegate
  that still prints to stderr), Claude (a placeholder).
- Color coding (`core/color_codes.*`, `color_coding.*`): usdview's values
  from usdviewq/common.py; in light mode the colors darken. `PrimSummary`
  has a style and font, and `InspectorRow` a value source, interpolation
  and targets.
- Relationship and connection targets: the properties table's context menu
  goes to them (selecting the prim, then the property row).

## Image and movie export (2026-10-16)

- `export_image` and `export_movie` (core/file_actions.cpp) call
  `Document::ViewportServices`, which the viewport sets: it owns the
  renderer. `Renderer::ReadPixels` renders offscreen until converged into
  its own texture and reads it back (8-bit display image, or the linear
  float color AOV for EXR/HDR, rendered without color correction since
  that overwrites the AOV). Nothing reads the window, so obscured or
  minimized windows export the same; the viewport doesn't draw meanwhile.
- Movies: `viewport/movie_writer.*` (AVAssetWriter; H.264/HEVC sizes round
  up to even), frames at time codes start, start + 1, ..., end through
  `ComputeViewGfCamera(aspect, time)`; a modal-session progress panel
  with Cancel removes the partial file.
- Resolution presets and framing (`core/export_framing.*`, unit-tested):
  the camera's aspect ratio is a scene camera's aperture or the
  viewport's; Fit renders the camera's own frame and composites it into
  black bars (any pixel format, either row order), Crop conforms the
  camera with `CameraUtilCrop`. Form choices can show titles for values
  (`FormField::choiceValues`).
- Menus: actions with a form and a save path ask for both (form first);
  `Action::saveTypes` gives the save panel a Format pop-up. Fixed while
  testing: the panels' completion blocks captured the `params` reference
  (C++ references are captured by reference), which dangled.

## Phase 7 design (2026-10-18)

Branch `vela-claude-panel`. Checked against Claude Code 2.1.296.

- **CLI protocol** (verified by hand): `claude -p --input-format stream-json
  --output-format stream-json --verbose --include-partial-messages
  --permission-prompt-tool stdio`. The host sends an `initialize` control
  request, then user messages (text and image blocks) as lines on stdin. The
  CLI streams `system/init` (session id), `stream_event` deltas, `assistant`
  and `user` (tool results) messages and a `result` per turn. It asks for
  approval with a `can_use_tool` control request, which the host answers
  with `allow` (with `updatedInput`) or `deny`. Stop sends an `interrupt`
  control request. Resuming a conversation passes `--resume <session id>`.
- **Adapter** (`core/claude_cli.*`): everything CLI-specific. It builds the
  arguments, parses lines into events, writes control and user messages,
  finds the executable (a setting, then the usual install folders, then a
  login shell's `command -v claude`) and checks the minimum version.
  `core/claude_process.*` runs it with pipes, and a reader thread hands
  lines to the main thread.
- **Tools:** the panel's CLI gets the app's MCP server, and only that:
  `--strict-mcp-config`, plus the built-in `Read`, `Glob` and `Grep` for
  the scene's files. So Claude has the whole `pxr` API (`run_python`,
  `query_python`) and every action (`ui_action`, `list_actions`,
  `get_app_state`, `viewport_screenshot`). Each panel runs a private
  socket server bound to its document (each message runs with that
  document current), so the panel works even with Settings' public MCP
  server off, and two windows never act on each other's scene.
- **`query_python`** (new, read-only): runs code like `run_python`, but
  reverts and rejects any edit to the stage's layers, so queries never
  need approval.
- **Approvals:** read-only tools, and `ui_action` with a query action,
  run without asking. Edits (`run_python`, other actions, `open_document`)
  follow a setting: *Edit, then summarize* (the default; each request is
  one undo step anyway) or *Ask first*. Anything else the CLI asks about
  shows the same in-panel card: Allow, Allow for This Conversation, Deny.
- **Undo:** each request becomes one undo step, "Claude: <request>".
  The panel marks the undo stack when the request starts and, when its
  result arrives, merges the steps added since into one
  (`UndoStack::Mark`/`Coalesce`). Undo stays available while Claude
  works.
- **Context:** each prompt carries the document's path, selection, edit
  target and time in a short preamble, as Claude Code includes an editor
  selection. Screenshot tool results show as thumbnails; a toggle attaches
  the viewport's image to a prompt.
- **History:** the panel writes its own log of each conversation (prompts,
  replies, tool calls, results) to `~/Library/Application Support/Vela/
  Claude/<session id>.jsonl`. The panel's menu lists the document's past
  conversations; choosing one replays the log and resumes the CLI session.
- **Mac-native:** Window → Claude (⌥⌘K); an `ask_claude` action (menu,
  palette and scripting); palette text that matches nothing offers "Ask
  Claude"; Settings → Claude and MCP gains the CLI's location, the approval
  mode and the model.

## Phase 7 progress

Done (2026-10-18), as designed above. The README's four example requests
worked end to end with Claude Code 2.1.296 on
`examples/living_room` (in a launched Vela, through the panel):

- "make every light in this room 20% warmer": Claude read the four lights'
  colors with `query_python`, then shifted each toward 5200 K with
  `run_python`. One undo step reverted all four.
- "add a variant set for the chair's fabric": a `fabric` variant set with
  four variants, each binding a different material to the seat and back,
  with the direct bindings moved into the variants. One undo step.
- "which prims have no material bound?": Rug, Vase and Book, which is
  correct. No undo step.
- "why is this mesh invisible in the render?" (with the plant selected):
  its parent Shelf is `invisible`. No undo step.

Also checked in the window: typing and Return, streaming Markdown, tool
rows and their details, screenshot thumbnails, an approval card (with
"Ask before Claude edits" on) and the panel's Undo button, Stop (Escape),
the conversation menu replaying and resuming an earlier conversation,
Settings → Claude and MCP, and the palette's "Ask Claude" entry. The CLI
exits when its window closes or the app quits.

- Core: `claude_cli.*` (the adapter), `claude_process.*` (child process),
  `claude_history.*`, `UndoStack::Mark`/`Coalesce`/`Discard`, the
  `query_python` MCP tool, and Claude settings in `AppSettings`.
- App: `claude_panel.*` (panel, `ask_claude`), `claude_markdown.*`.
- Tests: `claude_test` (protocol lines recorded from the CLI, approvals,
  processes, history), `undo_test` (coalescing, discarding), `mcp_ui_test`
  (`query_python`), and `claude_ui_test`, a launched Vela with a stand-in
  CLI that speaks the stream-JSON protocol and calls Vela's tools through
  the panel's `vela-mcp`. It checks the arguments, context, approvals, one
  undo step per request, history, the CLI stopping, and resuming.
- Found along the way: a query that changes a variant selection to inspect
  each variant is an edit, so `query_python` reverts it and says so. Claude
  read the results anyway and left the selection as it was.

Left for later:

- Edits the user makes while a request runs join that request's undo step.
- Python still runs on the main thread, so a long script freezes the
  window. Panels share the `mcp` Python namespace with outside MCP
  clients.
- With "Ask before edits" on, every `run_python` asks, even for code that
  only reads. Claude uses `query_python` for questions, which never asks.
- The CLI loads the user's own Claude Code settings (CLAUDE.md, hooks,
  plugins). That is intended, since it is their Claude, but it is untested
  with unusual setups.
- Screenshots are kept in the history as base64, so conversations with many
  screenshots make large files.

## Claude panel, more like the editor integrations (2026-10-18)

The panel now behaves like Claude Code's VS Code panel:

- **Header:** a History button opens a popover with this document's
  conversations, a search field, relative times, Open and Delete. The
  conversation's title, a spinner with a Stop button while Claude works,
  and "+" for a new conversation complete it. Switching conversations
  while Claude works stops the request first.
- **Queueing:** typing while Claude works queues the message (shown
  dimmed), which sends when the request ends. Stop returns it to the
  input.
- **Pickers under the input:** model, effort (`--effort`, new
  `AppSettings::claudeEffort`) and edit mode, stored in the settings and
  shared with Settings. A model or effort change ends the CLI before the
  next message, which resumes the conversation with the new flags
  (checked: `--resume <id> --effort high`).
- `stop_claude` action. `claude_ui_test` covers it with a slow reply from
  the stand-in CLI that honors interrupts.
- **One working folder:** the CLI used to run in the scene's folder, so
  Claude Code filed each session under that folder's project (and loaded
  any `CLAUDE.md` above the scene, such as this repository's). It now
  runs in `~/Library/Application Support/Vela/Claude/Workspace` with
  `--add-dir` for the scene's folder. Each conversation's record keeps
  that folder (`directory`), because resuming needs the same one;
  conversations from before resume in their document's folder. Claude
  Code's own transcripts stay under `~/.claude/projects/`: moving them
  would take `CLAUDE_CONFIG_DIR`, which also moves the user's login and
  settings.

## Testing

- `cc_test` for the core: document, actions, undo, camera math
- The `vela` module through `--check-python`, and MCP through a launched
  app (`mcp_ui_test`)
- Scripted tests that drive the app through the action registry
- Viewport image comparisons with `idiff`, as for Storm today
- A few `macos_ui_test` smoke tests

## Risks

1. **Threading** between Python, Hydra and the UI. Contained by the write
   lock and the read-only-while-running design.
2. **Undo correctness** in edge cases: namespace edits, sublayer changes,
   whole-layer replacement.
3. **Changes to the Claude CLI's interface.** Contained by the single
   adapter class and a pinned minimum version.
4. **Relocating the embedded Python** inside a signed bundle. The
   python-build-standalone interpreter that rules_python uses is designed to
   be relocatable, but this is tested first, in Phase 1.
