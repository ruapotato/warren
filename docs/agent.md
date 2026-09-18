# Driving Warren from a program

The editor is for a person. This is the same engine addressed the way
a program addresses it: one JSON object in, one JSON object out.

```
$ warren --agent --demo portals
{"commands":[...],"protocol":"one JSON object per line in, one per line out..."}
```

Commands go in on stdin, one per line. Replies come out on stdout, one
per line. Everything the engine logs goes to stderr instead, so the
protocol stream is only ever replies.

```json
{"cmd": "find", "class": "Light3D"}
{"cmd": "set", "path": "/root/Demo/Sun", "property": "energy", "value": 6}
{"cmd": "screenshot", "path": "look.png"}
```

Every reply has `ok`. A failure has `error` and a machine-readable
`code`, and usually more than that — see **When you get it wrong**.
An `id` on a command is echoed on its reply, so a caller that
pipelines does not have to count lines.

## Why it is not a separate layer

Warren has one class registry. It already generates the Python
bindings, the `.pyi` stubs, the property inspector, scene
serialisation and network replication. The agent interface is the
sixth reader of the same table.

That is the whole design. It means a property added to a node this
afternoon is settable from a script, visible in the inspector, saved
in a scene file **and** promptable — with nothing written and nothing
regenerated. An interface maintained alongside the engine would drift
out of date with it by the next release. This one cannot.

## Finding out what exists

```
$ warren --schema -            # every class, as JSON, on stdout
$ warren --schema api.json     # or to a file
```

No window, no GPU, nothing started: the registry is all it needs. The
dump is 43 classes, 167 properties and 105 methods, and for each it
gives the type, the range where there is one, whether it is writable,
and — because reflection kept them — the **names** of every method
argument.

```json
{
  "name": "AudioPlayer",
  "base": "Node",
  "instantiable": true,
  "properties": [
    {"name": "volume", "type": "float", "hint": "range:0,4", "min": 0, "max": 4},
    {"name": "pitch", "type": "float", "hint": "range:0.25,4", "min": 0.25, "max": 4},
    {"name": "loop", "type": "bool"},
    {"name": "autoplay", "type": "bool"}
  ],
  "methods": [
    {"name": "play", "args": [], "returns": "null"},
    {"name": "stop", "args": [], "returns": "null"},
    {"name": "is_playing", "args": [], "returns": "bool"}
  ]
}
```

A class lists only what it declares. `{"cmd": "schema", "class":
"OmniLight3D", "inherited": true}` folds in the base classes'
members, which is what you want when you are about to set a property
and do not care where it was declared — `energy` and `colour` are on
`Light3D`, and an agent setting them does not need to know that.

## The commands

| | |
|---|---|
| `help` | every command, its arguments and what it returns |
| `schema` | the API; `class` for one, `inherited` to fold in bases |
| `classes` | just names and bases; `base` narrows to one subtree |
| `tree` | the scene tree from a node down |
| `get` | one property, or **every** property when none is named |
| `set` | one property, or several at once with `properties` |
| `call` | a method; `args` by position or keyed by argument name |
| `create` | a node, with its properties, in one command |
| `delete`, `reparent` | `reparent` keeps the world transform |
| `find` | by class, by name substring, or by group |
| `eval` | Python in the engine's own interpreter |
| `step` | advance whole frames |
| `screenshot` | render and write a PNG |
| `save_scene`, `load_scene` | `.mfs` files |
| `stats` | frame times and what the renderer is doing |
| `shapes`, `make_mesh` | procedural geometry — see below |
| `surfaces`, `make_material` | procedural textures — see below |

`get` with no `property` returns the lot. An agent that has just found
a node usually wants to see it, and one round trip instead of twenty
is the difference between an interface that is pleasant to drive and
one that is not.

Only `step`, `screenshot` and `stats` need a running engine. The rest
work against a scene tree on its own, so a build step or a test can
use the same commands headless.

## Being liberal about what comes in

A vector may be an object, an array, or one number:

```json
{"x": 1, "y": 2, "z": 3}      [1, 2, 3]      2
```

A partial object patches: `{"y": 2}` on a position moves it up and
leaves x and z alone. A colour may be `"#ff8800"`. A transform may be
written the way somebody describes one, even though that is not how
one is stored:

```json
{"position": [0, 1, 0], "rotation": {"yaw": 90}, "scale": 2}
```

Rotations read back with their axes named — `yaw`, `pitch`, `roll` —
rather than as x, y and z. The engine's internal `euler` packs the
three angles in the order it applies them, so its `.x` is a turn about
Y; defensible in the maths and indefensible on a wire.

## When you get it wrong

This is the part that matters, and the reason this is usable by a
model rather than only by a program. A wrong guess is **answered**,
not refused.

```json
{"cmd": "set", "path": "/root/World/Lamp", "property": "color", "value": "#fff"}
→ {"ok": false, "code": "no_such_property",
   "error": "OmniLight3D has no property \"color\"",
   "did_you_mean": ["colour"]}
```

```json
{"cmd": "create", "class": "OmniLight", "parent": "/"}
→ {"did_you_mean": ["OmniLight3D"], ...}
```

```json
{"cmd": "get", "path": "/World/Player", "property": "position"}
→ {"did_you_mean": ["/root/PortalDemo/Player"], ...}
```

That last one matches on the **last segment** of the path, because a
caller with the wrong path usually has the right name.

Two more that are worth knowing about:

```json
{"cmd": "set", "path": "/root/SomeNode3D", "property": "energy", "value": 3}
→ {"property_exists_on": ["OmniLight3D", "SpotLight3D", ...], ...}
```

The name was right and the node was wrong — which a spelling
suggestion cannot help with.

```json
{"cmd": "call", "path": "/root/Lamp", "method": "set_position", "args": [[0,0,0]]}
→ {"but_there_is_a_property": ["position"], "use": "the set command", ...}
```

Nearly everything Godot exposes as `set_x()`/`get_x()` is a property
here, so this is the commonest miss there is.

A batched `set` that fails partway names the property it failed on and
reports what had already been applied, so the caller knows the state
rather than having to go and read it back.

---

# Shapes

Procedural geometry, so that most of a game's props can be written
rather than modelled. Send `{"cmd": "shapes"}` for the grammar; the
engine generates it from the same table the parser uses.

A shape is a **signed distance field** — a function from a point to
how far it is from the surface — contoured into a mesh. Booleans on
fields are `min` and `max`, so they cannot fail. Cutting one triangle
mesh out of another needs exact predicates and still falls over on
coplanar faces, which is what you get when you subtract a box from a
box, which is what everybody does first.

```json
{"cmd": "make_mesh", "parent": "/root/World", "name": "Crate", "detail": 64,
 "shape": {"op": "difference", "of": [
     {"shape": "box", "size": [1, 1, 1], "round": 0.06},
     {"shape": "box", "size": [0.8, 0.8, 1.2]}]}}
```

One object per node. `shape` for a primitive, `op` with `of` for a
combination, and any modifiers on the same object.

**Primitives** — all centred on the origin.
`sphere` (radius) · `box` (size, round) · `cylinder` (radius, height,
round) · `capsule` (radius, height) · `cone` (radius, height, round) ·
`torus` (major, minor) · `half_space` (normal, offset).

`round` takes the corners off **without growing the shape**: a 1 m
crate with soft edges is still 1 m.

**Operations** — `union`, `intersection`, `difference`, each taking
any number of operands, plus `blend` in metres.

A blend is the reason to model this way. It fills the crease between
two shapes with a fillet and leaves the far side of each exactly where
it was — which mesh booleans cannot do at all.

**Modifiers**, applied in the order they read, with placement last.
`at` · `rotate` (degrees) · `scale` · `round` · `shell` · `twist`
(turns per metre about Y) · `bend` · `displace` (fractal noise, which
turns a sphere into a rock) · `elongate` · `mirror` · `repeat` ·
`material`.

`half_space` and an uncounted `repeat` are unbounded, and say so
rather than trying to mesh the universe. Intersect them with something
first — which is how you slice a shape flat.

From Python, the same parser:

```python
import warren
mesh = warren.shape({"op": "union", "blend": 0.12, "of": [
    {"shape": "torus", "major": 0.35, "minor": 0.1},
    {"shape": "cylinder", "radius": 0.1, "height": 0.9, "at": [0, 0.45, 0]}]})
```

## Why the distances have to be honest

Every deformation — twist, bend, displace — breaks the property the
contourer relies on, that the field changes by at most a metre per
metre, unless it is corrected. `tests/test_procgen` measures it over
every shape in the library and 4000 point pairs each. It caught
`displace` overstating by 1.69×.

The correction is free: dividing a field by a constant cannot move its
zero set, both ends of an interpolated edge shrink together, and the
gradient is normalised before it is used as a normal. Adding one
changed nothing on screen and made the field a distance again.

---

# Surfaces

A surface is **one scalar field over the unit square, coloured at the
end**. Send `{"cmd": "surfaces"}` for the grammar.

```json
{"cmd": "make_material", "on": "/root/World/Wall", "size": 512,
 "surface": {"pattern": "bricks", "rows": 8, "columns": 4, "mortar": 0.06,
             "colours": [[0, "#4a4440"], [0.55, "#8a4f38"], [1, "#b57a55"]],
             "roughness": [0.95, 0.65], "bump": 1.4, "tile": 2}}
```

The same field becomes the albedo through the colour ramp, the normal
map through its slope, and the roughness through its value. So the
mortar comes out rougher than the brick because it is darker, without
anybody saying so twice.

**Patterns** — `noise` · `ridged` · `cells` · `checker` · `bricks` ·
`stripes` · `gradient` · `radial` · `dots` · `wave` · `constant`.

**Combining** — `{"op": "blend", "of": [...], "mode": ...}` with
`mix`, `multiply`, `add`, `subtract`, `min`, `max`, `screen`,
`difference` or `overlay`.

**Shaping** — `invert` · `contrast` · `brightness` · `power` ·
`threshold` · `range` · and `warp`, which pushes the lookup around
with another pattern. A sine wave warped by noise is convincing wood
grain in one line.

**Making it a material** — `colours` (a ramp, either bare colours
spread evenly or `[position, colour]` pairs) · `roughness` (a number,
or `[at black, at white]`) · `metallic` · `occlusion` · `bump` ·
`tile`.

Everything tiles. The noise wraps its lattice rather than blending its
edges, so there is no seam and no loss of detail. `gradient` is the
deliberate exception: it runs from one end to the other, so it cannot
join up with itself, and it is a mask rather than a surface.

Procedural materials are **triplanar** by default: projected from
three directions and blended by the normal, needing no UVs. Contoured
geometry has none worth having. `tile` is then repeats per metre,
which is also the more useful thing to be able to say. Pass
`{"uv": true}` for geometry somebody unwrapped.

Without a running engine there is nothing to upload to, so
`{"save": "brick"}` writes `brick_albedo.png`, `brick_normal.png`,
`brick_orm.png` and `brick_height.png` instead.

---

# A whole prop, in eight commands

```json
{"cmd":"make_mesh","name":"Pillar","parent":"/root/World","detail":64,"at":[0,0.8,-3],
 "shape":{"op":"union","blend":0.04,"of":[
   {"shape":"cylinder","radius":0.22,"height":1.5},
   {"shape":"cylinder","radius":0.34,"height":0.12,"at":[0,0.72,0]},
   {"shape":"cylinder","radius":0.34,"height":0.12,"at":[0,-0.72,0]}]}}
{"cmd":"make_material","on":"/root/World/Pillar","size":256,
 "surface":{"pattern":"bricks","rows":10,"columns":4,"mortar":0.06,
   "colours":[[0,"#43403c"],[0.5,"#9a968c"],[1,"#cfcabd"]],
   "roughness":[0.95,0.7],"bump":1.3,"tile":2}}
{"cmd":"step","frames":10}
{"cmd":"screenshot","path":"pillar.png"}
```

No artist, no unwrapping, no files. The screenshot closes the loop:
whatever is driving the engine can look at what it built and decide
whether it is right.
