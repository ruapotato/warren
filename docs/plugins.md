# Writing a Manifold plugin

A plugin is a shared library the engine loads at start-up. It can
register node classes, resource loaders and services exactly as
built-in code does — the voxel terrain that ships with the engine is
one, on purpose: if the plugin interface is not good enough to build
terrain with, it is not good enough.

## The contract

Three C entry points, and C++ for everything after them.

```cpp
#include "plugin/plugin.h"

MF_PLUGIN_DECLARE("my-plugin", "1.0.0", "Me", "What it does")

MF_PLUGIN_EXPORT bool mf_plugin_init(const mf::PluginContext *ctx) {
    // ctx->engine, ->device, ->tree, ->renderer, ->physics, ->directory
    mf::ClassDB::register_all();   // pick up this library's MF_REGISTER
    return true;
}

MF_PLUGIN_EXPORT void mf_plugin_shutdown() {}

// Optional, called once a frame before the tree is processed:
// MF_PLUGIN_EXPORT void mf_plugin_frame(float dt) {}
```

C at the boundary because a C++ symbol's name and a C++ object's
layout depend on the compiler, its version and its flags, and a plugin
that cannot say "I was built for engine version X" *before* anything
is dereferenced will crash instead of complaining. `MANIFOLD_PLUGIN_ABI`
is checked first and a mismatch is refused with a message.

C++ after that, because a plugin that could only speak C would have to
reimplement every type the engine already has. **The cost is stated
plainly rather than hidden: a plugin must be built from the same
headers, with a compatible compiler, as the engine it loads into.**

## Building one

```cmake
manifold_add_plugin(my_plugin src/a.cpp src/b.cpp)
```

Drop the directory in `plugins/` and it is picked up. The result lands
in `bin/plugins/`, which is where the runtime looks — beside the
executable, not beside the shell, because a game is launched from a
shortcut or a debugger and neither sets the working directory.

The engine is built as a **shared** library when plugins are enabled,
and that is not a packaging preference: a plugin registers classes into
ClassDB, logs through the engine's logger and allocates from its pools.
Link the engine statically into both and there are *two* of each — the
plugin's classes register into a registry nobody reads.

## Registering a node class

```cpp
class MyNode : public mf::Node3D {
    MF_CLASS(MyNode, Node3D)
public:
    float speed = 1.0f;
    void do_something(const mf::Vec3 &where);
};

static void register_my_classes() {
    mf::ClassBuilder<MyNode>()
        .field("speed", &MyNode::speed, "range:0,10")
        .method("do_something", &MyNode::do_something);
}
MF_REGISTER(register_my_classes)
```

From then on the class is reachable by name — from C++, from a saved
scene, and from Python — without the engine having heard of it:

```cpp
mf::Object *n = mf::ClassDB::instantiate("MyNode");
n->set_member("speed", mf::Variant(4.0));
n->callv("do_something", {mf::Variant(mf::Vec3(1, 2, 3))});
```

The terrain demo in `src/main.cpp` builds the whole voxel terrain this
way and cannot include a single one of the plugin's headers, which is
the honest test that this works.

## Threads

`mf::Jobs::submit` and `mf::Jobs::parallel_for` are available to
plugins. The voxel terrain meshes chunks on them. Two rules:

* **The GPU is the main thread's.** Creating buffers and textures is
  not thread safe; do the work on a worker and upload from
  `on_process`.
* **Publish with a release store.** The voxel chunk writes its mesh on
  a worker and flips an atomic state to `Ready`; the main thread reads
  that state with acquire. Without the pairing, the vertex data is a
  data race that happens to work on x86 and does not elsewhere.

## Level of detail, and what a test for it has to check

The voxel plugin's chunks carry a level. A level-L chunk is twice as
wide as a level-(L-1) one and holds the same number of cells, so it
costs the same to mesh and to draw and covers four times the ground.
Level 0 reaches `lod_distance`, each level after it twice as far.

The selection rule is deliberately generous at both edges: a chunk
belongs to level L if its NEAREST point is inside L's range and its
FARTHEST point is outside the finer level's. Two levels then overlap
by a chunk along a boundary, which the depth buffer settles. The tight
version — testing the chunk's centre for both — leaves a ring of the
world at the wrong level instead, and the depth buffer cannot settle
that.

`tests/test_voxel_lod` is the check, and its first version was worth
nothing. It asked "is some chunk loaded at this point", which a
level-4 chunk satisfies for five hundred metres in every direction: a
scheme that skipped level 0 entirely still passed. The check with
teeth is **at what level**: ground must be drawn at the level its
distance calls for, finer being allowed because the shells overlap on
purpose. Under that check the tight rule fails 275 of 797 samples,
worst by two levels, and the generous one fails none.
