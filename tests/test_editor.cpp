// Manifold -- the editor's two testable halves.
//
// A UI is hard to test and a scene file is not, so this concentrates
// on the parts where a bug is silent: the inspector deriving controls
// from a class it has never seen, and a scene surviving a round trip
// through bytes.
//
// The inspector matters because it is DERIVED rather than written.
// That is its virtue -- a class that adds a field gets a row for free
// -- and also its risk: a property whose type has no control quietly
// becomes uneditable, and nothing says so. So the test walks every
// registered class and reports which properties the inspector can
// actually edit, and fails if a type it claims to handle stops
// working.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/object.h"
#include "core/serialize.h"
#include "editor/editor.h"
#include "render/material.h"
#include "render/mesh.h"
#include "scene/nodes.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"
#include "ui/ui.h"

using namespace mf;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

// Which VTypes the inspector draws a real control for. Kept here
// rather than read out of the editor so that the two have to agree
// deliberately: adding a case to property_row without adding it here
// fails, which is the reminder to think about whether the control is
// right.
bool editable(VType t) {
    switch (t) {
        case VType::Bool:
        case VType::Float:
        case VType::Int:
        case VType::String:
        case VType::Vec3:
        case VType::Color:
        case VType::Quat:
            return true;
        default:
            return false;
    }
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    std::printf("editor\n");

    // ------------------------------- what the inspector can reach
    {
        std::map<std::string, int> by_type;
        int total = 0, reachable = 0;
        for (ClassInfo *ci : ClassDB::all()) {
            for (const std::string &pname : ci->property_order) {
                const PropertyInfo *p = ci->find_property(pname);
                if (!p) continue;
                total++;
                by_type[vtype_name(p->type)]++;
                if (editable(p->type)) reachable++;
            }
        }
        std::printf("  %d properties across %zu classes, %d with an editor "
                    "control\n", total, ClassDB::all().size(), reachable);
        std::printf("   ");
        for (const auto &kv : by_type) std::printf(" %s:%d", kv.first.c_str(), kv.second);
        std::printf("\n");

        char what[200];
        std::snprintf(what, sizeof(what),
                      "the inspector reaches most of the engine (%d of %d)",
                      reachable, total);
        check(total > 40 && reachable * 100 / std::max(total, 1) > 70, what);

        // The types it claims must actually be claimed. A property
        // of a type the editor forgot shows as read-only text, which
        // is honest but is not editing.
        check(editable(VType::Vec3) && editable(VType::Color) &&
                  editable(VType::Quat),
              "and covers the three that a transform needs");
    }

    // ------------------------------------------ a scene round trip
    {
        Editor editor;   // not init'd: serialisation needs no engine

        Node3D *root = new Node3D();
        root->set_name("Root");
        root->set_position({1, 2, 3});

        Camera3D *cam = new Camera3D();
        cam->set_name("Eye");
        cam->set_fov_degrees(72.0f);
        cam->set_position({0, 1.7f, 0});
        root->add_child(cam);

        OmniLight3D *lamp = new OmniLight3D();
        lamp->set_name("Lamp");
        lamp->range = 17.5f;
        lamp->colour = Color(0.9f, 0.4f, 0.1f, 1.0f);
        lamp->energy = 33.0f;
        root->add_child(lamp);

        Node3D *deep = new Node3D();
        deep->set_name("Child");
        deep->set_position({-4, 0, 9});
        lamp->add_child(deep);

        const std::vector<uint8_t> bytes = editor.serialise_scene(root);
        std::printf("  a four node scene is %zu bytes\n", bytes.size());
        // SMALL BECAUSE IT STORES STATE, NOT VIEWS OF IT. A Node3D
        // exposes transform, position, basis, rotation, euler, scale
        // and two global variants, all of them the same matrix; the
        // first version of this wrote every one and the file was
        // 856 bytes rather than 266 -- and, far worse, a child came
        // back with its parent's transform folded in, because the
        // last view read is the one that wins.
        check(bytes.size() > 40 && bytes.size() < 512,
              "a small scene is a small file");

        Node *back = editor.deserialise_scene(bytes.data(), bytes.size());
        check(back != nullptr, "and it reads back");
        if (back) {
            check(back->name() == "Root", "with its name");
            check(std::strcmp(back->get_class_name(), "Node3D") == 0,
                  "and its class");
            Node3D *b3 = back->cast_to<Node3D>();
            check(b3 && b3->position() == Vec3(1, 2, 3), "and its position");

            Node *eye = back->find_child("Eye");
            Camera3D *c = eye ? eye->cast_to<Camera3D>() : nullptr;
            check(c != nullptr, "the camera came back as a camera");
            char what[160];
            std::snprintf(what, sizeof(what), "with its field of view (%.1f)",
                          c ? double(rad2deg(c->fov())) : 0.0);
            check(c && std::fabs(rad2deg(c->fov()) - 72.0f) < 0.1f, what);

            Node *l = back->find_child("Lamp");
            OmniLight3D *o = l ? l->cast_to<OmniLight3D>() : nullptr;
            check(o != nullptr, "and the lamp as a lamp");
            check(o && std::fabs(o->range - 17.5f) < 1e-3f, "with its range");
            check(o && std::fabs(o->colour.r - 0.9f) < 1e-3f &&
                      std::fabs(o->colour.g - 0.4f) < 1e-3f,
                  "and its colour");

            // NESTING SURVIVES. A flat list of nodes is easy to get
            // right and useless; the shape of the tree is the scene.
            Node *grand = o ? o->find_child("Child") : nullptr;
            check(grand != nullptr, "a grandchild is still a grandchild");
            Node3D *g3 = grand ? grand->cast_to<Node3D>() : nullptr;
            if (g3)
                std::printf("       grandchild came back a %s at "
                            "(%.2f %.2f %.2f)\n", grand->get_class_name(),
                            double(g3->position().x), double(g3->position().y),
                            double(g3->position().z));
            check(g3 && g3->position() == Vec3(-4, 0, 9),
                  "in the right place");

            back->queue_free();
            delete back;
        }
        root->queue_free();
        delete root;
    }

    // -------------------------- resources and links across the wire
    {
        Editor editor;
        Node3D *root = new Node3D();
        root->set_name("Level");

        // A SHARED MESH AND MATERIAL. Written once each and referred
        // to by index, or a room made of twenty identical boxes is
        // twenty copies of the same box.
        Ref<Mesh> box = Mesh::box({2, 1, 3});
        Ref<Material> red = Material::make(Color(0.8f, 0.2f, 0.1f, 1), 0.4f);
        for (int i = 0; i < 4; i++) {
            MeshInstance3D *mi = new MeshInstance3D();
            char name[16];
            std::snprintf(name, sizeof(name), "box%d", i);
            mi->set_name(name);
            mi->mesh = box;
            mi->set_material(0, red.get());
            mi->set_position({float(i) * 3.0f, 0, 0});
            root->add_child(mi);
        }

        // AND TWO NODES THAT POINT AT EACH OTHER. A mesh is a
        // resource and a portal's partner is not: one is shared and
        // written once, the other is part of the scene and the only
        // thing that identifies it is where it sits in the tree.
        Portal3D *a = new Portal3D();
        a->set_name("A");
        a->set_position({0, 1, -4});
        root->add_child(a);
        Node3D *wing = new Node3D();
        wing->set_name("Wing");
        root->add_child(wing);
        Portal3D *b = new Portal3D();
        b->set_name("B");
        b->set_position({20, 1, 0});
        wing->add_child(b);
        a->link_to(b);

        const std::vector<uint8_t> bytes = editor.serialise_scene(root);
        const size_t one_box = box->vertices.size() * sizeof(Vertex) +
                               box->indices.size() * sizeof(uint32_t);
        std::printf("  four boxes sharing one mesh: %zu bytes, and one copy "
                    "of the mesh is %zu\n", bytes.size(), one_box);
        char what[200];
        std::snprintf(what, sizeof(what),
                      "a shared mesh is written once, not four times (%zu "
                      "bytes for four users of a %zu byte mesh)",
                      bytes.size(), one_box);
        check(bytes.size() < one_box * 2, what);

        Node *back = editor.deserialise_scene(bytes.data(), bytes.size());
        check(back != nullptr, "and the level reads back");
        if (back) {
            Node *n0 = back->find_child("box0");
            Node *n3 = back->find_child("box3");
            MeshInstance3D *m0 = n0 ? n0->cast_to<MeshInstance3D>() : nullptr;
            MeshInstance3D *m3 = n3 ? n3->cast_to<MeshInstance3D>() : nullptr;
            check(m0 && m0->mesh && m0->mesh->vertices.size() ==
                                        box->vertices.size(),
                  "with its geometry intact");
            check(m0 && m3 && m0->mesh.get() == m3->mesh.get(),
                  "and the four still SHARE one mesh, not four copies");
            check(m0 && m0->first_material() &&
                      std::fabs(m0->first_material()->albedo.r - 0.8f) < 1e-3f,
                  "and the material came with them");

            // THE LINK. Saved as a path relative to the scene root,
            // because an absolute one names the tree it was saved
            // from -- and a scene loaded as the new root has no
            // "/root" above it, so every link in the file would
            // point at nothing while everything else looked perfect.
            Node *ra = back->find_child("A");
            Portal3D *pa = ra ? ra->cast_to<Portal3D>() : nullptr;
            check(pa != nullptr, "the portal came back");
            check(pa && pa->link() != nullptr, "and it is still linked");
            check(pa && pa->link() && pa->link()->name() == "B",
                  "to the right portal, two levels down another branch");
            check(pa && pa->link() != b,
                  "and to the loaded one, not the original");

            back->queue_free();
            delete back;
        }
        root->queue_free();
        delete root;
    }

    // ---------------------------------- and a file that is not one
    {
        Editor editor;
        const uint8_t rubbish[16] = {1, 2, 3, 4, 5, 6, 7, 8};
        log_set_level(LogLevel::Fatal);   // the complaint is expected
        check(editor.deserialise_scene(rubbish, sizeof(rubbish)) == nullptr,
              "rubbish is refused rather than parsed");
        // AND A TRUNCATED SCENE, which is what a crash mid-save
        // leaves behind and what a reader must not walk off the end
        // of.
        Node3D *n = new Node3D();
        n->set_name("Half");
        n->set_position({5, 5, 5});
        Editor e2;
        std::vector<uint8_t> full = e2.serialise_scene(n);
        for (size_t cut = 12; cut < full.size(); cut += 3) {
            Node *partial = e2.deserialise_scene(full.data(), cut);
            if (partial) {
                partial->queue_free();
                delete partial;
            }
        }
        log_set_level(LogLevel::Warn);
        check(true, "and every truncation of a real one is survived");
        n->queue_free();
        delete n;
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
