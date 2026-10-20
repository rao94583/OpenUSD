# Vela

Native macOS USD viewer and editor. Requires macOS 13+, Xcode and
the repository's Bazel toolchain. It opens `.usd`, `.usda`, `.usdc` and
`.usdz` through `NSDocument` (File → Open, Finder) and shows the stage in a
Storm viewport rendered with Metal.

```sh
bazel build //app/vela:archive
bazel test //app/vela:all                  # core, menus, bundle, snapshots, MCP
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
ancestors of matching prims. Inactive prims and instance proxies are included. The filter menu narrows
the tree to active, loaded, model or visible prims while retaining ancestors.
Enable Prototypes to inspect USD-generated prototype roots; an instance or
instance proxy also exposes a Prototype button. Navigation reveals the
prototype by clearing the outline filters. Generated prototypes are read-only.
Tree selection and viewport picking share the same document selection.

The inspector tabs show Properties, Metadata, Composition, Layers and Variants.
Double-click a property for paginated array elements, relationship targets,
time samples or spline knots and tangent slopes. The Curve tab previews
scalar splines over their knot interval with bounded USD-evaluated samples.
Its Metadata and Opinions
tabs show property metadata and authoring across layers. Filter properties by
name or type in the inspector. Double-click Layers or Composition rows for
layer metadata and sublayer details. Large arrays show their count
in the property list. The inspector follows selection and time; multiple
selection inspects the last selected path. These views are read-only.

```sh
bazel test //app/vela:browser_test //app/vela:browser_ui_test
Vela --check-browser FILE           # controls/selection test; no shown window
```

Array previews cover all built-in Sdf array types in this build, including
half, quaternion, matrix, asset and path-expression arrays. Phase 4 still
needs composition graph/navigation and advanced spline/loop details. The layer tab currently
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

## View settings, cameras and files

View → Draw Mode, Complexity, Display (lights, materials, purposes,
culling), Background and Selection Highlighting match usdview's view
settings and defaults. View → Camera looks through a scene camera; any
navigation detaches a free copy. File → Save writes edited layers in
place; Export Flattened…, Export Session Layer… and Revert to Saved are
also in the File menu.

From a terminal, `Vela FILE` takes usdview's options (`--select`,
`--camera`, `--mask`, `--unloaded`, `--sessionLayer`, `--ff/--lf/--cf`,
`--complexity`, `--timing`, ...); `Vela --bogus` lists them.

## Exporting images and movies

File → Export Image… saves what the viewport shows (without the HUD) as
PNG, JPEG, TIFF, BMP, TGA or OpenEXR, chosen in the save panel's Format
menu. EXR files hold the renderer's linear color, before color correction.
File → Export Movie… asks for the frame range, frame rate, resolution and
codec. Resolutions: 4K UHD, 1440p, 1080p and 720p (16:9), DCI 4K and 2K
(1.90:1), Flat (1.85:1), Scope (2.39:1), 4:3, square, portrait (4:5),
vertical (9:16), the viewport's size, or a custom width and height. When
the camera's aspect ratio (a scene camera's aperture, or the viewport's
for the free camera) differs from the resolution's, the frame is
letterboxed or pillarboxed in black to keep all of it, or cropped to fill.
It renders each frame
through the viewport's camera (a scene camera, which may be animated, or
the free camera) into a .mov or .mp4: H.264, HEVC, ProRes 422 or ProRes
4444. A progress panel can cancel it. Both render offscreen at the size
asked for, so covering, minimizing or resizing the window doesn't change
the result, and they don't move the timeline. Python and MCP run them as
the `export_image` and `export_movie` actions:

```python
vela.run('export_image', path='/tmp/view.png', width=1600)
vela.run('export_movie', path='/tmp/turn.mov', start=1, end=96, codec='prores422',
         resolution='2160p', framing='fit')
```

## Window

The prim browser (left), inspector (right) and bottom panel each hide and
show from the View menu (⌃⌘S, ⌥⌘I, ⇧⌘Y), the toolbar's buttons, or by
dragging a divider to the window's edge. The bottom panel has three tabs:
Python, Log (⌥⌘L: USD's errors, warnings and status messages, failed
actions and MCP calls, filtered by level and text; its tab counts unseen
errors and warnings) and Claude (Phase 7).

The browser uses usdview's color coding. Prim names: orange for prims with
composition arcs (references, payloads, inherits, specializes, variant
sets), light blue for instances, periwinkle inside prototypes, dimmed when
inactive; bold for `def`, italic for `over`, regular for `class`. Property
names and values are colored by where the value comes from (default, time
samples, spline, value clips, schema fallback, none) and relationships are
red; italic values are interpolated. The palette buttons above the outline
and inspector, and Help → Color Legend, explain them. Right-click a
relationship (or a connected attribute) to go to its targets; double-click
a target in the property window does the same.

## Python and Claude

Window → Python Console (⌥⌘P) opens the bottom panel's interpreter for
the window's document, replacing usdview's. `vela.stage` is the document's own `Usd.Stage`, so
edits appear in the window at once; `vela.selection` and `vela.time` can be
read and assigned, `vela.run(action, **params)` runs any menu command and
`vela.actions()` describes them. Usd, Sdf, Gf, Vt, Tf, UsdGeom, UsdShade,
UsdLux, UsdUtils and Kind are imported. Each input's edits are one undo
step, named "Python". Return runs a complete statement (a block runs on a
blank line), Shift-Return adds a line, ⌘Return runs everything, ↑ and ↓
browse history.

Window → Claude (⌥⌘K) opens the Claude panel: describe what you want, and
Claude Code does it in this document. Ask questions ("which prims have no
material bound?", "why is this mesh invisible in the render?") or for
changes ("make every light in this room 20% warmer", "add a variant set for
the chair's fabric"). Claude has the whole USD Python API and every Vela
action. Its replies stream in. Each tool call shows as a row you can expand
to see the code and its result, and viewport screenshots show as
thumbnails. The camera button attaches an image of the viewport to a
request. Each request carries the selection, edit target and time, and
becomes one undo step ("Claude: …"), which the panel's Undo button or ⌘Z
reverts.

The panel works like Claude Code's editor panels:

- **History:** the clock button lists this document's earlier
  conversations, newest first, with a search field. Click one (or Return)
  to open it: the transcript replays and the conversation continues where
  it left off. Delete or the context menu removes one. "+" starts a new
  conversation.
- **Stop:** while Claude works, a spinner and a Stop button show in the
  header, and the send button becomes a stop button. Escape stops too, as
  does the `stop_claude` action. You can keep typing: Return queues the
  message, which sends when Claude finishes. Stop puts it back in the
  input.
- **Model, effort and edit mode:** the menus under the input choose the
  model (Claude Code's default, Opus, Sonnet, Haiku or Fable), the effort
  (default, low, medium, high, extra high or max) and whether Claude edits
  automatically or asks first. Vela remembers these, and Settings → Claude
  and MCP shows them too. A new model or effort applies from the next
  message; the conversation carries on.

Return sends, and Shift-Return adds a line. The command palette sends text
that matches no command to Claude ("Ask Claude: …"), and the
`ask_claude` action does the same from scripts.

The panel runs your `claude` CLI (Claude Code 2.1.296 or later, logged in),
which Vela finds in `~/.local/bin`, Homebrew or your shell's `PATH`.
Settings → Claude and MCP can point it elsewhere. Questions never need approval;
edits do only with that setting. Anything outside Vela's tools always asks.

Image generation is configured through Settings → Image Generation Settings….
Enable it, choose OpenAI (GPT Image) or Google (Gemini), and save that provider's
API key in Keychain. Then ask Claude to generate a texture or create a concept
from the viewport. Image requests have their own approval setting and API
billing. Window → Generated Images… shows approvals, job status, references,
previews, Save As and Continue with Claude. Window → Generate Image… also works
without Claude. Save textures into your project before asking Claude to bind
them; generating an image itself does not edit the USD scene.

The provider interface supports adding other hosted or local models; Hugging
Face and local adapters are not included yet. ChatGPT-plan image generation is
currently unsupported by its integration API. See
[IMAGE_GENERATION.md](IMAGE_GENERATION.md) for architecture, supported options,
validation and limitations. Tests use fixture providers without network calls;
live image output requires a configured provider account.
The CLI gets Vela's tools on a private socket bound to the window's
document, Read, Glob and Grep for the scene's files, Write and Edit for
plugins, and Vela's skills, and nothing else.

The skills are a Claude Code plugin in the bundle
(`Contents/Resources/ClaudePlugin`), and Claude loads one when a request
needs it (a "Skill" row in the panel):

- `vela:usd-python`: USD Python idioms and pitfalls (time samples,
  stronger opinions, bindings, variants, change blocks), with a reference of
  every schema in this build: each one's attributes, types, fallbacks,
  allowed tokens and Python getters.
- `vela:ui`: driving Vela with its actions, with a reference of every
  action, including your plugins' commands.
- `vela:plugins`: writing plugins (below).
- `vela:image-generation`: generating images, while that is turned on.

Each time the panel starts the CLI, it copies the plugin to
`~/Library/Application Support/Vela/Claude/Skills` and writes the actions
reference there. Your own Claude Code skills and plugins load too.
The panel keeps its own record of each conversation in
`~/Library/Application Support/Vela/Claude/`. The CLI runs in that
folder's `Workspace` subfolder (with `--add-dir` for the scene's folder),
so Claude Code keeps all of Vela's sessions in one project under
`~/.claude/projects/`, whatever folder the scene is in, and no `CLAUDE.md`
near the scene applies. Try it on
[examples/living_room](examples/living_room/README.md).

Vela is also an MCP server, so Claude Code in a terminal can query and edit
the open scene. Settings → Claude and MCP shows the command to add it (and
can turn the server off):

```sh
claude mcp add vela -- '/Applications/Vela.app/Contents/MacOS/vela-mcp'
```

`vela-mcp` bridges the client's stdio to a Unix socket the app listens on
(`~/Library/Application Support/Vela/mcp.sock`, mode 0600, this user only);
no TCP port opens. If Vela isn't running, the first message launches it.
Tools: `get_app_state`, `list_actions`, `ui_action`, `run_python` (a
persistent namespace, as in the console), `query_python` (the same, but
read-only: any edit is reverted and reported), `viewport_screenshot` and
`open_document`. Edits are undoable like any other. The server speaks both
MCP protocol eras: the stateless 2026-07-28 revision (per-request `_meta`,
`server/discover`) and the `initialize` handshake of 2025-11-25 and
earlier.

## Plugins

Vela loads plugins the way usdview does, with the same API under Vela's
names, so usdview plugins port by changing their imports. A plugin is a
Python package with a `plugInfo.json` that declares a container type
derived from `vela.PluginContainer`. USD's plugin registry finds it in
`~/Library/Application Support/Vela/Plugins` (Plugins → Show Plugins
Folder) or on `PXR_PLUGINPATH_NAME`:

```json
{"Plugins": [{"Type": "python", "Name": "scene_stats", "Info": {"Types": {
    "scene_stats.Container": {"bases": ["vela.PluginContainer"]}}}}]}
```

The container's `registerPlugins(registry, plugCtx)` calls
`registry.registerCommandPlugin(name, displayName, callback, description)`,
and `configureView(registry, builder)` adds commands to menus with
`builder.findOrCreateMenu('Tools').addItem(command, shortcut='Ctrl+K')`,
`findOrCreateSubmenu` and `addSeparator`. A menu's title can name one of
Vela's menus (Edit, View, …) or a new one, placed before Window. Each
command becomes a Vela action: it shows in the command palette, and
`vela.run`, MCP and Claude can run it. Its callback gets the
`vela.Document` it runs on, its printed output goes to the Log, and its
edits are one undo step.

Plugins can make AppKit UI with PyObjC (`pyobjc-core` and
`pyobjc-framework-Cocoa` are in the bundle). Objective-C classes can't be
defined twice, so for button and menu targets use
`vela.Target(callback)` rather than subclassing `NSObject`, which would
break reloading. Plugins → Reload Plugins (the `reload_plugins` action)
finds new plugins and reloads changed ones, and reports errors with their
tracebacks in the Log.

Plugins can use Python packages the bundle doesn't have. They declare
them in `plugInfo.json`, next to `Types`: `"PythonRequires": ["numpy>=2"]`.
A plugin whose packages aren't installed waits. At launch Vela offers to
install them, and Plugins → Install Packages for Plugins does it later.
Vela always asks first, listing the packages and where they come from.
Then the bundled pip installs them for your user account into
`~/Library/Application Support/Vela/Python/3.11` (Plugins → Show Python
Packages Folder), with a progress window and Cancel, and the plugin loads.
Scripts can ask too: `vela.install_packages('requests')` (the
`install_python_packages` action).

- **Search order:** Python searches USD's `pxr` first, then the bundle's
  own packages, then yours. The bundle's packages can't be replaced:
  asking for another PyObjC version fails. Packages that bring a second
  USD (`usd-core`) are refused.
- **One shared set:** every install resolves all plugins' requirements
  together, so a request that conflicts with what a plugin needs fails
  rather than breaking the plugin.
- **Your pip configuration still applies:** its index, certificates and
  proxy.
- **Reset Python Packages** deletes the installed packages and installs
  again what plugins and scripts asked for. The folder is per Python
  version, so a Vela with a newer Python starts afresh.

Claude can write plugins: ask the Claude panel for a command, menu item
or tool window that should stay in Vela. It writes the package into the
Plugins folder, reloads plugins and tries the command. Writing files in
that folder follows the panel's edit mode, and writing anywhere else
always asks. [examples/plugins/scene_stats](examples/plugins/scene_stats)
is a tool window Claude wrote from a one-line request; copy it into the
Plugins folder to try it.

## Command line

```sh
Vela --check-document FILE          # print the prim count
Vela --check-saving FILE PRIM       # edit and save through the document
Vela --check-python FILE            # the embedded interpreter and vela module
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
The bundle identifier is `org.openusd.vela`.

### Sharing Vela

To give Vela to someone else, build the release package:

```sh
bazel run --config=release //app/vela:package
```

This writes `dist/Vela-<version>.dmg` (about 85 MB): the optimized app
(`-c opt`) and a shortcut to Applications to drag it to. The app needs
nothing installed: USD, Python 3.11, PyObjC and every plugin are inside
it. It runs on Apple silicon Macs with macOS 13 or later. The Claude panel
needs Claude Code installed and logged in, and plugins that need Python
packages download them.

Without an Apple Developer ID, the app is signed ad hoc. macOS blocks the
first launch of a downloaded copy: open it once, then choose **Open
Anyway** in System Settings → Privacy & Security. With a Developer ID,
sign and notarize it so it opens without warnings:

```sh
# Once: save App Store Connect credentials for notarytool.
xcrun notarytool store-credentials vela-notary --apple-id YOU --team-id TEAM
bazel run --config=release //app/vela:package -- \
    --identity "Developer ID Application: NAME (TEAM)" --notarize vela-notary
```

`package.py` signs every Mach-O file inside-out with the hardened runtime,
using `Vela.entitlements` for the executables
(`disable-library-validation`, because plugins import packages that pip
installs on the user's Mac). Notarizing submits the app, then the disk
image, and staples both tickets, so the app opens offline after it is
copied out. A notarized copy opens with only macOS's usual "downloaded from
the Internet" confirmation. Allow `codesign` to use the key ("Always
Allow") the first time, or it asks once per file.

### What's inside

The bundle contains the existing monolithic USD install layout and the
hermetic Python 3.11 runtime under `Contents/Resources/Runtime`, and
Claude's skills under `Contents/Resources/ClaudePlugin`. The native app
and Python extensions link the same bundled `libusd_ms.dylib`. A
compilation-only header interface prevents statically linking another copy
of USD into the app. Runtime copies dereference Bazel symlinks.
`finish_bundle.py` (the app's post-processor) removes what the app never
uses (rules_apple's second copies of the linked libraries in
`Contents/Frameworks`, debug symbol bundles, PyObjC's test suite) and
ad-hoc signs the nested Mach-O files before the outer app is signed.

Python never writes into the bundle, since a changed file breaks the app's
signature: its bytecode (`.pyc`) goes to `~/Library/Caches/Vela/Python`
(`pycache_prefix`), for Vela's interpreter and the pip it runs. The icon
is `icon/Vela.png`, a 1024-pixel master on Apple's icon grid, turned into
`Vela.icns` at build time.

The bundle test extracts the signed archive into a temporary directory with
spaces and checks what ships (no `Frameworks` copies, debug symbols,
PyObjC tests or bytecode; the icon). In a clean environment with an empty
home folder, it runs the bundled interpreter in isolated mode, checking each
plugin manifest against the registry, loading every plugin, and creating
and opening all four supported formats through Python and the native
document reader. Then it runs Vela's Python check with dyld's library log,
and requires every library to load from the bundle or the system, with
one copy of USD and of Python. It verifies the bundle signature before and
after, and that the bytecode went to the cache. The native reader's
`--check-document PATH` mode uses the same document code as the window and
prints the prim count without starting the AppKit event loop.

The bundle test does not automate Finder or visually inspect the window.
Offline builds require refreshing `bazel vendor` for the new Apple rules.
