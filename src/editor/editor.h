// Manifold -- the editor.
//
// THE INSPECTOR IS NOT WRITTEN, IT IS DERIVED. Every class in this
// engine declares its properties once so that Python, the scene
// serialiser and the network layer can all walk them; the inspector
// is the fourth thing that walks them and it cost about eighty
// lines. A hand-written property panel per class is the thing that
// silently falls behind the class it inspects, and an engine with
// reflection has no excuse for one.
//
// The same argument runs through the rest: the console is the Python
// bridge with a text field in front of it, scene saving is the
// Variant serialiser, and the tree view is the scene tree. The
// editor is mostly an arrangement of things the engine already had
// to have.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "core/object.h"
#include "scene/node.h"
#include "ui/ui.h"

namespace mf {

class Engine;
class Node3D;
class Camera3D;

class Editor {
public:
    void init(Engine *engine);
    void shutdown();
    bool enabled() const { return enabled_; }
    void set_enabled(bool on) { enabled_ = on; }
    void toggle() { enabled_ = !enabled_; }

    // Builds a frame of UI. Call between the tree's process and the
    // render, with the input the platform collected.
    void build(ui::Context &ui, float dt);
    // True while a panel wants the pointer, so the game should not
    // also act on it.
    bool captures_mouse() const { return captures_mouse_; }

    Node *selection() const { return selected_.get(); }
    void select(Node *n) { selected_ = n; }

    // --- scene files ---------------------------------------------------
    // The whole tree as bytes: class names, the tree shape, and
    // every reflected property whose value differs from a freshly
    // constructed one of the same class.
    std::vector<uint8_t> serialise_scene(Node *root) const;
    Node *deserialise_scene(const uint8_t *data, size_t size) const;
    bool save_scene(const std::string &path);
    bool load_scene(const std::string &path);

    void log_line(const std::string &s) { console_.push_back(s); trim(); }

private:
    void panel_tree(ui::Context &ui);
    void panel_inspector(ui::Context &ui);
    void panel_console(ui::Context &ui);
    void panel_stats(ui::Context &ui, float dt);
    void panel_scene_file(ui::Context &ui);
    void draw_node_row(ui::Context &ui, Node *n, int depth);
    void property_row(ui::Context &ui, Object *o, const PropertyInfo &p);
    void trim() {
        while (console_.size() > 400) console_.pop_front();
    }

    Engine *engine_ = nullptr;
    bool enabled_ = false;
    bool captures_mouse_ = false;
    Ref<Node> selected_;
    std::deque<std::string> console_;
    std::string command_;
    std::string scene_path_ = "scene.mfs";
    bool show_tree_ = true, show_inspector_ = true, show_console_ = true,
         show_stats_ = true;
    // Smoothed, because a frame time that flickers between 6 and 7
    // is unreadable and a number nobody can read is not a number.
    float fps_ = 0.0f;
    float frame_ms_ = 0.0f;
};

}  // namespace mf
