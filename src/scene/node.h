// Warren -- the scene tree.
//
// In the image of Godot, because that shape is right: a tree of nodes,
// each a small thing that does one job, composed rather than inherited
// into a game object. What is different is underneath -- a Node3D's
// global transform may be reached through a PORTAL, and the tree knows
// it.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"

namespace wr {

class Node;
class SceneTree;

// What a script attached to a node implements. The engine calls these;
// the Python layer provides one of these per scripted node. Keeping it
// an interface rather than a language binding means a second language
// costs one class.
class ScriptInstance {
public:
    virtual ~ScriptInstance() = default;
    virtual void on_ready() {}
    virtual void on_process(float dt) {}
    virtual void on_physics(float dt) {}
    virtual void on_exit() {}
    // Returns false if the script does not define the member, so the
    // engine can fall back to its own property table.
    virtual bool script_get(const std::string &name, Variant *out) { return false; }
    virtual bool script_set(const std::string &name, const Variant &v) { return false; }
    virtual bool script_call(const std::string &name, const Variant *args, int argc,
                             Variant *out) {
        return false;
    }
    virtual const char *script_name() const { return "<script>"; }
};

class Node : public Object {
    WR_CLASS(Node, Object)

public:
    Node();
    ~Node() override;

    // --- identity ---------------------------------------------------------
    const std::string &name() const { return name_; }
    void set_name(const std::string &n);
    // Slash-separated, from the root. What an error message should say.
    std::string path() const;

    // --- the tree ----------------------------------------------------------
    // The parent takes a reference; `add_child` is the only owner.
    void add_child(Node *child);
    void remove_child(Node *child);
    // Detach from the parent and drop its reference. If nothing else
    // holds one, this is the end of the node.
    void free_from_parent();
    // Queue for removal at the end of the frame. Deleting a node from
    // inside its own _process is otherwise a use-after-free.
    void queue_free();
    bool is_queued_for_deletion() const { return queued_free_; }

    Node *parent() const { return parent_; }

    // WHICH SCENE THIS NODE BELONGS TO.
    //
    // Not its parent: a door instanced inside a room has the room's
    // wall as its parent and the DOOR as its owner, because the door
    // is where it is defined. Saving the room then writes that node
    // as part of an instance line rather than in full, which is what
    // makes editing the door change every room that uses one.
    //
    // Null means "belongs to whatever tree it is in", which is true
    // of anything built by hand in code.
    Node *owner() const { return owner_; }
    void set_owner(Node *o) { owner_ = o; }
    // Owner for this node and everything beneath it. What
    // PackedScene::instantiate uses, and what a tool uses after
    // building a subtree it means to save as a unit.
    void set_owner_recursive(Node *o);

    // WHERE THIS NODE CAME FROM, when it is the root of an instance.
    // Empty for everything else. Set by PackedScene::instantiate and
    // read by whatever saves the tree above it.
    const std::string &scene_path() const { return scene_path_; }
    void set_scene_path(const std::string &p) { scene_path_ = p; }
    const std::vector<Ref<Node>> &children() const { return children_; }
    int child_count() const { return int(children_.size()); }
    Node *child(int i) const {
        return (i >= 0 && i < int(children_.size())) ? children_[size_t(i)].get()
                                                     : nullptr;
    }
    // Direct child by name, or null.
    Node *find_child(const std::string &n) const;
    // "Arena/Props/Lamp", or "." for this node. Null and a logged error
    // if any step is missing.
    // Loud: logs when it finds nothing, for a caller that expected
    // something. Use find_path where a miss is a normal answer.
    Node *get_node(const std::string &path) const;
    Node *find_path(const std::string &path) const;
    // First descendant of the given class, breadth first.
    Node *find_by_class(const std::string &class_name) const;
    std::vector<Node *> find_all_by_class(const std::string &class_name) const;

    SceneTree *tree() const { return tree_; }
    bool is_inside_tree() const { return tree_ != nullptr; }
    Node *root() const;

    // --- groups ------------------------------------------------------------
    // The cheap way to ask "every enemy" without walking the tree.
    void add_to_group(const std::string &g);
    void remove_from_group(const std::string &g);
    bool in_group(const std::string &g) const;
    const std::vector<std::string> &groups() const { return groups_; }

    // --- ticking ------------------------------------------------------------
    bool processing() const { return process_; }
    void set_processing(bool on) { process_ = on; }
    bool physics_processing() const { return physics_; }
    void set_physics_processing(bool on) { physics_ = on; }

    // Overridden by engine classes; the default forwards to the script.
    virtual void on_ready();
    virtual void on_process(float dt);
    virtual void on_physics(float dt);
    virtual void on_enter_tree() {}
    virtual void on_exit_tree() {}

    // --- scripts ---------------------------------------------------------------
    void set_script(ScriptInstance *s);
    ScriptInstance *script() const { return script_; }

    // Reflection falls through to the script, so a Python attribute and
    // an engine property are the same thing to a caller.
    bool set_member(const std::string &name, const Variant &v);
    Variant get_member(const std::string &name) const;

    std::string to_string_tree(int indent = 0) const;

protected:
    friend class SceneTree;
    void propagate_enter(SceneTree *t);
    void propagate_exit();

    std::string name_ = "Node";
    Node *parent_ = nullptr;
    // Weak on purpose. An owner is always an ancestor, so it
    // outlives what it owns, and a reference would be a cycle.
    Node *owner_ = nullptr;
    std::string scene_path_;             // borrowed; the parent owns us
    std::vector<Ref<Node>> children_;
    std::vector<std::string> groups_;
    SceneTree *tree_ = nullptr;
    ScriptInstance *script_ = nullptr;   // owned
    bool process_ = true;
    bool physics_ = true;
    bool ready_called_ = false;
    bool queued_free_ = false;
};

// A node with a place in the world.
//
// The global transform is cached and invalidated down the subtree when
// anything above it moves, because the renderer and the physics both
// ask for it many times a frame and recomputing a chain of matrices per
// query is how a scene graph becomes the slowest part of an engine.
class Node3D : public Node {
    WR_CLASS(Node3D, Node)

public:
    Node3D() = default;

    const Transform3D &transform() const { return local_; }
    void set_transform(const Transform3D &t);
    Transform3D global_transform() const;
    void set_global_transform(const Transform3D &t);

    Vec3 position() const { return local_.origin; }
    void set_position(const Vec3 &p);
    Vec3 global_position() const { return global_transform().origin; }
    void set_global_position(const Vec3 &p);

    Basis basis() const { return local_.basis; }
    void set_basis(const Basis &b);
    Quat rotation() const { return local_.basis.to_quat(); }
    void set_rotation(const Quat &q);
    // Yaw, pitch, roll in radians, in the camera's YXZ order.
    Vec3 euler() const { return local_.basis.to_euler_yxz(); }
    void set_euler(const Vec3 &e);

    float scale() const { return local_.basis.uniform_scale(); }
    // UNIFORM ONLY. A portal warp produces uniform scale and nothing
    // else, and the physics shapes, the normals and the lighting all
    // assume it; letting a non-uniform scale in would break all three
    // quietly.
    void set_scale(float s);

    void translate(const Vec3 &delta);
    void rotate(const Vec3 &axis, float angle);
    void look_at(const Vec3 &target, const Vec3 &up = Vec3::up());

    Vec3 forward() const { return global_transform().basis.forward(); }
    Vec3 up() const { return global_transform().basis.y(); }
    Vec3 right() const { return global_transform().basis.x(); }

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }
    // False if this node or any ancestor is hidden.
    bool visible_in_tree() const;

    // Layers a camera may or may not be told to draw. Portal views use
    // these to hide a portal's own frame from the camera looking
    // through it.
    uint32_t layers() const { return layers_; }
    void set_layers(uint32_t m) { layers_ = m; }

    void invalidate_global();

protected:
    Transform3D local_;
    mutable Transform3D global_cache_;
    mutable bool global_dirty_ = true;
    bool visible_ = true;
    uint32_t layers_ = 1;
};

}  // namespace wr
