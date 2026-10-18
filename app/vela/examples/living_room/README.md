# Living room

A small room for trying Vela's Claude panel with the requests from the
project README. Open `living_room.usda`, then Window → Claude (⌥⌘K).

What's in it:

- **Lights:** a ceiling sphere light, a window rect light, a reading disk
  light on the floor lamp, and a dome light for the sky, each with its own
  color.
- **Furniture:** a chair (seat, back and legs, with the fabric material
  bound to the seat and back), a table and a floor lamp.
- **Decor:** a vase and a book with no material bound, and a shelf with a
  plant that never shows in the render.

Things to ask:

- "make every light in this room 20% warmer"
- "add a variant set for the chair's fabric", then switch it in the
  inspector's Variants tab
- "which prims have no material bound?"
- Select `/Room/Decor/Shelf/Plant` and ask "why is this mesh invisible in
  the render?"

Each request that changes the scene is one undo step, so ⌘Z puts it back.
The file on disk changes only if you save.
