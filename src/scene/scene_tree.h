// Warren -- the tree, and the loop that drives it.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "scene/node.h"

namespace wr {

class Camera3D;

class SceneTree {
public:
    SceneTree();
    ~SceneTree();

    Node *root() const { return root_.get(); }
    // Replaces the current scene; the old one is freed at the end of
    // the frame rather than under whatever is running now.
    void set_scene(Node *scene);
    Node *scene() const { return scene_; }

    // A fixed-step physics tick and a variable-step frame, the way
    // every engine that has ever had to be deterministic does it.
    void physics_tick(float fixed_dt);
    void process(float dt);
    // Run the frees that were queued during the frame. Called by the
    // engine after process; separate so a tool can drive the tree by
    // hand.
    void flush_frees();

    float physics_step() const { return physics_step_; }
    void set_physics_step(float s) { physics_step_ = s > 1e-4f ? s : 1e-4f; }

    // Groups. The cheap way to reach every enemy, every portal, every
    // light without walking the tree.
    const std::vector<Node *> &group(const std::string &g) const;
    void call_group(const std::string &g, const std::string &method,
                    const Array &args = {});

    // The camera the renderer should use. Set explicitly, or the first
    // Camera3D found in the tree.
    Camera3D *active_camera();
    void set_active_camera(Camera3D *c) { camera_ = c; }

    uint64_t frame() const { return frame_; }
    double time() const { return time_; }

    // Internal, called by Node.
    void _queue_free(Node *n);
    void _add_to_group(const std::string &g, Node *n);
    void _remove_from_group(const std::string &g, Node *n);

private:
    void tick_subtree(Node *n, float dt, bool physics);

    Ref<Node> root_;
    Node *scene_ = nullptr;
    Camera3D *camera_ = nullptr;
    std::unordered_map<std::string, std::vector<Node *>> groups_;
    std::vector<Ref<Node>> pending_free_;
    float physics_step_ = 1.0f / 60.0f;
    uint64_t frame_ = 0;
    double time_ = 0.0;
    // Recursion guard: a node freed from inside its own _process must
    // not be deleted while the loop still holds a pointer to it.
    bool ticking_ = false;
};

}  // namespace wr
