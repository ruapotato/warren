#include "editor/editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "app/engine.h"
#include "core/log.h"
#include "core/serialize.h"
#include "render/renderer.h"
#include "render/material.h"
#include "render/mesh.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

#if WARREN_PYTHON
#include "script/python.h"
#endif

namespace wr {
namespace {
Editor *g_editor = nullptr;

// The editor's own log sink, so that a warning from anywhere in the
// engine appears in the console rather than only in a terminal
// nobody is looking at.
void editor_log_sink(LogLevel level, const char *message) {
    if (!g_editor) return;
    const char *tag = level == LogLevel::Error   ? "[error] "
                      : level == LogLevel::Warn  ? "[warn ] "
                      : level == LogLevel::Debug ? "[debug] "
                                                 : "";
    g_editor->log_line(std::string(tag) + message);
}
}  // namespace

void Editor::init(Engine *engine) {
    engine_ = engine;
    g_editor = this;
    log_add_sink(&editor_log_sink);
    log_line("Warren editor. F1 toggles, ` focuses the console.");
}

void Editor::shutdown() {
    log_remove_sink(&editor_log_sink);
    g_editor = nullptr;
    selected_.reset();
    engine_ = nullptr;
}

// ---------------------------------------------------------- the panels

void Editor::build(ui::Context &ui, float dt) {
    captures_mouse_ = false;
    if (!enabled_ || !engine_) return;

    // Smoothed over about half a second: a number that flickers
    // between 6 and 7 is a number nobody can read.
    const float ms = dt * 1000.0f;
    frame_ms_ = frame_ms_ <= 0.0f ? ms : frame_ms_ * 0.95f + ms * 0.05f;
    fps_ = frame_ms_ > 1e-4f ? 1000.0f / frame_ms_ : 0.0f;

    if (show_tree_) panel_tree(ui);
    if (show_inspector_) panel_inspector(ui);
    if (show_console_) panel_console(ui);
    if (show_stats_) panel_stats(ui, dt);
    captures_mouse_ = ui.wants_mouse();
}

void Editor::draw_node_row(ui::Context &ui, Node *n, int depth) {
    if (!n) return;
    // The label carries the pointer as identity, so two siblings
    // with the same name are still two different rows -- the
    // immediate-mode bargain, paid explicitly.
    char label[192];
    std::snprintf(label, sizeof(label), "%s  [%s]##%p", n->name().c_str(),
                  n->get_class_name(), (const void *)n);

    const bool has_children = !n->children().empty();
    if (has_children) {
        const bool open = ui.tree_node(label, depth < 1);
        // The row is the selector as well as the expander, so a
        // click anywhere on it selects; the arrow is what expands.
        if (open) {
            for (const auto &c : n->children())
                if (c) draw_node_row(ui, c.get(), depth + 1);
            ui.tree_pop();
        }
    } else {
        if (ui.selectable(label, selected_.get() == n)) select(n);
    }
}

void Editor::panel_tree(ui::Context &ui) {
    if (!ui.begin_window("Scene", {12, 12, 300, 380})) {
        ui.end_window();
        return;
    }
    SceneTree *tree = engine_->tree();
    if (tree && tree->root()) {
        for (const auto &c : tree->root()->children())
            if (c) draw_node_row(ui, c.get(), 0);
    }
    ui.separator();
    if (ui.button("Deselect")) selected_.reset();
    ui.same_line();
    if (ui.button("Delete") && selected_ && selected_->parent()) {
        selected_->queue_free();
        selected_.reset();
    }
    ui.end_window();
}

// THE WHOLE INSPECTOR. Every editable control below is chosen from
// the property's declared VType, so a class that adds a field gets a
// row for it with no editor change at all -- including a class in a
// plugin the editor has never heard of.
void Editor::property_row(ui::Context &ui, Object *o, const PropertyInfo &p) {
    char id[160];
    const Variant v = o->get(p.name);
    switch (p.type) {
        case VType::Bool: {
            bool b = v.to_bool();
            std::snprintf(id, sizeof(id), "%s##%p", p.name.c_str(),
                          (const void *)&p);
            if (ui.checkbox(id, &b)) o->set(p.name, Variant(b));
            break;
        }
        case VType::Float: {
            float f = v.to_float();
            // A declared range gets a slider; anything else gets a
            // drag, because a slider with invented bounds is worse
            // than no slider.
            float low = 0, high = 1;
            if (std::sscanf(p.hint.c_str(), "range:%f,%f", &low, &high) == 2) {
                std::snprintf(id, sizeof(id), "%s##%p", p.name.c_str(),
                              (const void *)&p);
                if (ui.slider(id, &f, low, high)) o->set(p.name, Variant(double(f)));
            } else {
                std::snprintf(id, sizeof(id), "%s##%p", p.name.c_str(),
                              (const void *)&p);
                if (ui.drag_float(id, &f, 0.02f))
                    o->set(p.name, Variant(double(f)));
            }
            break;
        }
        case VType::Int: {
            float f = float(v.to_int());
            std::snprintf(id, sizeof(id), "%s##%p", p.name.c_str(),
                          (const void *)&p);
            if (ui.drag_float(id, &f, 0.25f))
                o->set(p.name, Variant(int64_t(f + (f < 0 ? -0.5f : 0.5f))));
            break;
        }
        case VType::String: {
            std::string s = v.to_string();
            std::snprintf(id, sizeof(id), "%s##%p", p.name.c_str(),
                          (const void *)&p);
            ui.text("%s", p.name.c_str());
            if (ui.input_text(id, &s)) o->set(p.name, Variant(s));
            break;
        }
        case VType::Vec3: {
            Vec3 a = v.to_vec3();
            ui.text("%s", p.name.c_str());
            bool changed = false;
            const float third = (ui.content_rect().w - 8.0f) / 3.0f;
            const char *axis[3] = {"x", "y", "z"};
            float *comp[3] = {&a.x, &a.y, &a.z};
            for (int i = 0; i < 3; i++) {
                std::snprintf(id, sizeof(id), "%s##%p%d", axis[i],
                              (const void *)&p, i);
                ui.set_next_width(third);
                if (ui.drag_float(id, comp[i], 0.02f)) changed = true;
                if (i < 2) ui.same_line();
            }
            if (changed) o->set(p.name, Variant(a));
            break;
        }
        case VType::Color: {
            Color c = v.to_color();
            ui.text("%s", p.name.c_str());
            bool changed = false;
            const float quarter = (ui.content_rect().w - 12.0f) / 4.0f;
            const char *ch[4] = {"r", "g", "b", "a"};
            float *comp[4] = {&c.r, &c.g, &c.b, &c.a};
            for (int i = 0; i < 4; i++) {
                std::snprintf(id, sizeof(id), "%s##%p%d", ch[i],
                              (const void *)&p, i);
                ui.set_next_width(quarter);
                if (ui.slider(id, comp[i], 0.0f, 1.0f)) changed = true;
                if (i < 3) ui.same_line();
            }
            if (changed) o->set(p.name, Variant(c));
            // A swatch, because four numbers are not a colour.
            const ui::Rect r = ui.content_rect();
            ui.spacing(2.0f);
            ui.rect({r.x, ui.cursor().y, r.w, 8.0f},
                    ui::rgba(uint8_t(c.r * 255), uint8_t(c.g * 255),
                             uint8_t(c.b * 255), 255));
            ui.spacing(10.0f);
            break;
        }
        case VType::Quat: {
            // Shown as Euler degrees, because nobody edits a
            // quaternion by typing four numbers.
            const Vec3 e = Basis(v.to_quat()).to_euler_yxz();
            Vec3 deg(rad2deg(e.x), rad2deg(e.y), rad2deg(e.z));
            ui.text("%s (degrees)", p.name.c_str());
            bool changed = false;
            const float third = (ui.content_rect().w - 8.0f) / 3.0f;
            const char *axis[3] = {"yaw", "pitch", "roll"};
            float *comp[3] = {&deg.x, &deg.y, &deg.z};
            for (int i = 0; i < 3; i++) {
                std::snprintf(id, sizeof(id), "%s##%p%d", axis[i],
                              (const void *)&p, i);
                ui.set_next_width(third);
                if (ui.drag_float(id, comp[i], 0.5f)) changed = true;
                if (i < 2) ui.same_line();
            }
            if (changed)
                o->set(p.name, Variant(Quat::from_euler_yxz(deg2rad(deg.x),
                                                            deg2rad(deg.y),
                                                            deg2rad(deg.z))));
            break;
        }
        default:
            // Everything else is shown but not edited. Better to
            // admit a gap than to offer a control that silently
            // does nothing.
            ui.text_coloured(ui.theme.text_dim, "%s : %s", p.name.c_str(),
                             v.to_string().c_str());
            break;
    }
}

void Editor::panel_inspector(ui::Context &ui) {
    if (!ui.begin_window("Inspector", {324, 12, 320, 460})) {
        ui.end_window();
        return;
    }
    Node *n = selected_.get();
    if (!n) {
        ui.text_coloured(ui.theme.text_dim, "Nothing selected.");
        ui.end_window();
        return;
    }
    ui.text("%s", n->name().c_str());
    ui.text_coloured(ui.theme.text_dim, "%s", n->get_class_name());
    ui.separator();

    std::string name = n->name();
    if (ui.input_text("name##rename", &name) && !name.empty())
        n->set_name(name);
    ui.separator();

    // UP THE INHERITANCE CHAIN, most-derived first, each class its
    // own section. That is the order someone thinks in: what is this
    // thing, then what is it also.
    std::vector<ClassInfo *> chain;
    for (ClassInfo *ci = n->get_class_info(); ci; ci = ci->base)
        chain.push_back(ci);
    for (ClassInfo *ci : chain) {
        if (ci->property_order.empty()) continue;
        char header[96];
        std::snprintf(header, sizeof(header), "%s##cls%p", ci->name.c_str(),
                      (const void *)ci);
        if (!ui.tree_node(header, ci == chain.front())) continue;
        for (const std::string &pname : ci->property_order) {
            const PropertyInfo *p = ci->find_property(pname);
            if (p) property_row(ui, n, *p);
        }
        ui.tree_pop();
    }
    ui.end_window();
}

void Editor::panel_console(ui::Context &ui) {
    if (!ui.begin_window("Console", {12, 400, 620, 260})) {
        ui.end_window();
        return;
    }
    for (const std::string &line : console_) {
        uint32_t colour = ui.theme.text;
        if (line.rfind("[error]", 0) == 0) colour = ui.theme.error;
        else if (line.rfind("[warn ]", 0) == 0) colour = ui.theme.warning;
        else if (line.rfind(">>>", 0) == 0) colour = ui.theme.accent;
        ui.text_coloured(colour, "%s", line.c_str());
    }
    ui.separator();
#if WARREN_PYTHON
    if (ui.input_text(">>>##cmd", &command_) && !command_.empty()) {
        log_line(">>> " + command_);
        // An expression prints its value; a statement does not.
        // Trying the expression first is what makes the console
        // usable as a calculator as well as a script host.
        const std::string result = Python::eval_repr(command_);
        if (result.rfind("invalid syntax", 0) != std::string::npos ||
            result.find("SyntaxError") != std::string::npos) {
            Python::run_string(command_, "<console>");
        } else {
            log_line(result);
        }
        command_.clear();
    }
#else
    ui.text_coloured(ui.theme.text_dim, "built without Python");
#endif
    ui.end_window();
}

void Editor::panel_stats(ui::Context &ui, float dt) {
    (void)dt;
    if (!ui.begin_window("Frame", {656, 12, 300, 300})) {
        ui.end_window();
        return;
    }
    ui.text("%.2f ms   %.0f fps", double(frame_ms_), double(fps_));
    const RenderStats &r = engine_->renderer()->stats();
    ui.separator();
    ui.text("draws      %u", r.draw_calls);
    ui.text("triangles  %u", r.triangles);
    ui.text("views      %u  (portals %u deep)", r.views, r.max_depth_reached);
    ui.text("meshes     %u", r.visible_meshes);
    ui.separator();
    ui.text("cascades   %u  (%u draws)", r.cascades, r.shadow_draws);
    ui.text("lights     %u  (%u casting)", r.lights, r.shadow_casting_lights);
    ui.text("clusters   %u views, %u assignments", r.clustered_views,
            r.light_assignments);
    ui.text("atlas      %s",
            r.punctual_shadows_reused ? "reused" : "rebuilt");
    ui.separator();
    ui.text("record     %.2f ms", r.cpu_ms);

    ui.separator();
    ui.text("Scene file");
    ui.input_text("path##scene", &scene_path_);
    if (ui.button("Save")) save_scene(scene_path_);
    ui.same_line();
    if (ui.button("Load")) load_scene(scene_path_);

    ui.separator();
    ui.checkbox("Scene tree", &show_tree_);
    ui.checkbox("Inspector", &show_inspector_);
    ui.checkbox("Console", &show_console_);
    ui.end_window();
}

// --------------------------------------------------- scene files
//
// The work is in resource/packed_scene.cpp, because a game loading a
// level at run time needs it and a game does not link the editor.

std::vector<uint8_t> Editor::serialise_scene(Node *root) const {
    return serialise_tree(root);
}

Node *Editor::deserialise_scene(const uint8_t *data, size_t size) const {
    return deserialise_tree(data, size);
}

bool Editor::save_scene(const std::string &path) {
    if (!engine_ || !engine_->tree()) return false;
    Node *scene = engine_->tree()->scene();
    if (!scene) scene = engine_->tree()->root();
    Ref<PackedScene> packed(new PackedScene());
    if (!packed->pack(scene)) return false;
    if (!packed->save(path)) return false;
    WR_INFO("scene: saved %s (%zu bytes)", path.c_str(), packed->bytes.size());
    return true;
}

bool Editor::load_scene(const std::string &path) {
    if (!engine_ || !engine_->tree()) return false;
    // THROUGH THE RESOURCE LOADER, so that anything which imports to
    // a scene opens the same way: .mfs, .glb, .gltf, and whatever a
    // plugin registers next. "Open a level" and "open a model"
    // should not be two commands.
    Ref<Resource> r = ResourceLoader::load(path);
    PackedScene *packed = r ? r->cast_to<PackedScene>() : nullptr;
    if (!packed || !packed->valid()) {
        WR_ERROR("scene: '%s' did not load as a scene", path.c_str());
        return false;
    }
    Node *scene = packed->instantiate();
    if (!scene) return false;
    selected_.reset();
    engine_->tree()->set_scene(scene);

    // A MODEL FILE HAS NO CAMERA, and a scene with no camera draws
    // nothing at all -- which looks like the import failing rather
    // than succeeding. Opening a model should show you the model, so
    // if the file brought no camera, one is put where the whole of
    // it is visible.
    if (!engine_->tree()->active_camera()) {
        AABB bounds;
        std::vector<Node *> stack{scene};
        while (!stack.empty()) {
            Node *n = stack.back();
            stack.pop_back();
            for (const auto &c : n->children())
                if (c) stack.push_back(c.get());
            if (MeshInstance3D *mi = n->cast_to<MeshInstance3D>())
                if (mi->mesh) bounds.expand(mi->world_bounds());
        }
        if (!bounds.valid()) bounds = AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1));
        const Vec3 centre = bounds.center();
        const float radius = std::max(0.5f, bounds.radius());

        Camera3D *cam = new Camera3D();
        cam->set_name("ViewCamera");
        cam->set_fov_degrees(50.0f);
        cam->set_near(std::max(0.01f, radius * 0.005f));
        cam->set_far(radius * 50.0f);
        // Back off far enough for the bounding sphere to fit the
        // vertical field of view, with a little margin.
        const float distance = radius / std::tan(cam->fov() * 0.5f) * 1.3f;
        cam->set_position(centre + Vec3(0.55f, 0.45f, 1.0f).normalized() *
                                       distance);
        cam->look_at(centre);
        scene->add_child(cam);
        cam->make_current();
        WR_INFO("scene: no camera in the file; framing %.2f m of content",
                double(radius * 2.0f));
    }

    WR_INFO("scene: loaded %s", path.c_str());
    return true;
}

}  // namespace wr
