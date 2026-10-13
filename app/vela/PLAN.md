# Vela: plan

Vela is a native macOS replacement for usdview, built on the Bazel build of
OpenUSD in this repository. Its goals are in the README under
[Where this is going](../../README.md#where-this-is-going): everything
usdview does, a modern native UI, and a Claude Code panel for editing and
querying scenes in plain English, with skills for the USD Python API and for
driving the app.

This document records the decisions made so far, the architecture and the
phases. Phases 1 and 2 are done; see [README.md](README.md) for building,
using and testing the app.

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
handler. Menus, keyboard shortcuts, a ⌘K command palette, the Python API and
Claude all call the same entries. Examples: `select`, `frame_selection`,
`set_time`, `set_camera`, `open_inspector`, `set_draw_mode`.

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
| 5 | **Editing and view settings to match usdview:** attribute editing, variants, load/unload, activation, visibility, purposes, draw modes, scene cameras, render settings and AOVs, HUD, OCIO, bounding boxes, validation panel, preferences, usdview's command-line flags | A parity checklist built from `pxr/usdImaging/usdviewq` is complete. Selected `testusdview` scenarios are ported to the action API. |
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

## Testing

- `cc_test` for the core: document, actions, undo, camera math
- Python tests for the `vela` module
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
