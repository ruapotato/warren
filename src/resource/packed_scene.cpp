#include "resource/packed_scene.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/bind.h"
#include "core/log.h"
#include "core/serialize.h"
#include "render/material.h"
#include "render/mesh.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

namespace mf {


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

void collect_resources(Node *n, ResourceTable *table, Node *scene_root) {
    if (!n) return;
    // AND THE RESOURCE TABLE STOPS THERE TOO. The eight doors in a
    // corridor each hold their own copy of the door's mesh, and
    // collecting all eight put twelve kilobytes of geometry into a
    // file whose whole purpose was to say "eight doors" -- which is
    // the same mistake as copying the nodes, one level down.
    if (!n->scene_path().empty() && n != scene_root) {
        // Except for anything hung on the instance by hand, which
        // is the outer scene's and does need its resources.
        for (const auto &child : n->children())
            if (child && child->owner() != n)
                collect_resources(child.get(), table, scene_root);
        return;
    }
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
        if (child) collect_resources(child.get(), table, scene_root);
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

// Which of a node's properties differ from a reference object.
// Used both for "differs from a fresh instance of this class" when
// writing a plain node, and for "differs from what the scene file
// says" when writing an override on an instanced one.
std::vector<std::pair<std::string, Variant>> changed_properties(
    Node *n, Object *reference) {
    std::vector<std::pair<std::string, Variant>> changed;
    for (ClassInfo *c = n->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type == VType::Object) continue;
            const Variant v = n->get(pname);
            if (reference && reference->has_property(pname) &&
                v == reference->get(pname))
                continue;
            changed.emplace_back(pname, v);
        }
    return changed;
}

void write_node(ByteWriter &w, Node *n, const ResourceTable &table,
                const std::string &root_path, Node *scene_root);

// AN INSTANCED SUBTREE, AS A REFERENCE AND A LIST OF CHANGES.
//
// This is the whole point of the scene system. The door is written
// as "door.mfs, and this one is at (4, 0, 9) and its handle is
// blue" -- two lines -- rather than as a copy of the door. Edit
// door.mfs and every placement changes; move one door and only that
// line changes.
//
// The overrides are found by instancing the referenced scene and
// comparing, which costs a load per instance when saving and is the
// only way to know what was actually changed. A cache of one
// instance per path makes it a load per distinct scene instead.
void write_instance(ByteWriter &w, Node *n, const std::string &root_path) {
    w.str(n->scene_path());
    w.str(n->name());

    static std::unordered_map<std::string, Ref<Node>> pristine;
    auto it = pristine.find(n->scene_path());
    if (it == pristine.end()) {
        Ref<PackedScene> scene = PackedScene::resolve(n->scene_path());
        Ref<Node> fresh(scene && scene->valid() ? scene->instantiate()
                                                : nullptr);
        it = pristine.emplace(n->scene_path(), fresh).first;
    }
    Node *reference = it->second.get();

    // Every node inside the instance, by its path within it, with
    // whatever differs from the pristine copy. A node the user added
    // to the instance by hand has no counterpart and is written in
    // full -- see write_node's owner test.
    struct Override {
        std::string path;
        std::vector<std::pair<std::string, Variant>> values;
    };
    std::vector<Override> overrides;
    std::vector<std::pair<Node *, Node *>> stack{{n, reference}};
    while (!stack.empty()) {
        auto [live, base] = stack.back();
        stack.pop_back();
        std::vector<std::pair<std::string, Variant>> diff =
            changed_properties(live, base);
        if (!diff.empty()) {
            Override o;
            o.path = live == n ? "." : relative_path(live, n->path());
            o.values = std::move(diff);
            overrides.push_back(std::move(o));
        }
        for (const auto &c : live->children()) {
            if (!c) continue;
            // Only nodes that came from the scene: one added
            // afterwards is the outer scene's and is written in
            // full by write_node.
            if (c->owner() != n) continue;
            // AGAINST THE PRISTINE ROOT, not against this node's
            // own counterpart. The path is relative to the instance
            // root -- "Frame/Handle" -- so resolving it against the
            // pristine Frame looks for a Frame inside a Frame and
            // finds nothing. Every property then reads as changed,
            // an override is written for all of them, and editing
            // the door stops changing any door that uses it: the
            // whole feature, quietly undone by one wrong receiver.
            Node *counterpart =
                reference
                    ? reference->find_path(relative_path(c.get(), n->path()))
                    : nullptr;
            stack.emplace_back(c.get(), counterpart);
        }
    }

    w.u32(uint32_t(overrides.size()));
    for (const Override &o : overrides) {
        w.str(o.path);
        w.u32(uint32_t(o.values.size()));
        for (const auto &kv : o.values) {
            w.str(kv.first);
            w.variant(kv.second);
        }
    }

    // And anything hung on the instance after the fact.
    std::vector<Node *> extra;
    for (const auto &c : n->children())
        if (c && c->owner() != n) extra.push_back(c.get());
    w.u32(uint32_t(extra.size()));
    ResourceTable empty;
    for (Node *e : extra) write_node(w, e, empty, root_path, nullptr);
}

void write_node(ByteWriter &w, Node *n, const ResourceTable &table,
                const std::string &root_path, Node *scene_root) {
    // THE FORK. A node that came from another scene file is written
    // as a reference to it; anything else is written out in full.
    // The root of what is being saved is always written in full,
    // even if it was itself instanced -- otherwise saving a scene
    // under a new name would produce a file that just points at the
    // old one.
    const bool is_instance = !n->scene_path().empty() && n != scene_root;
    w.u8(is_instance ? 1 : 0);
    if (is_instance) {
        write_instance(w, n, root_path);
        return;
    }

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
        if (c) write_node(w, c.get(), table, root_path, scene_root);
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
                std::vector<PendingLink> *links);

// The other side of write_instance: load the referenced scene, make
// one, then apply what the file says was changed about it.
Node *read_instance(ByteReader &r,
                    const std::vector<Ref<Object>> &resources,
                    std::vector<PendingLink> *links) {
    const std::string path = r.str();
    const std::string name = r.str();
    if (!r.ok()) return nullptr;

    Ref<PackedScene> scene = PackedScene::resolve(path);
    Node *n = scene && scene->valid() ? scene->instantiate() : nullptr;
    if (!n) {
        // A MISSING SCENE IS NOT A CORRUPT FILE. The rest of the
        // level should still open, with a hole where the door was,
        // because that is repairable and a refusal to load is not.
        MF_WARN("scene: '%s' is not available; '%s' will be missing",
                path.c_str(), name.c_str());
    } else {
        n->set_name(name);
    }

    const uint32_t override_count = r.u32();
    if (!r.ok() || override_count > 1u << 20) return n;
    for (uint32_t i = 0; i < override_count && r.ok(); i++) {
        const std::string node_path = r.str();
        const uint32_t props = r.u32();
        Node *target = nullptr;
        if (n) target = node_path == "." ? n : n->find_path(node_path);
        for (uint32_t p = 0; p < props && r.ok(); p++) {
            const std::string pname = r.str();
            const Variant v = r.variant();
            if (target && target->has_property(pname)) target->set(pname, v);
        }
        if (n && !target)
            MF_WARN("scene: '%s' overrides '%s', which %s no longer has",
                    name.c_str(), node_path.c_str(), path.c_str());
    }

    const uint32_t extra = r.u32();
    if (!r.ok() || extra > 1u << 20) return n;
    for (uint32_t i = 0; i < extra && r.ok(); i++) {
        Node *child = read_node(r, resources, links);
        if (!child) continue;
        if (n)
            n->add_child(child);
        else
            child->queue_free();
    }
    return n;
}

Node *read_node(ByteReader &r, const std::vector<Ref<Object>> &resources,
                std::vector<PendingLink> *links) {
    if (r.u8() == 1) return read_instance(r, resources, links);
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


// ------------------------------------------------------------- the API

std::vector<uint8_t> serialise_tree(Node *root) {
    ByteWriter w;
    w.u32(kSceneMagic);
    w.u32(kSceneVersion);
    if (!root) {
        w.u32(0);
        w.u32(0);
        return std::move(w.bytes);
    }
    ResourceTable table;
    collect_resources(root, &table, root);
    w.u32(uint32_t(table.objects.size()));
    for (Object *o : table.objects) write_resource(w, o);
    w.u32(1);
    write_node(w, root, table, root->path(), root);
    return std::move(w.bytes);
}

Node *deserialise_tree(const uint8_t *data, size_t size) {
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
    for (const PendingLink &link : links) {
        if (!link.from) continue;
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

// -------------------------------------------------------- PackedScene

namespace {
PackedScene::Resolver g_resolver = nullptr;
}

void PackedScene::set_resolver(Resolver r) { g_resolver = r; }

Ref<PackedScene> PackedScene::resolve(const std::string &path) {
    if (g_resolver) return g_resolver(path);
    return load(path);
}

bool PackedScene::pack(Node *root) {
    if (!root) return false;
    bytes = serialise_tree(root);
    return !bytes.empty();
}

Node *PackedScene::instantiate() const {
    if (bytes.empty()) return nullptr;
    Node *n = deserialise_tree(bytes.data(), bytes.size());
    if (!n) return nullptr;
    // EVERYTHING INSIDE BELONGS TO THIS INSTANCE. That is what makes
    // a tree containing it write one line instead of a copy.
    n->set_owner_recursive(n);
    n->set_owner(nullptr);   // the root of an instance owns itself
    n->set_scene_path(path);
    return n;
}

Ref<PackedScene> PackedScene::load(const std::string &p) {
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) {
        MF_ERROR("scene: could not open '%s'", p.c_str());
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    Ref<PackedScene> scene(new PackedScene());
    scene->bytes.resize(size_t(std::max(0L, size)));
    const size_t got = std::fread(scene->bytes.data(), 1, scene->bytes.size(), f);
    std::fclose(f);
    scene->bytes.resize(got);
    scene->path = p;
    return scene;
}

bool PackedScene::save(const std::string &p) {
    FILE *f = std::fopen(p.c_str(), "wb");
    if (!f) {
        MF_ERROR("scene: could not write '%s'", p.c_str());
        return false;
    }
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        MF_ERROR("scene: short write to '%s'", p.c_str());
        return false;
    }
    path = p;
    return true;
}

static void register_packed_scene() {
    ClassBuilder<PackedScene>()
        .prop("path", &PackedScene::path_get, &PackedScene::path_set)
        .method("instantiate", &PackedScene::instantiate)
        .method("is_valid", &PackedScene::valid);
}
MF_REGISTER(register_packed_scene)

}  // namespace mf
