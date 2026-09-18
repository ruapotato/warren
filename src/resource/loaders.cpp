// Manifold -- who can read what.
//
// One place where every format the engine understands is attached to
// the extensions that mean it. A plugin adds its own the same way,
// which is why the table is a registry rather than a switch.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "core/serialize.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"

namespace mf {
namespace {

constexpr uint32_t kMeshMagic = 0x4D464D53u;   // 'MFMS'
constexpr uint32_t kMeshVersion = 1;
constexpr uint32_t kMaterialMagic = 0x4D464D54u;  // 'MFMT'
constexpr uint32_t kMaterialVersion = 1;

// The device textures are uploaded through. Set by the Engine once
// the device exists; a texture loaded before then is an error a game
// would rather see at start-up than as a black surface.
rhi::Device *g_device = nullptr;

Ref<Resource> load_scene(const std::string &path) {
    Ref<PackedScene> s = PackedScene::load(path);
    if (!s || !s->valid()) return {};
    return Ref<Resource>(s.get());
}

Ref<Resource> load_wav(const std::string &path) {
    Ref<AudioClip> c = AudioClip::load_wav(path);
    return Ref<Resource>(c.get());
}

Ref<Resource> load_image(const std::string &path) {
    if (!g_device) {
        MF_ERROR("resource: '%s' needs a graphics device, and none is set",
                 path.c_str());
        return {};
    }
    Ref<Texture> t = Texture::load(g_device, path);
    return Ref<Resource>(t.get());
}

// --- the engine's own mesh format
//
// Vertices and indices as they sit in memory, which is the whole
// point: a mesh file that has to be parsed is a mesh file that costs
// a second to open. glTF is the interchange format; this is the one
// a build writes and a game reads.
Ref<Resource> load_mesh(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size_t(std::max(0L, size)));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);

    ByteReader r(bytes);
    if (r.u32() != kMeshMagic || r.u32() != kMeshVersion) {
        MF_ERROR("mesh: '%s' is not a Manifold mesh", path.c_str());
        return {};
    }
    Ref<Mesh> m(new Mesh());
    const uint32_t vcount = r.u32();
    if (!r.ok() || uint64_t(vcount) * sizeof(Vertex) > r.left()) return {};
    m->vertices.resize(vcount);
    if (vcount) r.raw(m->vertices.data(), vcount * sizeof(Vertex));
    const uint32_t icount = r.u32();
    if (!r.ok() || uint64_t(icount) * sizeof(uint32_t) > r.left()) return {};
    m->indices.resize(icount);
    if (icount) r.raw(m->indices.data(), icount * sizeof(uint32_t));
    const uint32_t subs = r.u32();
    if (!r.ok() || subs > r.left()) return {};
    for (uint32_t i = 0; i < subs && r.ok(); i++) {
        SubMesh sm;
        sm.first_index = r.u32();
        sm.index_count = r.u32();
        sm.material_slot = r.i32();
        sm.name = r.str();
        m->submeshes.push_back(sm);
    }
    if (!r.ok()) return {};
    m->compute_bounds();
    return Ref<Resource>(m.get());
}

bool save_mesh(Resource *res, const std::string &path) {
    Mesh *m = res ? res->cast_to<Mesh>() : nullptr;
    if (!m) return false;
    ByteWriter w;
    w.u32(kMeshMagic);
    w.u32(kMeshVersion);
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
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        MF_ERROR("mesh: could not write '%s'", path.c_str());
        return false;
    }
    const size_t n = std::fwrite(w.bytes.data(), 1, w.bytes.size(), f);
    std::fclose(f);
    return n == w.bytes.size();
}

// --- materials, by their reflected properties
//
// Unlike a mesh, a material is a handful of numbers and its shape
// changes as the renderer grows, so it is written through reflection
// and read back by name. A field added tomorrow reads as its default
// out of a file written today, which is the behaviour a project
// wants from its own asset format.
Ref<Resource> load_material(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size_t(std::max(0L, size)));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);

    ByteReader r(bytes);
    if (r.u32() != kMaterialMagic || r.u32() != kMaterialVersion) {
        MF_ERROR("material: '%s' is not a Manifold material", path.c_str());
        return {};
    }
    Ref<Material> m(new Material());
    const uint32_t props = r.u32();
    if (!r.ok() || props > r.left()) return {};
    for (uint32_t i = 0; i < props && r.ok(); i++) {
        const std::string name = r.str();
        const Variant v = r.variant();
        if (m->has_property(name)) m->set(name, v);
    }
    // Textures by path, because a material's textures are resources
    // of their own and nobody wants them inlined either.
    const uint32_t textures = r.u32();
    for (uint32_t i = 0; i < textures && r.ok(); i++) {
        const std::string slot = r.str();
        const std::string tex_path = r.str();
        if (tex_path.empty()) continue;
        Ref<Resource> t = ResourceLoader::load(tex_path);
        Texture *tex = t ? t->cast_to<Texture>() : nullptr;
        if (!tex) continue;
        if (slot == "albedo") m->albedo_map = Ref<Texture>(tex);
        else if (slot == "normal") m->normal_map = Ref<Texture>(tex);
        else if (slot == "orm") m->orm_map = Ref<Texture>(tex);
        else if (slot == "emissive") m->emissive_map = Ref<Texture>(tex);
    }
    if (!r.ok()) return {};
    m->touch();
    return Ref<Resource>(m.get());
}

bool save_material(Resource *res, const std::string &path) {
    Material *m = res ? res->cast_to<Material>() : nullptr;
    if (!m) return false;
    ByteWriter w;
    w.u32(kMaterialMagic);
    w.u32(kMaterialVersion);
    std::vector<std::pair<std::string, Variant>> props;
    for (ClassInfo *c = m->get_class_info(); c; c = c->base)
        for (const std::string &name : c->property_order) {
            const PropertyInfo *p = c->find_property(name);
            if (!p || !p->set || p->transient) continue;
            if (p->type == VType::Object) continue;
            if (name == "resource_path" || name == "resource_name") continue;
            props.emplace_back(name, m->get(name));
        }
    w.u32(uint32_t(props.size()));
    for (const auto &kv : props) {
        w.str(kv.first);
        w.variant(kv.second);
    }
    struct Slot { const char *name; Texture *tex; };
    const Slot slots[4] = {{"albedo", m->albedo_map.get()},
                           {"normal", m->normal_map.get()},
                           {"orm", m->orm_map.get()},
                           {"emissive", m->emissive_map.get()}};
    uint32_t count = 0;
    for (const Slot &s : slots)
        if (s.tex && s.tex->has_path()) count++;
    w.u32(count);
    for (const Slot &s : slots) {
        if (!s.tex || !s.tex->has_path()) continue;
        w.str(s.name);
        w.str(s.tex->resource_path());
    }

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        MF_ERROR("material: could not write '%s'", path.c_str());
        return false;
    }
    const size_t n = std::fwrite(w.bytes.data(), 1, w.bytes.size(), f);
    std::fclose(f);
    return n == w.bytes.size();
}

bool save_scene_file(Resource *res, const std::string &path) {
    PackedScene *s = res ? res->cast_to<PackedScene>() : nullptr;
    return s && s->save(path);
}

struct RegisterLoaders {
    RegisterLoaders() {
        ResourceLoader::register_loader({"mfs"}, &load_scene);
        ResourceLoader::register_loader({"wav"}, &load_wav);
        ResourceLoader::register_loader(
            {"png", "jpg", "jpeg", "tga", "bmp", "hdr"}, &load_image);
        ResourceLoader::register_loader({"mesh"}, &load_mesh);
        ResourceLoader::register_loader({"mat"}, &load_material);
        ResourceSaver::register_saver({"mesh"}, &save_mesh);
        ResourceSaver::register_saver({"mat"}, &save_material);
        ResourceSaver::register_saver({"mfs"}, &save_scene_file);
    }
};
RegisterLoaders g_register;

}  // namespace

void resource_set_device(rhi::Device *dev) { g_device = dev; }
rhi::Device *resource_device() { return g_device; }

}  // namespace mf
