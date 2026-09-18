#include "scene_tree.h"

#include <algorithm>

#include "core/log.h"
#include "scene/nodes.h"

namespace wr {

SceneTree::SceneTree() {
    root_ = Ref<Node>(new Node());
    root_->set_name("root");
    root_->propagate_enter(this);
}

SceneTree::~SceneTree() {
    pending_free_.clear();
    if (root_) root_->propagate_exit();
    root_.reset();
}

void SceneTree::set_scene(Node *s) {
    if (scene_) {
        scene_->queue_free();
        scene_ = nullptr;
    }
    if (s) {
        root_->add_child(s);
        scene_ = s;
    }
}

void SceneTree::tick_subtree(Node *n, float dt, bool physics) {
    if (!n || n->is_queued_for_deletion()) return;
    if (physics) {
        if (n->physics_processing()) n->on_physics(dt);
    } else {
        if (n->processing()) n->on_process(dt);
    }
    // The child list is copied because a handler may add or remove
    // children, and iterating a vector being mutated underneath is the
    // classic way a scene tree corrupts itself mid-frame.
    std::vector<Ref<Node>> snapshot = n->children();
    for (auto &c : snapshot)
        if (c) tick_subtree(c.get(), dt, physics);
}

void SceneTree::physics_tick(float fixed_dt) {
    ticking_ = true;
    tick_subtree(root_.get(), fixed_dt, true);
    ticking_ = false;
}

void SceneTree::process(float dt) {
    ticking_ = true;
    tick_subtree(root_.get(), dt, false);
    ticking_ = false;
    frame_++;
    time_ += dt;
}

void SceneTree::flush_frees() {
    if (pending_free_.empty()) return;
    // Taken by value: freeing a node may queue more.
    std::vector<Ref<Node>> batch;
    batch.swap(pending_free_);
    for (auto &n : batch)
        if (n) n->free_from_parent();
}

void SceneTree::_queue_free(Node *n) {
    if (!n) return;
    for (const auto &p : pending_free_)
        if (p.get() == n) return;
    // A reference is held until the flush, so a node freed from inside
    // its own callback stays alive until the stack has unwound.
    pending_free_.push_back(Ref<Node>(n));
    if (n == scene_) scene_ = nullptr;
    if (camera_ && static_cast<Node *>(camera_) == n) camera_ = nullptr;
}

void SceneTree::_add_to_group(const std::string &g, Node *n) {
    std::vector<Node *> &v = groups_[g];
    if (std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
}

void SceneTree::_remove_from_group(const std::string &g, Node *n) {
    auto it = groups_.find(g);
    if (it == groups_.end()) return;
    auto &v = it->second;
    v.erase(std::remove(v.begin(), v.end(), n), v.end());
}

const std::vector<Node *> &SceneTree::group(const std::string &g) const {
    static const std::vector<Node *> empty;
    auto it = groups_.find(g);
    return it == groups_.end() ? empty : it->second;
}

void SceneTree::call_group(const std::string &g, const std::string &method,
                           const Array &args) {
    std::vector<Node *> snapshot = group(g);
    for (Node *n : snapshot)
        if (n && !n->is_queued_for_deletion() && n->has_method(method))
            n->callv(method, args);
}

Camera3D *SceneTree::active_camera() {
    if (camera_) return camera_;
    if (Node *n = root_->find_by_class("Camera3D"))
        camera_ = static_cast<Camera3D *>(n);
    return camera_;
}

}  // namespace wr
