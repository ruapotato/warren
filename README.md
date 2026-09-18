# Warren

A game engine for spaces that are locally Euclidean and globally
whatever you wire them to be.

*A warren is a network of connected tunnels, which is what a portal
graph is and what the voxel terrain digs.*

**Portals are not a feature bolted onto Warren. They are the reason
it exists**, and the engine is built from the bottom up so that they
can be done the right way — which turns out to require decisions in the
projection maths, the clip space, the depth format and the render
hardware interface that no engine makes unless it means it.

C++20 core, Python scripting, Vulkan **and** OpenGL, Windows and Linux.
AGPL-3.0 licensed.

---

## Why it exists

The correct way to draw a portal has been known since 2007: mark the
aperture in the stencil buffer, redraw the world from a warped camera
inside that mask, and clip that inner view with a near plane lying
*exactly in the destination aperture* so nothing in front of it can be
drawn. It is exact. There is no seam, no resolution to choose, no
texture to filter, and the anti-aliasing matches the rest of the frame
because it *is* the rest of the frame.

It needs two things from an engine:

1. **Eight bits of stencil**, on the main depth buffer.
2. **A camera that is a matrix**, so the near plane can be oblique.

Mainstream engines give you the first and not the second. A camera is a
field of view and two clip planes, and the matrix is built behind your
back. Everything that makes portals hard elsewhere — the wall behind
the far portal creeping into shot, the wedge of view traded away to
hide it, the resolution ladders and clip biases that manage the
compromise — is downstream of not being allowed to touch sixteen
floats.

So in Warren, `Camera3D` holds a `Projection`. `perspective()` and
`orthographic()` are constructors for one, not the definition of a
camera. And `Projection::with_oblique_near(plane)` is a public method
that any code may call.

## What is different, concretely

**One clip space for both backends.** Depth in `[0, 1]`, reversed, +Y
up. Vulkan gets a negative-height viewport; OpenGL gets
`glClipControl`. One projection matrix drives both, no shader contains
a per-backend branch, and `tests/test_backend_parity` renders the same
scene through both and requires **zero** differing pixels.

**Reverse-Z, and it is measured.** Two surfaces a centimetre apart at a
kilometre are separated by **138 float ULPs** under reverse-Z and by
**0** — the same float — under the standard mapping. The depth-stencil
format is `D32_SFLOAT_S8_UINT`, the only one that is both float (for
reverse-Z) and stencil-bearing (for portals); an adapter without it is
rejected at start-up rather than rendered to incorrectly.

**The oblique near plane, re-derived.** Lengyel's published form
assumes OpenGL's `[-1, 1]` depth. Warren derives it again from the
clip condition itself, which is shorter and more general: it works for
off-axis frusta, for an already-oblique projection (a portal seen
through a portal) and for orthographic ones (a shadow cascade clipped
to a portal). `tests/test_math` sweeps 408 projection/plane
combinations and checks that a point on the plane lands exactly on the
near plane, that the right half-space survives, and that the x/y image
is bit-identical to the unmodified projection.

**Portals carry a size ratio.** Both ends of a pair need not be the
same size. The warp between a two-metre ring and a six-metre one is a
similarity transform with a factor of three in it, so walking through
makes you three times the size and the view through is magnified to
match. An unequal pair is a size machine, not a funnel.

**One shader source.** GLSL 4.50 with Vulkan semantics, compiled to
SPIR-V with `glslangValidator` for Vulkan and cross-compiled from that
same SPIR-V to GLSL 460 with `spirv-cross` for OpenGL. The two backends
cannot drift, because one is built from the other's output.

**Shadow cascades know about portals.** Before anything is drawn the
renderer walks the portal recursion without drawing it, and fits the
cascades to the union of every view the frame will render. An engine
that fits them to the main camera leaves everything seen through a
portal either unshadowed or shadowed by a cascade meant for somewhere
else — and in this engine that is most of what the player is looking
at. The light is uploaded through the same uniform block a camera
uses, so the matrix that rendered the map and the matrix that samples
it cannot drift apart.

**The environment is the sky, and it cannot disagree with it.** Image-
based lighting is baked from the same `sky_radiance` the backdrop is
drawn with — six faces, a cosine convolution for diffuse and a GGX
prefilter per mip for specular — and rebaked when the sun moves, which
in a static scene is once. Nothing to author and nothing to ship. The
second half of the split-sum is Lazarov's analytic fit rather than a
lookup table: four multiply-adds, and one less texture to bind with
the wrong filter.

**Punctual lights cast.** Spots and omnis get shadows from one atlas
of square tiles — a spot takes one, an omni six — and the atlas is
**cached on a hash of every caster and every casting light**, because
a lamp bolted to a wall in a room made of walls produces the same six
depth images every frame for ever. A static scene bakes it once.

**So do the light clusters.** Punctual lights are binned into a froxel
grid, and the grid is built **per view**: a portal view is a different
camera looking at different geometry through the same pixels, so it
gets its own. A lamp forty metres away in the far room lights what is
seen through the hole and nothing around it. `tests/test_lights`
renders exactly that and measures 0.884 inside the aperture with a
grid for that view and **0.000** without.

**And so does sound.** A generator humming in the far room is forty
metres away through the rock and three metres away through the arch,
and it is heard at three — arriving *from the arch*, panned to where
the aperture actually is. Every engine attenuates by distance; in a
world with portals the straight-line distance is the wrong number and
the straight-line direction is the wrong direction, and getting it
wrong does not sound like a bug, it sounds like the portal is a
picture. `tests/test_audio` measures 80.0 m through the rock against
3.00 m through the arch, arriving from (0, 0, −1).

**Physics knows about portals.** A swept capsule that crosses an
aperture continues out of the far side with its velocity rotated and
its length remaining — `PhysicsWorld::trace` returns the accumulated
warp, so a character walks through a portal in one `move_and_slide`
rather than teleporting between frames. A body that crosses an unequal
pair is rescaled by the ratio, and its collision radius, step height
and eye height all follow from that one number.

**Replication is a list of property names.** A node says which of its
properties matter over the wire; the server reads them through
ClassDB, writes them as Variants and the client sets them back the
same way. Nothing in the net layer knows what a `Vec3` is or that
`Node3D` has a position — which is why a plugin's class and a Python
subclass replicate without the net layer being told they exist.

**One declaration, every binding.** A class says what it has once, in
C++:

```cpp
ClassBuilder<Portal3D>(...)
    .prop("width", &Portal3D::width)
    .method("link_to", &Portal3D::link_to).args("other")
    .method("within_aperture", &Portal3D::within_aperture, {Variant(0.0)})
        .args("world_point", "margin");
```

and from that come the Python type, the property inspector, the scene
serialiser, the error message when a script calls something that is
not there, and the `.pyi` an editor completes against. Bindings
written by hand are how an engine's scripting layer ends up a release
behind its engine; generated from the table the engine itself
dispatches through, they cannot be.

## Building

Needs a C++20 compiler, CMake 3.20, Python 3, SDL2, and — for the
shader build — `glslang-tools` and `spirv-cross`. Vulkan headers are
optional; the Vulkan backend is loaded with `dlopen` at run time, so a
machine with no Vulkan driver runs OpenGL rather than failing to start.

```sh
sudo apt install build-essential cmake ninja-build python3 \
                 libsdl2-dev glslang-tools spirv-cross libvulkan-dev \
                 python3-dev
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

```sh
build/bin/warren --demo portals              # Vulkan by default
build/bin/warren --demo portals --backend gl
build/bin/warren --demo terrain              # the voxel plugin
build/bin/warren --demo terrain --bench      # time it and print percentiles
build/bin/warren --help
```

`--bench` runs 600 frames unthrottled and reports percentiles rather
than an average, because an engine that averages 4 ms and spikes to 40
whenever a chunk streams in is worse to play than one that sits at 8:

```
bench  Vulkan  terrain  1600x900  msaa 4
  frame   mean  12.43 ms  ( 80.4 fps)
          min    8.87   p50  10.85   p95  18.36   p99  30.47   max  59.34
  record  mean   2.93 ms   (the renderer's own CPU time)
```

Right mouse captures the cursor, escape releases it. WASD moves, Q/E or
space/ctrl go up and down, shift hurries.

## Scripting

Python 3 is embedded, and the `warren` module is built at start-up by
walking the class registry — so every class, method and property the
engine has is scriptable the moment it is declared, plugins included.
Engine classes can be subclassed, and `_ready`, `_process`,
`_physics_process` and `_exit` are called by the scene tree:

```python
import math
import warren as mf

class Spinner(mf.Node3D):
    def _ready(self):
        self.t = 0.0
    def _process(self, dt):
        self.t += dt
        self.position = mf.Vec3(0, 1.5 + 0.4 * math.sin(self.t), -3)
        self.rotate(mf.Vec3(0, 1, 0), dt)
```

```sh
build/bin/warren --demo portals --script scripts/spinner.py
build/bin/warren --stubs scripts/warren.pyi   # type stubs, then exit
```

`--stubs` writes a PEP 484 stub file from the same registry, with real
argument names and return classes, so an editor completes `mf.` without
a hand-maintained shadow of the API. It loads plugins first, so their
classes are in there too. `tests/test_python` compiles the result and
checks it covers every registered class, which is what keeps it from
rotting.

## Scenes and UI, the way Godot does them

A scene is a file you can **instance**. A door is a scene, a corridor
is eight instances of it, and editing the door changes all eight —
except the one somebody deliberately made different. Measured: one
door is 1581 bytes and a corridor of eight is **632**, because it is
eight references and a handful of overrides.

A **UI is made of nodes**, so a health bar is a scene and a party of
four is four instances of it. `Control` has Godot's anchors-and-
offsets model: four fractions of the parent and four pixel offsets,
which covers pinning, stretching and centring with one mechanism and
no separate docking concept. `HBoxContainer`, `VBoxContainer`,
`GridContainer`, `MarginContainer`, `CenterContainer` and
`PanelContainer` arrange their children and override their anchors,
which is what putting something in a box means. `Label`, `Button`,
`CheckBox`, `LineEdit`, `Slider`, `ProgressBar`, `Panel` and
`ColorRect` emit signals — `pressed`, `value_changed`,
`text_submitted` — so a button knows nothing about what it does.

**The engine has two user interfaces on purpose.** A game's UI is
content: authored, themed, saved in a file, instanced. A tool's UI is
a view of state that changes underneath it, and is better rebuilt
every frame than told about every change. So the editor is immediate
mode and the game's is a node tree, and they share a font, a draw
list and one renderer.

## The editor

`--editor` starts with it open; **F1** toggles it. A scene tree, an
inspector, a Python console and a frame readout, over the running
game.

**The inspector is not written, it is derived.** Every class declares
its properties once so that Python, the scene serialiser and the
network layer can walk them; the inspector is the fourth thing that
walks them and it cost about eighty lines. Eighty-six of the
engine's ninety-seven properties get a real control, chosen from the
declared type — including the properties of a class in a plugin the
editor has never heard of.

The console is the Python bridge with a text field in front of it,
and the engine's log goes to it, so a warning from the renderer
appears where you are looking rather than in a terminal behind the
window.

```sh
build/bin/warren --demo portals --editor
build/bin/warren --demo portals --save-scene level.mfs
build/bin/warren --load-scene level.mfs
```

A scene file stores the tree, every property whose value differs
from a fresh instance of its class, and the meshes and materials
inline. It does **not** store scripts or state a program built by
calling methods — colliders, for instance. Reloading the portals
demo gives back the same rooms, lights, shadows and linked portals;
the player does not walk, because walking is a script.

## Plugins

A plugin is a shared library exporting three C functions. It registers
node classes into the same `ClassDB` the engine uses, which is why the
engine is built as one shared library — two copies of the registry
would mean the plugin's classes register somewhere nobody reads. See
`docs/plugins.md`.

The voxel terrain plugin ships in the box: a signed distance field,
dual contouring with QEF vertex placement, chunk streaming on the job
system, runtime digging and building, and **level of detail** — a
chunk holds the same number of cells at every level and covers twice
the world at each one, so the horizon costs what your feet cost. On
this machine that is the difference between 25 fps at a 192 m view
distance and **80 fps at 512 m**, for 150k triangles instead of 1.1M.

```python
t = mf.instantiate("VoxelTerrain3D")
t.set_viewer(mf.camera())
t.dig(mf.Vec3(0, 12, 0), 6.0)
```

## Built by something that is not a person

Warren has one class registry, and it already generates the Python
bindings, the `.pyi` stubs, the property inspector, scene
serialisation and network replication. Add a sixth reader of the same
table and the engine becomes drivable by a program:

```
$ warren --schema -       # the whole API as JSON: 43 classes, 166
                          # properties, 105 methods, argument names
                          # included. No window, no GPU.
$ warren --agent          # JSON commands on stdin, replies on stdout
```

```json
{"cmd": "create", "class": "OmniLight3D", "parent": "/root/World",
 "properties": {"position": {"y": 2.2}, "colour": "#ff7733", "energy": 40}}
{"cmd": "screenshot", "path": "look.png"}
```

Because it is the same table, a property added to a node this
afternoon is scriptable, inspectable, saveable **and** promptable this
afternoon — with nothing written and nothing regenerated. An interface
maintained alongside the engine drifts out of date with it. This one
cannot.

**A wrong guess is answered rather than refused**, which is what makes
it usable by a model and not only by a program. `"color"` comes back
with `did_you_mean: ["colour"]`. `"OmniLight"` comes back with
`"OmniLight3D"`. A wrong path is matched on its last segment, because
a caller with the wrong path usually has the right name. Setting
`energy` on a plain `Node3D` comes back with `property_exists_on:
["OmniLight3D", ...]` — the name was right and the node was wrong,
which a spelling suggestion cannot help with. And calling
`set_position()`, which is Godot's spelling and the commonest miss
there is, comes back with `but_there_is_a_property: ["position"]`.

## Assets that are written, not modelled

An engine an agent can drive is not much use if every prop in it still
has to come out of Blender. So geometry and surfaces are both things
you can describe.

**A shape is a signed distance field**, contoured with the same dual
contourer the voxel terrain uses:

```json
{"op": "difference", "of": [
    {"shape": "box", "size": [1, 1, 1], "round": 0.06},
    {"shape": "cylinder", "radius": 0.3, "height": 2, "at": [0.6, 0, 0.6]}]}
```

Booleans on fields are `min` and `max`. They cannot fail, have no
special cases and cost one instruction. Cutting one triangle mesh out
of another needs exact predicates and still falls over on coplanar
faces — which is what you get when you subtract a box from a box,
which is what everybody does first. Fields also give you two things
mesh booleans cannot do at all: smooth blends, where two shapes meet
in a fillet rather than a crease, and twist, bend and displace applied
to the whole tree at once.

**A surface is one scalar field over the unit square, coloured at the
end:**

```json
{"pattern": "bricks", "rows": 8, "columns": 4, "mortar": 0.06,
 "colours": [[0, "#4a4440"], [0.55, "#8a4f38"], [1, "#b57a55"]],
 "roughness": [0.95, 0.65], "bump": 1.4}
```

The same field becomes the albedo through the colour ramp, the normal
map through its slope and the roughness through its value — so the
mortar is rougher than the brick because it is darker, without anybody
saying so twice. Eleven patterns, nine blend modes, and a domain warp
that turns a sine wave into convincing wood grain in one line.
Everything tiles exactly, by wrapping the noise lattice rather than
blending its edges.

Both are reachable three ways: the agent protocol, `warren.shape({...})`
and `warren.surface({...})` from Python, and `gen::Sdf` in C++.
Procedural materials are triplanar by default, because contoured
geometry has no UVs worth having.

`docs/agent.md` is the reference for all of it.

## Layout

```
src/core/          types, math, reflection, Variant
src/core/math/     vectors, quaternions, transforms, Projection
src/platform/      window, input, clock (SDL2)
src/rhi/           the render hardware interface
src/rhi/gl/        OpenGL 4.5 backend
src/rhi/vk/        Vulkan 1.3 backend
src/render/        meshes, materials, textures, the renderer
src/render/shaders/  one source per program, both backends
src/scene/         node tree, cameras, lights, Portal3D, bodies
src/anim/          skeletons, poses, clips, retargeting
src/nav/           navmesh baking, pathfinding, crowds
src/physics/       shapes, BVH, sweeps, portal-aware tracing
src/audio/         the mixer and clip loading
src/net/           sockets, reliability, replication
src/ui/            the draw list, a 5x7 font, immediate-mode widgets
src/resource/      resources, PackedScene, glTF import
src/procgen/       noise, fields, dual contouring, SDF shapes,
                   procedural textures
src/agent/         the JSON protocol and the API schema
src/editor/        the scene tree, inspector, console and scene files
src/script/        the Python bridge and the stub generator
src/plugin/        the plugin ABI and host
src/app/           Engine: the loop that ties it together
plugins/voxel/     voxel terrain: generation, edits, LOD streaming
tools/             the three code generators
tests/             maths, backend parity, the portal stencil
                   sequence, portal traversal, shadows, clustered
                   lights, image-based lighting, punctual shadows,
                   the plugin ABI and terrain LOD, audio,
                   networking, the UI, controls, scenes, the
                   editor, Python, resources, glTF, procedural
                   generation, skinning, the navmesh bake and the
                   funnel, crowds, the agent protocol
docs/conventions.md  the rules, stated once
docs/agent.md        driving the engine from a program
docs/plugins.md      how to write one
```

Three things are generated at build time from manifests checked into
the tree, never committed: the OpenGL loader
(`tools/glfns.txt`), the Vulkan loader (`tools/vkfns.txt`) and the
shaders. A name that is not in the system header is a build failure
rather than a null pointer at run time, and each manifest doubles as a
statement of exactly how much of that API the engine depends on.

## Status

Early, and honest about it.

Working: both backends at verified parity, the reverse-Z oblique
projection, the scene tree with reflection, PBR forward shading with
cascaded and punctual shadows, clustered lights, image-based lighting,
a procedural sky, filmic tonemapping, MSAA, triplanar mapping,
**recursive stencil-clipped portals with per-pair size ratios**,
portal-aware swept physics with size-changing traversal, portal-aware
audio, a networking layer with three delivery channels, a
work-stealing job system, the plugin ABI, streaming dual-contoured
voxel terrain, Control-node UI, an editor whose inspector is derived
from reflection, resources and scene instancing, glTF import,
procedural shapes and surfaces, GPU skeletal animation with clip
blending and retargeting, navmesh baking with A* and funnel
string-pulling, off-mesh links, reachability pruning, crowds with
reciprocal avoidance, and Python scripting with generated type stubs.

Not yet: animation state machines beyond two slots and a masked
one-shot, scripts saved in scene files, transform gizmos, particles,
rigid bodies, tiled navmesh bakes for streamed worlds, and text
wrapping in `Label`. The Windows paths exist and have never been
compiled.

See `docs/conventions.md` before touching the renderer.
