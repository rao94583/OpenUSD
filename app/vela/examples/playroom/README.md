# Playroom

A small scene for trying Vela. Open `playroom.usda` (File → Open, or drag
it onto Vela). The other two files are pulled in by it: `block.usda` is
referenced as instances, and `shelf.usda` is a payload.

What's in it, and what to try:

- **Animation, frames 1–96.** The ball bounces and the top spins. Press
  Space to play, or use ← and → to step a frame at a time.
- **Cameras.** The window opens looking through `main_cam`. View → Camera
  switches to `top_cam`. Option-drag to navigate (left button tumbles,
  middle tracks, right dollies), the way Maya does. Navigating while
  looking through a scene camera detaches a free copy, so the scene's
  camera isn't changed. F frames the selection; A frames everything.
- **Variants.** Select `/World/Toys/Top`. The inspector's Variants tab
  switches its color between blue, green and yellow.
- **Purposes.** The lamp shows as a plain cylinder: that's its proxy. In
  View → Display, turn on Render Purpose and turn off Proxy Purpose to see
  the detailed lamp instead. Guide Purpose shows the world axes at the
  origin.
- **Instancing.** The six blocks are native instances of one prototype.
  The outline's Prototypes checkbox shows the prototype.
- **Payloads.** Select `/World/Shelf`, then Prim → Unload Selected, and Load
  Selected to bring it back.
- **Editing.** Change a value in the inspector (try the ball's `radius`),
  hide or deactivate prims from the Prim menu, and undo with ⌘Z.
- **Python.** Window → Python Console (⌥⌘P), then try:

  ```python
  [p.GetPath() for p in vela.stage.Traverse()]
  vela.selection = ['/World/Toys/Ball']
  vela.stage.GetPrimAtPath('/World/Toys/Ball').GetAttribute('radius').Set(0.6)
  vela.run('set_draw_mode', mode='wireframe')
  ```

- **Claude.** Add Vela to Claude Code with the command in Settings → Claude
  and MCP, then ask, for example, "make the ball blue and twice as big" or
  "what's on the shelf?".
- **Validation.** Window → Validation checks the stage with USD's
  validators. This scene has no issues.
