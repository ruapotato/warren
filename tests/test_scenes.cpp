// Warren -- scenes as things you can have more than one of.
//
// A tree you can save and load is a level format. A tree you can
// save, load and then INSTANCE FIFTY TIMES INSIDE ANOTHER TREE is
// how a game gets built, and the difference between the two is
// entirely in what happens when you edit the thing that was
// instanced. So that is what this tests: change the door, and every
// door in the corridor changes -- except the one someone had
// deliberately made different, which does not.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "render/material.h"
#include "render/texture.h"
#include "render/mesh.h"
#include "resource/packed_scene.h"
#include "scene/nodes.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

// Scenes live in memory for the test, resolved by name. The engine
// asks through PackedScene::resolve precisely so that a game, a pak
// file or a test can answer however it likes.
std::vector<std::pair<std::string, Ref<PackedScene>>> g_library;

Ref<PackedScene> library_resolver(const std::string &path) {
    for (auto &kv : g_library)
        if (kv.first == path) return kv.second;
    return {};
}

Ref<PackedScene> publish(const std::string &path, Node *root) {
    Ref<PackedScene> scene(new PackedScene());
    scene->pack(root);
    scene->path = path;
    for (auto &kv : g_library)
        if (kv.first == path) {
            kv.second = scene;
            return scene;
        }
    g_library.emplace_back(path, scene);
    return scene;
}

// A door: a frame with a handle on it.
Node3D *build_door(float handle_height) {
    Node3D *door = new Node3D();
    door->set_name("Door");
    MeshInstance3D *frame = new MeshInstance3D();
    frame->set_name("Frame");
    frame->mesh = Mesh::box({1, 2, 0.1f});
    door->add_child(frame);
    Node3D *handle = new Node3D();
    handle->set_name("Handle");
    handle->set_position({0.4f, handle_height, 0});
    frame->add_child(handle);
    door->set_owner_recursive(door);
    door->set_owner(nullptr);
    return door;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    PackedScene::set_resolver(&library_resolver);
    std::printf("scenes\n");

    // ------------------------------------------- pack and instance
    {
        Node3D *door = build_door(1.0f);
        Ref<PackedScene> scene = publish("door.mfs", door);
        check(scene && scene->valid(), "a scene can be packed");

        Node *a = scene->instantiate();
        Node *b = scene->instantiate();
        check(a && b && a != b, "and instanced more than once");
        check(a && a->find_path("Frame/Handle") != nullptr,
              "with its whole tree");
        check(a && b && a->find_path("Frame/Handle") !=
                            b->find_path("Frame/Handle"),
              "and the two instances are separate nodes");

        // EVERY NODE INSIDE KNOWS WHICH SCENE IT CAME FROM. That is
        // what lets a tree containing it write one line.
        Node *handle = a ? a->find_path("Frame/Handle") : nullptr;
        check(handle && handle->owner() == a,
              "every node inside belongs to the instance");
        check(a && a->scene_path() == "door.mfs",
              "and the instance remembers where it came from");

        if (a) { a->queue_free(); delete a; }
        if (b) { b->queue_free(); delete b; }
        door->queue_free();
        delete door;
    }

    // --------------------------- a corridor of them, saved and reloaded
    {
        Node3D *door = build_door(1.0f);
        Ref<PackedScene> door_scene = publish("door.mfs", door);

        Node3D *corridor = new Node3D();
        corridor->set_name("Corridor");
        for (int i = 0; i < 8; i++) {
            Node *d = door_scene->instantiate();
            char name[16];
            std::snprintf(name, sizeof(name), "door%d", i);
            d->set_name(name);
            static_cast<Node3D *>(d)->set_position({float(i) * 3.0f, 0, 0});
            corridor->add_child(d);
        }
        // One door is special: someone moved its handle.
        Node *third = corridor->find_child("door3");
        Node3D *odd_handle =
            third ? static_cast<Node3D *>(third->find_path("Frame/Handle"))
                  : nullptr;
        if (odd_handle) odd_handle->set_position({0.4f, 1.7f, 0});
        // And someone hung a lamp inside another one.
        Node *fifth = corridor->find_child("door5");
        OmniLight3D *lamp = new OmniLight3D();
        lamp->set_name("Lamp");
        lamp->range = 4.0f;
        if (fifth) fifth->add_child(lamp);   // owner stays null: ours

        corridor->set_owner_recursive(corridor);
        corridor->set_owner(nullptr);
        // The doors keep their own ownership -- set_owner_recursive
        // stops at an instance boundary, which is the rule that
        // makes all of this work.
        // The placement belongs to the corridor -- that is what
        // makes the corridor responsible for writing it -- while
        // everything inside belongs to the door.
        check(third && third->owner() == corridor,
              "an instanced root belongs to the scene holding it");
        Node *inner = third ? third->find_path("Frame") : nullptr;
        check(inner && inner->owner() == third,
              "but its contents still belong to it");

        Ref<PackedScene> corridor_scene = publish("corridor.mfs", corridor);
        const size_t one_door = door_scene->bytes.size();
        std::printf("  one door is %zu bytes; a corridor of eight is %zu\n",
                    one_door, corridor_scene->bytes.size());

        // THE MEASUREMENT THAT MATTERS. Eight doors written as eight
        // references and a handful of overrides, not as eight
        // copies of a door.
        char what[220];
        std::snprintf(what, sizeof(what),
                      "eight doors cost less than two copies of one (%zu "
                      "against %zu)",
                      corridor_scene->bytes.size(), one_door * 2);
        check(corridor_scene->bytes.size() < one_door * 2, what);

        Node *back = corridor_scene->instantiate();
        check(back != nullptr, "the corridor reads back");
        if (back) {
            int found = 0;
            for (int i = 0; i < 8; i++) {
                char name[16];
                std::snprintf(name, sizeof(name), "door%d", i);
                Node *d = back->find_child(name);
                if (d && d->find_path("Frame/Handle")) found++;
            }
            std::snprintf(what, sizeof(what), "with all eight doors (%d)",
                          found);
            check(found == 8, what);

            // Positions are overrides and must have survived.
            Node *d6 = back->find_child("door6");
            Node3D *d63 = d6 ? d6->cast_to<Node3D>() : nullptr;
            check(d63 && std::fabs(d63->position().x - 18.0f) < 1e-3f,
                  "each in the place it was put");

            // The deliberately different one is still different.
            Node *t = back->find_child("door3");
            Node3D *h = t ? static_cast<Node3D *>(t->find_path("Frame/Handle"))
                          : nullptr;
            std::snprintf(what, sizeof(what),
                          "the door someone changed is still changed (%.2f)",
                          h ? double(h->position().y) : -1.0);
            check(h && std::fabs(h->position().y - 1.7f) < 1e-3f, what);

            // And the ones nobody touched are not.
            Node *u = back->find_child("door4");
            Node3D *uh = u ? static_cast<Node3D *>(u->find_path("Frame/Handle"))
                           : nullptr;
            check(uh && std::fabs(uh->position().y - 1.0f) < 1e-3f,
                  "and the ones nobody touched are not");

            // The lamp hung inside an instance belongs to the outer
            // scene and is written in full, so it comes back.
            Node *f = back->find_child("door5");
            check(f && f->find_child("Lamp") != nullptr,
                  "a node added to an instance by hand survives");

            back->queue_free();
            delete back;
        }

        // ------------------------------------- NOW EDIT THE DOOR
        //
        // This is the whole reason scenes exist. Change door.mfs and
        // every placement of it changes, except the ones that were
        // deliberately overridden.
        Node3D *taller = build_door(1.4f);
        publish("door.mfs", taller);

        Node *after = corridor_scene->instantiate();
        check(after != nullptr, "the corridor still loads after the edit");
        if (after) {
            Node *u = after->find_child("door4");
            Node3D *uh = u ? static_cast<Node3D *>(u->find_path("Frame/Handle"))
                           : nullptr;
            std::snprintf(what, sizeof(what),
                          "editing the door moved every untouched handle "
                          "(%.2f, was 1.00)",
                          uh ? double(uh->position().y) : -1.0);
            check(uh && std::fabs(uh->position().y - 1.4f) < 1e-3f, what);

            Node *t = after->find_child("door3");
            Node3D *h = t ? static_cast<Node3D *>(t->find_path("Frame/Handle"))
                          : nullptr;
            std::snprintf(what, sizeof(what),
                          "and left the overridden one alone (%.2f)",
                          h ? double(h->position().y) : -1.0);
            check(h && std::fabs(h->position().y - 1.7f) < 1e-3f, what);

            after->queue_free();
            delete after;
        }
        taller->queue_free();
        delete taller;
        corridor->queue_free();
        delete corridor;
        door->queue_free();
        delete door;
    }

    // ---------------------------------- a scene that is not there
    {
        Node3D *holder = new Node3D();
        holder->set_name("Holder");
        Node3D *ghost = new Node3D();
        ghost->set_name("Ghost");
        ghost->set_scene_path("nothing.mfs");
        holder->add_child(ghost);
        holder->set_owner_recursive(holder);
        holder->set_owner(nullptr);

        Ref<PackedScene> s(new PackedScene());
        s->pack(holder);
        log_set_level(LogLevel::Fatal);
        Node *back = s->instantiate();
        log_set_level(LogLevel::Warn);
        check(back != nullptr,
              "a level whose scene is missing still opens, with a hole");
        if (back) { back->queue_free(); delete back; }
        holder->queue_free();
        delete holder;
    }

    // ------------------------------------- a resource holding a resource
    //
    // A material's textures are resources of its own, and the scene
    // format used to walk one level: it collected a node's material
    // and stopped. Every model imported from glTF came back
    // untextured, nothing was logged, and the material was there
    // with its map simply gone -- which reads as a lighting problem
    // for as long as you are willing to believe it is one.
    //
    // No device here, so the texture cannot be decoded or uploaded;
    // what is checked is that its BYTES travel. That is the part
    // that was missing.
    {
        Node3D *root = new Node3D();
        root->set_name("Gallery");

        MeshInstance3D *mi = new MeshInstance3D();
        mi->set_name("Wall");
        mi->mesh = Mesh::box(Vec3(1, 1, 1));
        Ref<Material> mat(new Material());
        mat->albedo = Color(1, 1, 1, 1);
        mi->set_material(0, mat.get());
        root->add_child(mi);

        const std::vector<uint8_t> bare = serialise_tree(root);

        // A texture carrying a recognisable run of bytes. Not a real
        // PNG: nothing here decodes it, and a pattern is easier to
        // find in a blob than a valid file would be.
        std::vector<uint8_t> payload(4096);
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] = uint8_t(0xA5 ^ (i & 0xFF));
        Ref<Texture> tex(new Texture());
        tex->keep_source(payload.data(), payload.size(), true);
        mat->albedo_map = tex;
        mat->touch();

        const std::vector<uint8_t> dressed = serialise_tree(root);
        check(dressed.size() >= bare.size() + payload.size(),
              "a material's texture is written with the scene");

        // And the actual bytes, not merely that it got bigger.
        auto at = std::search(dressed.begin(), dressed.end(),
                              payload.begin(), payload.end());
        check(at != dressed.end(),
              "and it is the texture's own bytes that are written");

        root->queue_free();
        delete root;
    }

    PackedScene::set_resolver(nullptr);
    g_library.clear();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
