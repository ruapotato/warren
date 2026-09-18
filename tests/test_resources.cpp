// Warren -- assets that live in files and are shared.
//
// A scene used to write its meshes inline, which is correct and does
// not scale: a level of forty rooms each using the same crate is
// forty copies of a crate, and editing the crate edits none of them.
// The fix is the same one scene instancing uses, a layer down -- a
// reference to a file, with inline kept only for what has no file.
//
// So the measurements here are about identity and size. Loading a
// path twice must give THE SAME OBJECT, because "the same material"
// is what a renderer batches by. And a scene referring to a mesh
// file must be small, because that is the entire point.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.h"
#include "render/material.h"
#include "render/mesh.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/nodes.h"
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

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    std::printf("resources\n");

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "warren_resource_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ResourceLoader::set_base_directory(dir.string());
    ResourceLoader::forget_all();

    // ----------------------------------------- saving and loading a mesh
    {
        Ref<Mesh> box = Mesh::box({2, 1, 3});
        const size_t vertices = box->vertices.size();
        check(ResourceSaver::save(box.get(), "crate.mesh"),
              "a mesh can be written to a file");
        check(box->resource_path() == "crate.mesh",
              "and it remembers where it went");

        Ref<Resource> loaded = ResourceLoader::load("crate.mesh");
        Mesh *m = loaded ? loaded->cast_to<Mesh>() : nullptr;
        check(m != nullptr, "and read back as a mesh");
        char what[200];
        std::snprintf(what, sizeof(what), "with its geometry (%zu vertices)",
                      m ? m->vertices.size() : 0);
        check(m && m->vertices.size() == vertices, what);
        check(m && m->submeshes.size() == box->submeshes.size(),
              "and its submeshes");

        // THE CACHE IS PART OF THE CONTRACT. Two loads of one path
        // are one object, or a renderer batching by material pointer
        // sees two materials where the project has one.
        Ref<Resource> again = ResourceLoader::load("crate.mesh");
        check(again.get() == loaded.get(),
              "loading the same path twice gives the same object");
        check(ResourceLoader::cached_count() == 1, "and caches it once");
    }

    // ------------------------------------------------- a material file
    {
        Ref<Material> mat = Material::make(Color(0.8f, 0.3f, 0.1f, 1), 0.35f);
        mat->metallic = 0.75f;
        check(ResourceSaver::save(mat.get(), "rust.mat"),
              "a material can be written");
        Ref<Resource> loaded = ResourceLoader::load("rust.mat");
        Material *m = loaded ? loaded->cast_to<Material>() : nullptr;
        check(m != nullptr, "and read back");
        check(m && std::fabs(m->metallic - 0.75f) < 1e-3f,
              "with its fields");
        check(m && std::fabs(m->albedo.r - mat->albedo.r) < 1e-3f,
              "including its colour");
    }

    // ------------------------ a scene that REFERS rather than copies
    {
        Ref<Resource> crate = ResourceLoader::load("crate.mesh");
        Ref<Resource> rust = ResourceLoader::load("rust.mat");
        Mesh *mesh = crate->cast_to<Mesh>();
        Material *mat = rust->cast_to<Material>();

        Node3D *room = new Node3D();
        room->set_name("Room");
        for (int i = 0; i < 40; i++) {
            MeshInstance3D *mi = new MeshInstance3D();
            char name[16];
            std::snprintf(name, sizeof(name), "crate%d", i);
            mi->set_name(name);
            mi->mesh = Ref<Mesh>(mesh);
            mi->set_material(0, mat);
            mi->set_position({float(i) * 2.0f, 0, 0});
            room->add_child(mi);
        }
        room->set_owner_recursive(room);
        room->set_owner(nullptr);

        Ref<PackedScene> scene(new PackedScene());
        scene->pack(room);

        const size_t mesh_bytes =
            mesh->vertices.size() * sizeof(Vertex) +
            mesh->indices.size() * sizeof(uint32_t);

        // THE DIRECT MEASUREMENT: the same scene, with the mesh's
        // path taken away so it has nowhere to be loaded from and
        // must be written in full. The difference between the two
        // files is exactly the geometry, which is what a reference
        // buys -- and forty nodes cost what forty nodes cost either
        // way, which is why "smaller than one mesh" was the wrong
        // question to ask.
        const std::string saved_path = mesh->resource_path();
        mesh->set_resource_path("");
        Ref<PackedScene> inlined(new PackedScene());
        inlined->pack(room);
        mesh->set_resource_path(saved_path);

        std::printf("  forty crates sharing one mesh:\n"
                    "    %zu bytes referring to it, %zu bytes inlining it, "
                    "mesh alone %zu\n",
                    scene->bytes.size(), inlined->bytes.size(), mesh_bytes);
        char what[240];
        std::snprintf(what, sizeof(what),
                      "referring to the mesh leaves it out of the scene "
                      "(%zu against %zu, mesh is %zu)",
                      scene->bytes.size(), inlined->bytes.size(), mesh_bytes);
        check(inlined->bytes.size() > scene->bytes.size() + mesh_bytes / 2,
              what);

        Node *back = scene->instantiate();
        check(back != nullptr, "and it reads back");
        if (back) {
            Node *a = back->find_child("crate0");
            Node *z = back->find_child("crate39");
            MeshInstance3D *ma = a ? a->cast_to<MeshInstance3D>() : nullptr;
            MeshInstance3D *mz = z ? z->cast_to<MeshInstance3D>() : nullptr;
            check(ma && ma->mesh, "with its mesh");
            // AND IT IS THE VERY SAME MESH -- not a copy each, and
            // not even one shared copy, but the object already in
            // memory, so that editing it in a tool shows everywhere
            // at once.
            check(ma && mz && ma->mesh.get() == mz->mesh.get(),
                  "shared between all forty");
            check(ma && ma->mesh.get() == mesh,
                  "and it is the object already loaded, not a new one");
            check(ma && ma->first_material() == mat,
                  "the material too");
            back->queue_free();
            delete back;
        }
        room->queue_free();
        delete room;
    }

    // ------------------------------- a procedural mesh is still inlined
    {
        // Nothing to refer to, so it has to be written in full --
        // which is the case the old format handled and the new one
        // must not lose.
        Node3D *root = new Node3D();
        root->set_name("Procedural");
        MeshInstance3D *mi = new MeshInstance3D();
        mi->set_name("Generated");
        mi->mesh = Mesh::sphere(1.0f, 8, 12);
        root->add_child(mi);
        root->set_owner_recursive(root);
        root->set_owner(nullptr);

        Ref<PackedScene> scene(new PackedScene());
        scene->pack(root);
        Node *back = scene->instantiate();
        Node *g = back ? back->find_child("Generated") : nullptr;
        MeshInstance3D *bm = g ? g->cast_to<MeshInstance3D>() : nullptr;
        check(bm && bm->mesh &&
                  bm->mesh->vertices.size() == mi->mesh->vertices.size(),
              "a mesh with no file is still written in full");
        if (back) { back->queue_free(); delete back; }
        root->queue_free();
        delete root;
    }

    // --------------------------------------- a file that is not there
    {
        log_set_level(LogLevel::Fatal);
        Ref<Resource> missing = ResourceLoader::load("nothing.mesh");
        check(!missing, "a missing file loads as nothing");
        Ref<Resource> unknown = ResourceLoader::load("thing.xyzzy");
        check(!unknown, "and an unreadable extension is refused");
        log_set_level(LogLevel::Warn);
    }

    ResourceLoader::forget_all();
    std::filesystem::remove_all(dir, ec);
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
