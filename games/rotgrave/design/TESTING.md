# Testing ROTGRAVE without playing it

There is no test framework here and there is not going to be one: almost
nothing in this project has a right answer a unit test could assert. What
it has instead is a set of **harnesses** — modes the game boots into that
drive themselves, photograph the result and print numbers — because the
failures that matter in a game of this shape are visual and emergent, and
a green test suite would not have caught a single one of them.

Every harness is a flag after `--` and writes its screenshots to
`~/.local/share/godot/app_userdata/ROTGRAVE/shots/`.

```sh
./godot4 --path godot -- <flags>
```

| flag | what it does |
| --- | --- |
| `--town [--fixed]` | builds the town with nothing alive in it and photographs eight views, plus a board of every material with and without its normal map |
| `--solo` | straight into a game, no menu |
| `--solo --shots` | ten photographs at three-second intervals with a line of telemetry each |
| `--bot` | a survivor played by nobody: aims, fires, reloads, retreats and buys |
| `--long` | forty samples instead of ten |
| `--open` | every zone unlocked and the power on |
| `--round=N` | start at round N (the pack is every fifth from six; a boss is every tenth) |
| `--tp` | third person |
| `--guns` | every weapon in the hand, one photograph each |
| `--bestiary` | one of every species in a row, with their measured heights |
| `--buy` | walk up to every purchase in the town and press the key |
| `--menu` | photograph the front end |
| `--host` / `--join` | two processes, a networked match, both sides logging what they see |
| `--portalcheck` | links a portal to a copy of itself turned half round, so the warp is the identity, and measures how far the view through it differs from the view without it |
| `--portalplay` | a crowd walked into a hole and dropped out of one thirty metres up |
| `--showcase` | portals and the hook photographed in use |
| `--plugin` | does the native extension load, register and agree with the engine's own transform maths |

## What each one has actually caught

These are not hypothetical. Every entry is a bug that shipped into a
build and was found by the harness named.

- **`--town`** — the ground rendering pure black. Godot's front face is
  the *clockwise* one, so the right-hand-rule normal points away from the
  visible side and every surface in the world was lit from behind.
  The material board next to it is what separated "bad tangent frame"
  from "bad light".
- **`--town`** — brick courses running vertically. A 1-D array broadcast
  along the wrong axis, twice, in two different texture functions.
- **`--solo --shots`** — the entire HUD stacked in the top-left corner. A
  `Control` parented to a `Node3D` has no rect to anchor against, and
  `set_anchors_preset` preserves the current (zero) rect in its offsets.
- **`--bot`** — fifty shots, zero hits. The undead are on physics layer
  three, which is the bit `1 << 2`; the trace mask said `1 << 3`.
- **`--guns`** — every weapon pointing at the sky. `MeshKit.cylinder`
  extrudes along +Y, which is right for a lamp post and wrong for a
  barrel.
- **`--buy`** — every purchase taking the points and changing nothing. An
  `rpc()` with no multiplayer peer is a silent no-op and `call_local` does
  not fire, so single-player broadcast nothing at all.
- **`--buy`** — one zone of eighteen never opening. Mill Row is reached
  only through free openings, and a free opening was not treated as part
  of the same purchase.
- **`--bestiary`** — a spider the size of the church. The scene bounds
  were measured through local transforms instead of global ones.
- **`--round=10`** — thirty frames a second. Thirty skeletons re-posed
  every frame, twenty-five of them eight pixels tall.
- **`--bot --long`** — a round that would not end. Two bodies left,
  nowhere near anybody, and nothing in the game to resolve it.

## And what the portal harnesses caught

`--portalcheck` is the interesting one, because it turns "does the
portal look right" — which is a judgement — into a number. Link a portal
to a copy of itself rotated 180° and the warp is exactly the identity, so
a correct renderer draws through the hole precisely what is behind it and
the portal is invisible. Render with it and without it, subtract.

It went 0.117 → **0.002** over four fixes, each of which was invisible
by eye and obvious in the number:

- **0.117** — screen position derived from `PROJECTION_MATRIX *
  MODELVIEW_MATRIX` in the vertex shader. Godot's projection carries a
  reverse-Z correction and a Vulkan clip-space Y that points the other
  way from OpenGL's. `SCREEN_UV` states the number instead of implying
  it.
- **0.048** — the portal view rendered with the main environment, so the
  frame was tonemapped, bloomed and colour-graded twice. Portal views now
  render linear with the screen-space pass off.
- **0.015** — the frustum not matching the render target's aspect after
  the pixel counts were snapped to a ladder and capped.
- **0.002** — and the last of it was the test itself: the player kept
  falling between the two captures, so the comparison was measuring
  gravity. The steep case, four metres in the air, measured five times
  the error of the others for that reason alone.

`--plugin` checks forty random transforms against the engine's own
`Transform3D * Vector3`, which is what proves the C structs are laid out
the way the engine lays its own out. A Basis stores ROWS; getting that
backwards transposes every portal transform in the game and looks almost
right.

`--portalplay` found the two that only a live crowd would:

- a portal is a hole in the *picture*, not in the collision mesh, so a
  body walking at one is stopped by the wall with its centre still a
  radius short of the aperture. It now tests a point a radius ahead of
  itself.
- and the point tested was the body's ORIGIN, which for a
  CharacterBody3D is between its feet — so a portal resting on the floor
  had its lower edge exactly at the height being tested. Ten shamblers
  walked through the middle of a six-metre hole and none of them noticed
  it. Measured at the body's middle: eight of ten through, and dead on
  landing.

## The one thing worth asserting

```sh
make check
```

Boots headless, parses every script, and fails on any parse or compile
error. Ten seconds, and it is worth running before every launch.
