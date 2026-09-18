// Manifold -- a scene as a thing you can have more than one of.
//
// THIS IS THE FEATURE THAT MAKES A SCENE TREE A TOOL RATHER THAN A
// DATA STRUCTURE.
//
// A tree of nodes you can save and load is a level format. A tree of
// nodes you can save, load, and then INSTANCE FIFTY TIMES INSIDE
// ANOTHER TREE is how a game gets built: a door is a scene, a room
// is a scene containing eight doors, a floor is a scene containing
// six rooms, and editing the door changes every one of them. Godot's
// whole editing model rests on that and it is what the engine has
// been missing.
//
// Three things have to be true for it to work, and they are the
// whole of the design:
//
//   1. An instanced subtree is written as A REFERENCE PLUS WHAT WAS
//      CHANGED, never as a copy. Otherwise editing the door does
//      nothing to the fifty doors already placed.
//   2. Every node knows which scene it BELONGS to -- its `owner` --
//      so that saving a room writes the room's own nodes in full and
//      each door as one line.
//   3. A node added to an instance by hand, after the fact, belongs
//      to the outer scene and is written in full. Otherwise the lamp
//      you hung inside one particular door vanishes on reload.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/object.h"
#include "resource/resource.h"
#include "scene/node.h"

namespace mf {

class PackedScene : public Resource {
    MF_CLASS(PackedScene, Resource)

public:
    // The serialised tree. Public because a tool may want to write
    // it somewhere this class knows nothing about.
    std::vector<uint8_t> bytes;
    // Where it came from, which is what an instance of it records.
    std::string path;
    // Reflected accessors: a std::string member cannot be a
    // property directly, and a scene that can be inspected is worth
    // two lines.
    std::string path_get() const { return path; }
    void path_set(const std::string &p) { path = p; }

    // Build a new tree from it. Every node's `owner` is set to the
    // returned root, so saving the result again writes it as one
    // instance line rather than as a copy of everything inside.
    Node *instantiate() const;
    bool valid() const { return !bytes.empty(); }

    // Capture a tree. Nodes whose owner is not `root` -- the
    // contents of scenes instanced inside it -- are written as
    // references and overrides.
    bool pack(Node *root);

    static Ref<PackedScene> load(const std::string &path);
    bool save(const std::string &path);

    // How a scene file names another scene. Resolved through this,
    // so a game can redirect it -- a pak file, a hot-reload cache --
    // without the serialiser knowing.
    using Resolver = Ref<PackedScene> (*)(const std::string &path);
    static void set_resolver(Resolver r);
    static Ref<PackedScene> resolve(const std::string &path);
};

// Serialising a tree without going through a PackedScene, for
// anything that wants the bytes: the network, an undo stack, a test.
std::vector<uint8_t> serialise_tree(Node *root);
Node *deserialise_tree(const uint8_t *data, size_t size);

}  // namespace mf
