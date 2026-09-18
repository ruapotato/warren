#include "editor/editor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "app/engine.h"
#include "core/log.h"
#include "core/serialize.h"
#include "render/renderer.h"
#include "render/material.h"
#include "render/mesh.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

#if MANIFOLD_PYTHON
#include "script/python.h"
#endif

namespace mf {
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
    log_line("Manifold editor. F1 toggles, ` focuses the console.");
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
#if MANIFOLD_PYTHON
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

// ---------------------------------------------------------- scene files

namespace {
constexpr uint32_t kSceneMagic = 0x4D465343u;  // 'MFSC'
constexpr uint32_t kSceneVersion = 1;

// A freshly built instance of each class, kept so that saving can
// skip every property still at its default. A scene file that lists
// every field of every node is mostly noise, and noise is what makes
// a diff between two versions of a level unreadable.
Object *default_instance(ClassInfo *ci) {
    static std::unordered_map<ClassInfo *, Object *> cache;
    auto it = cache.find(ci);
    if (it != cache.end()) return it->second;
    Object *o = ci->construct ? ci->construct() : nullptr;
    if (o) o->ref_retain();
    cache[ci] = o;
    return o;
}

// THE RESOURCE TABLE.
//
// A node's mesh and material are objects, and a pointer means
// nothing in a file. Without a resource system -- paths, an
// importer, a cache -- the only correct thing to do is write them
// INLINE, once each, and have the nodes refer to them by index. A
// forty-thousand-triangle scene becomes a megabyte, which is the
// honest cost of a format that does not lose the geometry; when
// there is a resource system, a mesh with a path will write the path
// instead and this stays for the procedural ones.
struct ResourceTable {
    std::vector<Object *> objects;
    std::unordered_map<const Object *, uint32_t> index;

    uint32_t add(Object *o) {
        if (!o) return 0xFFFFFFFFu;
        auto it = index.find(o);
        if (it != index.end()) return it->second;
        const uint32_t i = uint32_t(objects.size());
        objects.push_back(o);
        index[o] = i;
        return i;
    }
};

void collect_resources(Node *n, ResourceTable *table) {
    if (!n) return;
    for (ClassInfo *c = n->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type != VType::Object) continue;
            Object *o = n->get(pname).to_object();
            // A node is not a resource; it is written as a path.
            if (!o || o->cast_to<Node>()) continue;
            table->add(o);
        }
    for (const auto &child : n->children())
        if (child) collect_resources(child.get(), table);
}

void write_resource(ByteWriter &w, Object *o) {
    w.str(o->get_class_name());
    if (Mesh *m = o->cast_to<Mesh>()) {
        // Geometry is not reflected and never will be: a property
        // list is the wrong shape for a hundred thousand vertices.
        w.u32(uint32_t(m->vertices.size()));
        if (!m->vertices.empty())
            w.raw(m->vertices.data(), m->vertices.size() * sizeof(Vertex));
        w.u32(uint32_t(m->indices.size()));
        if (!m->indices.empty())
            w.raw(m->indices.data(), m->indices.size() * sizeof(uint32_t));
        w.u32(uint32_t(m->submeshes.size()));
        for (const SubMesh &sm : m->submeshes) {
            w.u32(sm.first_index);
            w.u32(sm.index_count);
            w.i32(sm.material_slot);
            w.str(sm.name);
        }
        return;
    }
    // Everything else by its reflected properties, which is how a
    // Material works and how anything added later will.
    std::vector<std::pair<std::string, Variant>> props;
    for (ClassInfo *c = o->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type == VType::Object) continue;
            props.emplace_back(pname, o->get(pname));
        }
    w.u32(uint32_t(props.size()));
    for (const auto &kv : props) {
        w.str(kv.first);
        w.variant(kv.second);
    }
}

Object *read_resource(ByteReader &r) {
    const std::string cls = r.str();
    if (!r.ok()) return nullptr;
    Object *o = ClassDB::instantiate(cls);
    if (Mesh *m = o ? o->cast_to<Mesh>() : nullptr) {
        const uint32_t vcount = r.u32();
        // A count is an allocation request from a file: bounded by
        // what is actually there, not by trust.
        if (!r.ok() || uint64_t(vcount) * sizeof(Vertex) > r.left()) {
            r.variant();  // force the failure flag
            delete o;
            return nullptr;
        }
        m->vertices.resize(vcount);
        if (vcount) r.raw(m->vertices.data(), vcount * sizeof(Vertex));
        const uint32_t icount = r.u32();
        if (!r.ok() || uint64_t(icount) * sizeof(uint32_t) > r.left()) {
            delete o;
            return nullptr;
        }
        m->indices.resize(icount);
        if (icount) r.raw(m->indices.data(), icount * sizeof(uint32_t));
        const uint32_t subs = r.u32();
        if (!r.ok() || subs > r.left()) {
            delete o;
            return nullptr;
        }
        for (uint32_t i = 0; i < subs && r.ok(); i++) {
            SubMesh sm;
            sm.first_index = r.u32();
            sm.index_count = r.u32();
            sm.material_slot = r.i32();
            sm.name = r.str();
            m->submeshes.push_back(sm);
        }
        m->compute_bounds();
        return o;
    }
    const uint32_t props = r.u32();
    if (!r.ok() || props > r.left()) {
        if (o) delete o;
        return nullptr;
    }
    for (uint32_t i = 0; i < props && r.ok(); i++) {
        const std::string pname = r.str();
        const Variant v = r.variant();
        if (o && o->has_property(pname)) o->set(pname, v);
    }
    if (o) {
        // Materials cache GPU state keyed on their fields; loading
        // new values behind that cache would show the old ones.
        if (Material *mat = o->cast_to<Material>()) mat->touch();
    }
    return o;
}

// A path relative to the subtree being written, because that is
// what will still mean something when the file is opened. An
// absolute path names the tree it was saved from -- "/root/Demo/..."
// -- and a scene loaded as the new root has no "root" above it, so
// every link in the file points at nothing. The symptom is portals
// that come back unlinked while everything else looks perfect.
std::string relative_path(Node *target, const std::string &root_path) {
    const std::string full = target->path();
    if (full == root_path) return ".";
    if (full.size() > root_path.size() &&
        full.compare(0, root_path.size(), root_path) == 0 &&
        full[root_path.size()] == '/')
        return full.substr(root_path.size() + 1);
    return full;   // outside the subtree; it will warn on load
}

void write_node(ByteWriter &w, Node *n, const ResourceTable &table,
                const std::string &root_path) {
    w.str(n->get_class_name());
    w.str(n->name());

    // Only what differs from a new one of the same class.
    ClassInfo *ci = n->get_class_info();
    Object *fresh = default_instance(ci);
    std::vector<std::pair<std::string, Variant>> changed;
    for (ClassInfo *c = ci; c; c = c->base) {
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set) continue;          // read-only
            if (p->transient) continue;           // a view, not the state
            if (p->type == VType::Object) continue;  // a pointer means nothing
            const Variant v = n->get(pname);
            if (fresh && fresh->has_property(pname) &&
                v == fresh->get(pname))
                continue;
            changed.emplace_back(pname, v);
        }
    }
    w.u32(uint32_t(changed.size()));
    for (const auto &kv : changed) {
        w.str(kv.first);
        w.variant(kv.second);
    }

    // Object-valued properties come in two kinds and they are not
    // interchangeable. A MESH is a resource: shared, written once,
    // referred to by index. ANOTHER NODE -- the portal this portal
    // is linked to, the body a camera follows -- is part of the
    // scene, and the only thing that identifies it is where it sits
    // in the tree. Writing a node into the resource table would
    // duplicate it; writing a mesh as a path would name nothing.
    std::vector<std::pair<std::string, uint32_t>> refs;
    std::vector<std::pair<std::string, std::string>> links;
    for (ClassInfo *c = ci; c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type != VType::Object) continue;
            Object *o = n->get(pname).to_object();
            if (!o) continue;
            if (Node *other = o->cast_to<Node>()) {
                if (other->is_inside_tree() || other->root() == n->root())
                    links.emplace_back(pname, relative_path(other, root_path));
                continue;
            }
            auto it = table.index.find(o);
            if (it != table.index.end()) refs.emplace_back(pname, it->second);
        }
    w.u32(uint32_t(refs.size()));
    for (const auto &kv : refs) {
        w.str(kv.first);
        w.u32(kv.second);
    }
    w.u32(uint32_t(links.size()));
    for (const auto &kv : links) {
        w.str(kv.first);
        w.str(kv.second);
    }

    uint32_t children = 0;
    for (const auto &c : n->children())
        if (c) children++;
    w.u32(children);
    for (const auto &c : n->children())
        if (c) write_node(w, c.get(), table, root_path);
}

// A reference to another node, kept until the whole tree exists.
// Resolving as we go cannot work: the portal at the top of a scene
// is linked to one near the bottom, which has not been read yet.
struct PendingLink {
    Node *from = nullptr;
    std::string property;
    std::string path;
};

Node *read_node(ByteReader &r, const std::vector<Ref<Object>> &resources,
                std::vector<PendingLink> *links) {
    const std::string cls = r.str();
    const std::string name = r.str();
    if (!r.ok()) return nullptr;
    Object *o = ClassDB::instantiate(cls);
    Node *n = o ? o->cast_to<Node>() : nullptr;
    if (!n) {
        // UNKNOWN CLASS: READ PAST IT, DO NOT GIVE UP. A scene saved
        // with a plugin loaded should still open without it, missing
        // those nodes and keeping the rest -- which is the
        // difference between a level you can repair and one you have
        // lost.
        if (o) delete o;
        MF_WARN("scene: no class '%s'; its node '%s' is skipped", cls.c_str(),
                name.c_str());
    } else {
        n->set_name(name);
    }

    const uint32_t props = r.u32();
    for (uint32_t i = 0; i < props && r.ok(); i++) {
        const std::string pname = r.str();
        const Variant v = r.variant();
        if (n && n->has_property(pname)) n->set(pname, v);
    }
    const uint32_t refs = r.u32();
    for (uint32_t i = 0; i < refs && r.ok(); i++) {
        const std::string pname = r.str();
        const uint32_t index = r.u32();
        if (!n || index >= resources.size()) continue;
        if (n->has_property(pname))
            n->set(pname, Variant(resources[index].get()));
    }

    const uint32_t link_count = r.u32();
    for (uint32_t i = 0; i < link_count && r.ok(); i++) {
        PendingLink link;
        link.from = n;
        link.property = r.str();
        link.path = r.str();
        if (n) links->push_back(link);
    }

    const uint32_t children = r.u32();
    for (uint32_t i = 0; i < children && r.ok(); i++) {
        Node *child = read_node(r, resources, links);
        if (!child) continue;
        if (n)
            n->add_child(child);
        else
            child->queue_free();  // its parent did not survive
    }
    return n;
}
}  // namespace

std::vector<uint8_t> Editor::serialise_scene(Node *root) const {
    ByteWriter w;
    w.u32(kSceneMagic);
    w.u32(kSceneVersion);
    if (!root) {
        w.u32(0);   // no resources
        w.u32(0);   // and no root
        return std::move(w.bytes);
    }
    ResourceTable table;
    collect_resources(root, &table);
    w.u32(uint32_t(table.objects.size()));
    for (Object *o : table.objects) write_resource(w, o);

    w.u32(1);
    write_node(w, root, table, root->path());
    return std::move(w.bytes);
}

Node *Editor::deserialise_scene(const uint8_t *data, size_t size) const {
    ByteReader r(data, size);
    if (r.u32() != kSceneMagic) {
        MF_ERROR("scene: not a Manifold scene file");
        return nullptr;
    }
    const uint32_t version = r.u32();
    if (version != kSceneVersion) {
        MF_ERROR("scene: version %u, this build reads %u", version,
                 kSceneVersion);
        return nullptr;
    }
    const uint32_t resource_count = r.u32();
    if (!r.ok() || resource_count > 1u << 20) {
        MF_ERROR("scene: an implausible resource count");
        return nullptr;
    }
    std::vector<Ref<Object>> resources;
    resources.reserve(resource_count);
    for (uint32_t i = 0; i < resource_count && r.ok(); i++)
        resources.push_back(Ref<Object>(read_resource(r)));
    if (!r.ok()) {
        MF_ERROR("scene: the resource table is truncated");
        return nullptr;
    }

    if (r.u32() == 0) return nullptr;
    std::vector<PendingLink> links;
    Node *n = read_node(r, resources, &links);
    if (!r.ok()) {
        MF_ERROR("scene: the file ended in the middle of a node");
        if (n) n->queue_free();
        return nullptr;
    }

    // NOW the tree exists, so a path means something. A path that
    // resolves to nothing is a warning and not a failure: a scene
    // referring to a node someone deleted should still open.
    for (const PendingLink &link : links) {
        if (!link.from) continue;
        // Relative to the scene root, which is what was written.
        Node *target = link.path == "." ? n : n->find_path(link.path);
        if (!target) {
            MF_WARN("scene: '%s' points at '%s', which is not in the file",
                    link.from->name().c_str(), link.path.c_str());
            continue;
        }
        if (link.from->has_property(link.property))
            link.from->set(link.property, Variant(static_cast<Object *>(target)));
    }
    return n;
}

bool Editor::save_scene(const std::string &path) {
    if (!engine_ || !engine_->tree()) return false;
    Node *scene = engine_->tree()->scene();
    if (!scene) scene = engine_->tree()->root();
    const std::vector<uint8_t> bytes = serialise_scene(scene);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        MF_ERROR("scene: could not write '%s'", path.c_str());
        return false;
    }
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        MF_ERROR("scene: short write to '%s'", path.c_str());
        return false;
    }
    MF_INFO("scene: saved %s (%zu bytes)", path.c_str(), bytes.size());
    return true;
}

bool Editor::load_scene(const std::string &path) {
    if (!engine_ || !engine_->tree()) return false;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        MF_ERROR("scene: could not open '%s'", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size_t(std::max(0L, size)));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);

    Node *scene = deserialise_scene(bytes.data(), bytes.size());
    if (!scene) return false;
    selected_.reset();
    engine_->tree()->set_scene(scene);
    MF_INFO("scene: loaded %s", path.c_str());
    return true;
}

}  // namespace mf
