#include "node.h"

#include <algorithm>

#include "core/log.h"
#include "scene/scene_tree.h"

namespace wr {

// ------------------------------------------------------------------ Node

Node::Node() = default;

Node::~Node() {
    // Children are released here; each one's destructor runs when the
    // last reference to it goes, which for a tree-owned node is now.
    for (auto &c : children_)
        if (c) c->parent_ = nullptr;
    children_.clear();
    delete script_;
    script_ = nullptr;
}

void Node::set_name(const std::string &n) {
    if (n.empty()) return;
    // Siblings with the same name make `get_node` ambiguous, so a
    // collision is resolved rather than allowed.
    if (parent_) {
        std::string candidate = n;
        int suffix = 2;
        while (true) {
            Node *other = parent_->find_child(candidate);
            if (!other || other == this) break;
            candidate = n + std::to_string(suffix++);
        }
        name_ = candidate;
    } else {
        name_ = n;
    }
}

std::string Node::path() const {
    if (!parent_) return "/" + name_;
    return parent_->path() + "/" + name_;
}

void Node::set_owner_recursive(Node *o) {
    owner_ = o;
    for (auto &c : children_) {
        if (!c) continue;
        // STOP AT AN INSTANCE. A door placed in a corridor belongs
        // to the corridor -- that is what makes the corridor write
        // it -- but everything INSIDE the door belongs to the door,
        // and claiming it for the corridor is what turns "one line
        // referring to a door" back into "a copy of a door".
        if (!c->scene_path_.empty()) {
            c->owner_ = o;
            continue;
        }
        c->set_owner_recursive(o);
    }
}

void Node::add_child(Node *child) {
    if (!child) return;
    if (child == this) {
        WR_ERROR("%s: a node cannot be its own child", name_.c_str());
        return;
    }
    if (child->parent_ == this) return;
    // A cycle would make the tree a graph and every traversal infinite.
    for (Node *p = this; p; p = p->parent_)
        if (p == child) {
            WR_ERROR("%s: adding %s would make a cycle", name_.c_str(),
                     child->name_.c_str());
            return;
        }
    if (child->parent_) child->parent_->remove_child(child);

    child->parent_ = this;
    children_.push_back(Ref<Node>(child));
    child->set_name(child->name_);  // resolve a collision with a sibling
    if (Node3D *n3 = child->cast_to<Node3D>()) n3->invalidate_global();
    if (tree_) child->propagate_enter(tree_);
}

void Node::remove_child(Node *child) {
    if (!child) return;
    auto it = std::find_if(children_.begin(), children_.end(),
                           [child](const Ref<Node> &r) { return r.get() == child; });
    if (it == children_.end()) return;
    if (child->tree_) child->propagate_exit();
    child->parent_ = nullptr;
    // Hold a reference across the erase so the caller still has a live
    // pointer when this returns.
    Ref<Node> keep = *it;
    children_.erase(it);
}

void Node::free_from_parent() {
    if (parent_) parent_->remove_child(this);
}

void Node::queue_free() {
    queued_free_ = true;
    if (tree_) tree_->_queue_free(this);
    else free_from_parent();
}

Node *Node::find_child(const std::string &n) const {
    for (const auto &c : children_)
        if (c && c->name_ == n) return c.get();
    return nullptr;
}

Node *Node::find_path(const std::string &p) const {
    if (p.empty() || p == ".") return const_cast<Node *>(this);
    const Node *cur = this;
    size_t i = 0;
    if (p[0] == '/') {
        cur = root();
        i = 1;
    }
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!cur->parent_) return nullptr;
            cur = cur->parent_;
            continue;
        }
        Node *next = cur->find_child(part);
        if (!next) return nullptr;
        cur = next;
    }
    return const_cast<Node *>(cur);
}

Node *Node::get_node(const std::string &p) const {
    // THE LOUD ONE. A script asking for a node it expects to exist
    // wants to be told when it does not, because the alternative is
    // a null dereference three lines later with no clue why. A
    // lookup that is allowed to fail -- resolving a saved path,
    // probing for an optional child -- calls find_path instead, and
    // a "not found" that is a normal answer should never be logged
    // as an error.
    Node *n = find_path(p);
    if (!n)
        WR_ERROR("get_node('%s'): nothing there, from %s", p.c_str(),
                 path().c_str());
    return n;
}

Node *Node::root() const {
    const Node *n = this;
    while (n->parent_) n = n->parent_;
    return const_cast<Node *>(n);
}

Node *Node::find_by_class(const std::string &class_name) const {
    // Breadth first: the nearest match is almost always the one meant.
    std::vector<const Node *> queue{this};
    for (size_t i = 0; i < queue.size(); i++) {
        const Node *n = queue[i];
        if (n != this && n->is_class(class_name)) return const_cast<Node *>(n);
        for (const auto &c : n->children_)
            if (c) queue.push_back(c.get());
    }
    return nullptr;
}

std::vector<Node *> Node::find_all_by_class(const std::string &class_name) const {
    std::vector<Node *> out;
    std::vector<const Node *> queue{this};
    for (size_t i = 0; i < queue.size(); i++) {
        const Node *n = queue[i];
        if (n->is_class(class_name)) out.push_back(const_cast<Node *>(n));
        for (const auto &c : n->children_)
            if (c) queue.push_back(c.get());
    }
    return out;
}

void Node::add_to_group(const std::string &g) {
    if (in_group(g)) return;
    groups_.push_back(g);
    if (tree_) tree_->_add_to_group(g, this);
}

void Node::remove_from_group(const std::string &g) {
    auto it = std::find(groups_.begin(), groups_.end(), g);
    if (it == groups_.end()) return;
    groups_.erase(it);
    if (tree_) tree_->_remove_from_group(g, this);
}

bool Node::in_group(const std::string &g) const {
    return std::find(groups_.begin(), groups_.end(), g) != groups_.end();
}

void Node::propagate_enter(SceneTree *t) {
    tree_ = t;
    for (const std::string &g : groups_) t->_add_to_group(g, this);
    on_enter_tree();
    // Children enter before _ready runs, so that a node's _ready can
    // rely on its whole subtree being present -- which is the thing
    // that makes _ready different from a constructor.
    for (auto &c : children_)
        if (c) c->propagate_enter(t);
    if (!ready_called_) {
        ready_called_ = true;
        on_ready();
    }
}

void Node::propagate_exit() {
    for (auto &c : children_)
        if (c) c->propagate_exit();
    if (tree_)
        for (const std::string &g : groups_) tree_->_remove_from_group(g, this);
    on_exit_tree();
    if (script_) script_->on_exit();
    tree_ = nullptr;
}

void Node::on_ready() {
    if (script_) script_->on_ready();
}
void Node::on_process(float dt) {
    if (script_) script_->on_process(dt);
}
void Node::on_physics(float dt) {
    if (script_) script_->on_physics(dt);
}

void Node::set_script(ScriptInstance *s) {
    delete script_;
    script_ = s;
    // A script attached to a node already in the tree gets its _ready
    // now; otherwise it would never get one.
    if (script_ && ready_called_) script_->on_ready();
}

bool Node::set_member(const std::string &name, const Variant &v) {
    if (script_ && script_->script_set(name, v)) return true;
    return set(name, v);
}

Variant Node::get_member(const std::string &name) const {
    Variant out;
    if (script_ && script_->script_get(name, &out)) return out;
    return get(name);
}

std::string Node::to_string_tree(int indent) const {
    std::string s(size_t(indent) * 2, ' ');
    s += name_;
    s += " [";
    s += get_class_name();
    s += "]";
    if (script_) {
        s += " <";
        s += script_->script_name();
        s += ">";
    }
    s += "\n";
    for (const auto &c : children_)
        if (c) s += c->to_string_tree(indent + 1);
    return s;
}

// ---------------------------------------------------------------- Node3D

void Node3D::invalidate_global() {
    if (global_dirty_) return;  // the subtree below is already marked
    global_dirty_ = true;
    for (const auto &c : children_)
        if (Node3D *n = c ? c->cast_to<Node3D>() : nullptr) n->invalidate_global();
}

void Node3D::set_transform(const Transform3D &t) {
    local_ = t;
    invalidate_global();
}

Transform3D Node3D::global_transform() const {
    if (!global_dirty_) return global_cache_;
    // Walk up to the nearest ancestor that is a Node3D. A plain Node in
    // between is not a break in the chain -- it simply has no transform
    // of its own -- which is what lets a Node be used purely as a
    // folder.
    const Node *p = parent_;
    while (p && !p->is_class("Node3D")) p = p->parent();
    if (const Node3D *pn = p ? static_cast<const Node3D *>(p) : nullptr)
        global_cache_ = pn->global_transform() * local_;
    else
        global_cache_ = local_;
    global_dirty_ = false;
    return global_cache_;
}

void Node3D::set_global_transform(const Transform3D &t) {
    const Node *p = parent_;
    while (p && !p->is_class("Node3D")) p = p->parent();
    if (const Node3D *pn = p ? static_cast<const Node3D *>(p) : nullptr)
        local_ = pn->global_transform().inverse() * t;
    else
        local_ = t;
    invalidate_global();
}

void Node3D::set_position(const Vec3 &p) {
    local_.origin = p;
    invalidate_global();
}

void Node3D::set_global_position(const Vec3 &p) {
    Transform3D g = global_transform();
    g.origin = p;
    set_global_transform(g);
}

void Node3D::set_basis(const Basis &b) {
    local_.basis = b;
    invalidate_global();
}

void Node3D::set_rotation(const Quat &q) {
    // Keep whatever scale was there; a rotation should not resize.
    float s = local_.basis.uniform_scale();
    local_.basis = q.to_basis() * s;
    invalidate_global();
}

void Node3D::set_euler(const Vec3 &e) {
    set_rotation(Quat::from_euler_yxz(e.x, e.y, e.z));
}

void Node3D::set_scale(float s) {
    // UNIFORM ONLY, and the engine depends on it: the normal basis in
    // every shader is the model matrix with one length divided out,
    // the physics shapes are scaled by one number, and a portal warp
    // produces nothing else. A non-uniform scale would break all three
    // quietly, so there is no way to ask for one.
    if (s < 1e-6f) s = 1e-6f;
    local_.basis = local_.basis.rescaled(s);
    invalidate_global();
}

void Node3D::translate(const Vec3 &delta) {
    local_.origin += delta;
    invalidate_global();
}

void Node3D::rotate(const Vec3 &axis, float angle) {
    local_.basis = Basis::from_axis_angle(axis, angle) * local_.basis;
    invalidate_global();
}

void Node3D::look_at(const Vec3 &target, const Vec3 &up) {
    Transform3D g = global_transform();
    Vec3 dir = target - g.origin;
    if (dir.length_sq() < EPS) return;
    float s = g.basis.uniform_scale();
    g.basis = Basis::looking_at(dir.normalized(), up) * s;
    set_global_transform(g);
}

bool Node3D::visible_in_tree() const {
    if (!visible_) return false;
    for (const Node *p = parent(); p; p = p->parent())
        if (const Node3D *n = p->cast_to<Node3D>())
            if (!n->visible()) return false;
    return true;
}

// ------------------------------------------------------------ reflection

static void register_node_classes() {
    ClassBuilder<Node>()
        .method("set_name", &Node::set_name).args("name")
        .method("get_name", &Node::name)
        .method("get_path", &Node::path)
        .method("add_child", &Node::add_child).args("child")
        .method("remove_child", &Node::remove_child).args("child")
        .method("queue_free", &Node::queue_free)
        .method("get_parent", &Node::parent)
        .method("get_owner", &Node::owner)
        .method("set_owner", &Node::set_owner).args("owner")
        .prop("scene_path", &Node::scene_path, &Node::set_scene_path)
        .method("get_child_count", &Node::child_count)
        .method("get_child", &Node::child).args("index")
        .method("find_child", &Node::find_child).args("name")
        .method("get_node", &Node::get_node).args("path")
        .method("find_path", &Node::find_path).args("path")
        .method("find_by_class", &Node::find_by_class).args("class_name")
        .method("add_to_group", &Node::add_to_group).args("group")
        .method("remove_from_group", &Node::remove_from_group).args("group")
        .method("is_in_group", &Node::in_group).args("group")
        .method("is_inside_tree", &Node::is_inside_tree)
        .prop("process", &Node::processing, &Node::set_processing)
        .prop("physics_process", &Node::physics_processing,
              &Node::set_physics_processing)
        .method("print_tree", &Node::to_string_tree, {Variant(0)}).args("indent")
        .signal("tree_entered")
        .signal("tree_exiting");

    ClassBuilder<Node3D>()
        // POSITION AND BASIS ARE THE STATE; the rest are views of
        // it. A scene file stores those two and nothing else, or a
        // child comes back with its parent's transform folded in.
        .prop("transform", &Node3D::transform, &Node3D::set_transform)
        .transient()
        .prop("global_transform", &Node3D::global_transform,
              &Node3D::set_global_transform)
        .transient()
        .prop("position", &Node3D::position, &Node3D::set_position)
        .prop("global_position", &Node3D::global_position,
              &Node3D::set_global_position)
        .transient()
        .prop("basis", &Node3D::basis, &Node3D::set_basis)
        .prop("rotation", &Node3D::rotation, &Node3D::set_rotation)
        .transient()
        .prop("euler", &Node3D::euler, &Node3D::set_euler)
        .transient()
        .prop("scale", &Node3D::scale, &Node3D::set_scale)
        .transient()
        .prop("visible", &Node3D::visible, &Node3D::set_visible)
        .prop("layers", &Node3D::layers, &Node3D::set_layers)
        .method("translate", &Node3D::translate).args("delta")
        .method("rotate", &Node3D::rotate).args("axis", "angle")
        .method("look_at", &Node3D::look_at, {Variant(Vec3::up())}).args("target", "up")
        .method("forward", &Node3D::forward)
        .method("up", &Node3D::up)
        .method("right", &Node3D::right)
        .method("is_visible_in_tree", &Node3D::visible_in_tree);
}
WR_REGISTER(register_node_classes)

}  // namespace wr
