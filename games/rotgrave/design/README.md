# ROTGRAVE

**Four survivors hold a dead town against rounds of the risen.**
Co-operative first- and third-person survival, built in Godot 4.7.

Hollow Ridge is a hundred and thirty metres of motel, church, diner,
garage, clinic and storm drain. You start in the motel courtyard with a
1911 and five hundred points. Every round, more of them come, and they
come harder. Points buy doors, doors buy ground, and ground buys the
circuit you need to keep running when the round you are on stops being
survivable standing still.

---

## Running it

```sh
make plugin       # build the C portal/grapple extension (once)
make run          # play
make run-mp       # a host and a client on this machine, two windows
make shots        # photograph the town without playing it
make soak         # drive a bot through forty rounds and print the numbers
```

Or directly:

```sh
./godot4 --path godot                       # menu
./godot4 --path godot -- --solo             # straight into a game
```

The engine binary is not in the repository. Any Godot 4.7 build works:

```sh
make engine       # fetches it
```

## Controls

| | |
| --- | --- |
| `W A S D` | move |
| `Shift` | sprint (costs stamina) |
| `Ctrl` | crouch |
| `Space` | jump |
| Left mouse | fire |
| Right mouse | aim down sights |
| `R` | reload |
| `F` | use — buy, board a window, pick somebody up |
| `Q` | swap weapon |
| `V` | knife &nbsp;·&nbsp; `Ctrl+V` first/third person |
| `G` | grenade |
| `1` / `2` | place the orange / blue portal — **hold to make it bigger** |
| `3` | close both portals |
| `E` | grapple: hold to fire and reel, release to let go |
| `Tab` | scoreboard |
| `Esc` | pause |

## Portals and the hook

Two of the tools are a **GDExtension written in C** (`plugin/`,
[its own README](plugin/README.md)). Build it with `make plugin`; without
it the game runs exactly as before, minus these two things.

`make portalcheck` photographs a portal against the same scene without
one and measures the seam. Head on it is 0.0034 mean absolute error and
indistinguishable; at a steep angle it is 0.0451, which is over the
harness's own 0.03 bar. That last number is not a bug to be found — it
is the limit of approximating an oblique near plane when the engine
will not take an arbitrary projection matrix, and it is what the
plugin's README explains in full.

**The portal device** puts a hole in a wall. Tap `1` and you get a
sixty-centimetre letterbox; hold `1` and you get five metres of open
wall. The two ends of a pair need not match, and that is the whole
mechanic.

**Size is carried through the transform, so an unequal pair is a size
machine rather than a funnel.** A body that walks into a four-metre
portal and out of a one-metre one comes out a quarter the size, moving a
quarter as fast — it does not squeeze, it *shrinks*, and it stays that
way. So a horde driven into the big arch arrives as something you can
step over and shoot at your leisure; one shambler sent the other way
arrives four times the size, hitting harder and reaching further, which
is the reason to think before you open that end.

The same applies to you. Walk into the small one and you come out big,
with a longer stride, a longer grapple and a camera near plane to
match; walk back and you are small again. The portals are always
flush — nothing is ever wedged in an aperture — because it is the
traveller that changes, not the hole.

And a portal is a hole in the **route**, not only in the picture. Put one
on the courtyard wall and the other over a thirty-metre drop and the
round walks into it and falls to its death; the points are credited to
whoever opened the hole. The town has a church balcony, a crypt, rooftops
and a storm drain, and all of them are a long way down from somewhere.

**The hook** (`E`) fires a grapple, and the rope is the shortest path to
the anchor that does not pass through the world — it catches on corners
and comes off them again as you swing. It can be fired **through** a
portal: the rope's first pivot is then the aperture, so it hauls you at
the hole, through it, and on towards an anchor you cannot see.

## The mode

**Rounds.** A round is a fixed crowd. Kill them all and there is a nine
second breath before the next, which is bigger. Every fifth round from six
is the pack — nothing but hellhounds. Every tenth is a boss.

**Points** are the score and the currency at once. A hit pays 10, a kill
60, a headshot 100, a knife 130, picking somebody up 200, and a board back
on a window 10 — which is what the first three rounds are for.

**Doors.** Eighteen of them, 750 to 1750 points. Every zone they open pays
for itself with a weapon, a machine, a trap or a shortcut, and every zone
closes a **loop** — there is no room in Hollow Ridge with one way out.

**The power** is one lever in the clinic basement, four purchases deep on
the far side of the town. Until it is thrown there are no perks but Quick
Revive, no traps and no Pack-a-Punch.

**The box** is 950 points for a gun you did not choose, and it moves after
a few uses — which turns buying a weapon into going and finding where the
weapon is.

**Going down.** At zero health you are on the floor with a pistol and
thirty-two seconds. Anybody who reaches you puts you back up. Alone, Quick
Revive picks you up once; without it, alone, that is the run.

## Multiplayer

Up to four. **Host a game** and give the others the address the lobby
prints; they **Join** it. The server owns the world, the round, the
economy and every one of the risen; each player owns their own position.
Shots are traced by the client that fired them and checked by the server.

Nobody sends geometry — the town is a seed.

## What is in here

```
plugin/      the portal and grapple GDExtension, in C
godot/
  core/      boot, settings, the round director, sound
  net/       the session
  world/     the plan, the builder, the materials, everything you buy
  actors/    the figure loader, the animation bank, the survivor, the risen
  weapons/   eleven guns as numbers, and their meshes
  ui/        the HUD, the menu, the pause screen
  fx/        tracers, blood, decals, blasts
  assets/    bodies, animation, audio, generated textures
tools/       the generators for sound and for every texture in the game
docs/        design notes
```

Nothing in `assets/` that can be generated is committed as a binary:
`make assets` rebuilds every texture and every sound from the scripts in
`tools/`.

## Credits

Bodies from MakeHuman, animation and creatures from Quaternius, both CC0;
music from the author's own earlier project. Everything else — the town,
the textures, the sound, the code — is generated or written here. Full
account in [ATTRIBUTION.md](ATTRIBUTION.md).
