# ROTGRAVE

The first game built in Warren, and the reason Warren exists: a
round-based survival shooter in a dead town, where the thing you carry
that nobody else has is **a gun that puts holes in the world**.

It ran once already, in Godot, as about fourteen thousand lines of
GDScript over a C extension that did the portals. That version is gone
— see *What came across* below. What it leaves behind is the part worth
keeping: a design that was played and tuned, written down as data.

## What came across

`design/rotgrave_design.json` is the whole of the previous version's
tuning, dumped by the code that was running it rather than copied out
by hand:

| | |
|---|---|
| `undead` | 11 species: speed, health, damage, reach, swing, radius, points, and the round-weights that decide the mix |
| `weapons` | 12 guns and the melee, with the mystery box weights and the Pack-a-Punch upgrade rules |
| `plan` | the town: 18 zones, 26 doors with their costs, 46 spawn points, the perk machines, the traps, the box spots |
| `rounds` | forty rounds of crowd size, spawn interval, alive cap, special rounds and the exact species mix, **sampled from the curves rather than transcribed** — a formula copied by hand is a formula copied wrong |

`design/DESIGN.md`, `design/TESTING.md` and the old `README.md` are the
prose that goes with it: what the mode is for, what each round is
supposed to feel like, and what was worth testing.

`design/ATTRIBUTION.md` is not optional reading. The bodies are CC0
MakeHuman builds and the animation is Quaternius's CC0 Universal
Animation Library; both have terms, and the file is the source of truth
for the credits screen.

`tools/gen_sfx.py` is the sound. It is pure Python and numpy and knows
nothing about any engine, so it came across unchanged.

## What did not

The Godot project, the GDScript, and the C portal extension. Warren has
portals of its own — properly, in the renderer, with an oblique near
plane rather than a staircase of perpendicular ones approximating it —
so the extension had nothing left to do.

A tarball of the whole Godot tree is at `~/rotgrave-godot-archive.tar.gz`
until the port is playable. It is not in this repository on purpose:
having two implementations of one game where a search can find both is
how an afternoon disappears.

## What the port needs from the engine

Warren does not yet have two things this game is built on, and they are
engine work rather than game work:

- **Skeletal animation.** Every survivor and eight of the eleven undead
  are the same nineteen-bone rig playing the same retargeted walk
  cycle. Warren imports bones from glTF and does not deform anything
  with them.
- **Navigation.** Thirty shamblers pathing round a town, with links
  between levels for ladders, walkways and roof edges.

Both are being built now; see the engine's own README.
