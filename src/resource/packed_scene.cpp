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
#include "render/texture.h"
#include "anim/clip.h"
#include "anim/skeleton.h"
#include "render/mesh.h"
#include "scene/nodes.h"
#include "resource/resource.h"
#include "scene/scene_tree.h"

namespace wr {


namespace {
constexpr uint32_t kSceneMagic = 0x4D465343u;  // 'MFSC'
// 2 adds two sections that version 1 had no way to express: the
// skin stream on a mesh, and resource LISTS on a node -- a mesh's
// materials and a player's clips, both of which used to be dropped
// silently on save. The reader refuses an older file rather than
// mis-parsing one, which is what an unversioned format change does.
// 3: resources may hold resources -- a material's textures.
constexpr uint32_t kSceneVersion = 3;

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

// A LIST OF RESOURCES IS A PROPERTY TOO.
//
// A MeshInstance3D has one mesh and a LIST of materials; an
// AnimationPlayer has a list of clips. Neither survived being saved,
// because the format knew about a single object property and nothing
// else -- so a scene with a three-material prop in it came back with
// three default-grey ones and the failure looked like a material bug.
//
// An Array of objects is the general answer, and it costs the format
// one extra section rather than a special case per class. Nulls are
// kept: slot two of a three-slot list being empty is information.
bool is_resource_array(const PropertyInfo &p, const Variant &v) {
    if (p.type != VType::Array) return false;
    const Array *a = v.array_ptr();
    if (!a) return false;
    for (const Variant &e : *a)
        if (e.type() == VType::Object) return true;
    // An empty list is treated as one: it has to be written, or a
    // node whose list was cleared comes back with the default.
    return a->empty();
}

// A RESOURCE CAN HOLD RESOURCES, and until now the table stopped at
// the first level: a MeshInstance3D's material was collected, and
// that material's textures were not. Every imported model came back
// white, and nothing said why -- the material was there, its
// albedo_map was simply gone.
//
// Dependencies go in FIRST, so that when the table is read back in
// order a material's texture already exists by the time the material
// asks for it.
void collect_dependencies(Object *o, ResourceTable *table) {
    if (!o) return;
    for (ClassInfo *c = o->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type != VType::Object) continue;
            Object *dep = o->get(pname).to_object();
            if (!dep || dep->cast_to<Node>()) continue;
            collect_dependencies(dep, table);
            table->add(dep);
        }
}

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
            collect_dependencies(o, table);
            table->add(o);
        }
    // And the ones held in lists. See is_resource_array.
    for (ClassInfo *c = n->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient || p->type != VType::Array) continue;
            const Variant v = n->get(pname);
            if (!is_resource_array(*p, v)) continue;
            if (const Array *a = v.array_ptr())
                for (const Variant &e : *a) {
                    Object *o = e.to_object();
                    if (!o || o->cast_to<Node>()) continue;
                    collect_dependencies(o, table);
                    table->add(o);
                }
        }
    for (const auto &child : n->children())
        if (child) collect_resources(child.get(), table, scene_root);
}

void write_resource(ByteWriter &w, Object *o,
                    const ResourceTable *table) {
    // A RESOURCE THAT HAS A FILE IS WRITTEN AS THE FILE.
    //
    // Inline is the fallback, not the rule. A level of forty rooms
    // all using one crate should be forty references to crate.mesh,
    // not forty crates -- and editing the crate should change all
    // forty, which it cannot do if each room has its own copy. The
    // same argument as scene instancing, one layer down.
    Resource *res = o->cast_to<Resource>();
    if (res && res->has_path()) {
        w.u8(1);
        w.str(res->resource_path());
        return;
    }
    w.u8(0);
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
        // THE SECOND STREAM, which most meshes do not have. Written
        // after the submeshes rather than beside the vertices so that
        // a file produced before skinning existed still reads: the
        // reader treats a missing count as zero.
        w.u32(uint32_t(m->skin.size()));
        if (!m->skin.empty())
            w.raw(m->skin.data(), m->skin.size() * sizeof(SkinVertex));
        return;
    }
    if (AnimationClip *c = o->cast_to<AnimationClip>()) {
        // Keyframes are the same case as geometry: thousands of
        // small records, no business in a property list.
        w.str(c->name);
        w.f32(c->duration);
        w.u8(c->loops ? 1 : 0);
        w.u32(uint32_t(c->tracks.size()));
        for (const AnimationClip::BoneTrack &t : c->tracks) {
            // The bone NAME, not the index it happens to resolve to.
            // A clip is retargeted against whatever skeleton it is
            // played on, and an index is only meaningful against the
            // one it was imported with.
            w.str(t.bone_name);
            w.u32(uint32_t(t.position.times.size()));
            for (size_t i = 0; i < t.position.times.size(); i++) {
                w.f32(t.position.times[i]);
                w.f32(t.position.values[i].x);
                w.f32(t.position.values[i].y);
                w.f32(t.position.values[i].z);
            }
            w.u32(uint32_t(t.rotation.times.size()));
            for (size_t i = 0; i < t.rotation.times.size(); i++) {
                w.f32(t.rotation.times[i]);
                w.f32(t.rotation.values[i].x);
                w.f32(t.rotation.values[i].y);
                w.f32(t.rotation.values[i].z);
                w.f32(t.rotation.values[i].w);
            }
            w.u32(uint32_t(t.scale.times.size()));
            for (size_t i = 0; i < t.scale.times.size(); i++) {
                w.f32(t.scale.times[i]);
                w.f32(t.scale.values[i].x);
                w.f32(t.scale.values[i].y);
                w.f32(t.scale.values[i].z);
            }
        }
        return;
    }
    if (Skeleton *sk = o->cast_to<Skeleton>()) {
        // A skeleton is a flat array of small records and, like
        // geometry, is the wrong shape for a property list.
        w.u32(uint32_t(sk->bones.size()));
        for (const Skeleton::Bone &b : sk->bones) {
            w.str(b.name);
            w.i32(b.parent);
            w.xform(b.rest);
            w.xform(b.inverse_bind);
        }
        return;
    }
    if (Texture *t = o->cast_to<Texture>()) {
        // The bytes it was decoded from. A texture with a file of
        // its own was written by path far above; this is the one
        // embedded in a model, which has nowhere else to live.
        const std::vector<uint8_t> &src = t->source();
        w.u32(uint32_t(src.size()));
        w.u8(t->source_srgb() ? 1 : 0);
        if (!src.empty()) w.raw(src.data(), src.size());
        return;
    }
    // Everything else by its reflected properties, which is how a
    // Material works and how anything added later will.
    //
    // OBJECT PROPERTIES ARE TABLE INDICES. Skipping them, which is
    // what this did, is why a material's textures did not survive
    // being packed. The table holds dependencies before dependents,
    // so the index always names something already read.
    std::vector<std::pair<std::string, Variant>> props;
    std::vector<std::pair<std::string, uint32_t>> refs;
    for (ClassInfo *c = o->get_class_info(); c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient) continue;
            if (p->type == VType::Object) {
                Object *dep = o->get(pname).to_object();
                if (!dep || dep->cast_to<Node>()) continue;
                auto it = table ? table->index.find(dep) : decltype(table->index)::const_iterator();
                if (table && it != table->index.end())
                    refs.emplace_back(pname, it->second);
                continue;
            }
            props.emplace_back(pname, o->get(pname));
        }
    w.u32(uint32_t(props.size()));
    for (const auto &kv : props) {
        w.str(kv.first);
        w.variant(kv.second);
    }
    w.u32(uint32_t(refs.size()));
    for (const auto &kv : refs) {
        w.str(kv.first);
        w.u32(kv.second);
    }
}

// RETURNS A Ref, not a raw pointer. The old contract was
// "a fresh object at refcount zero, which the caller wraps",
// and it cannot express a resource that is BUILT already
// held -- a texture decoded by Texture::from_memory comes
// back in a Ref, and handing back its raw pointer freed it
// the moment this function returned.
Ref<Object> read_resource(ByteReader &r,
                          const std::vector<Ref<Object>> &done) {
    if (r.u8() == 1) {
        const std::string path = r.str();
        if (!r.ok()) return {};
        Ref<Resource> res = ResourceLoader::load(path);
        if (!res) {
            // A MISSING ASSET IS A HOLE, NOT A REFUSAL -- the same
            // rule as a missing scene. A level that opens with one
            // crate missing can be repaired; one that will not open
            // cannot.
            WR_WARN("scene: '%s' could not be loaded", path.c_str());
            return {};
        }
        // The caller wraps this in a Ref, which retains it. The
        // inline path below returns a fresh object at refcount zero
        // and gets the same treatment, so both end up held exactly
        // once -- retaining here as well would leak every shared
        // resource in the level.
        return Ref<Object>(res.get());
    }
    const std::string cls = r.str();
    if (!r.ok()) return {};
    // A TEXTURE IS NOT INSTANTIATED, IT IS DECODED. It has no
    // default constructor in the registry on purpose -- an empty
    // one is not a useful thing -- so this is handled before the
    // generic path rather than by making one and filling it in.
    if (cls == "Texture") {
        const uint32_t n = r.u32();
        const bool srgb = r.u8() != 0;
        if (!r.ok() || uint64_t(n) > r.left()) {
            r.variant();
            return {};
        }
        std::vector<uint8_t> bytes(n);
        if (n) r.raw(bytes.data(), n);
        if (!r.ok()) return {};
        // Rebuilt through the same path that made it: decode, upload,
        // and keep the bytes so it can be written again.
        if (rhi::Device *dev = resource_device()) {
            Ref<Texture> made = Texture::from_memory(dev, bytes.data(),
                                                     bytes.size(), srgb, true,
                                                     "packed");
            if (made) return Ref<Object>(made.get());
        }
        // No device: a headless load, in a test or a tool. There
        // is nothing to upload to and nothing to show, and a null
        // here is a material with no map rather than a failure.
        return {};
    }
    Object *o = ClassDB::instantiate(cls);
    if (Mesh *m = o ? o->cast_to<Mesh>() : nullptr) {
        const uint32_t vcount = r.u32();
        // A count is an allocation request from a file: bounded by
        // what is actually there, not by trust.
        if (!r.ok() || uint64_t(vcount) * sizeof(Vertex) > r.left()) {
            r.variant();  // force the failure flag
            delete o;
            return {};
        }
        m->vertices.resize(vcount);
        if (vcount) r.raw(m->vertices.data(), vcount * sizeof(Vertex));
        const uint32_t icount = r.u32();
        if (!r.ok() || uint64_t(icount) * sizeof(uint32_t) > r.left()) {
            delete o;
            return {};
        }
        m->indices.resize(icount);
        if (icount) r.raw(m->indices.data(), icount * sizeof(uint32_t));
        const uint32_t subs = r.u32();
        if (!r.ok() || subs > r.left()) {
            delete o;
            return {};
        }
        for (uint32_t i = 0; i < subs && r.ok(); i++) {
            SubMesh sm;
            sm.first_index = r.u32();
            sm.index_count = r.u32();
            sm.material_slot = r.i32();
            sm.name = r.str();
            m->submeshes.push_back(sm);
        }
        // The skin stream, if this file has one. A file written
        // before it existed simply ends here, and `left()` says so.
        if (r.ok() && r.left() >= 4) {
            const uint32_t scount = r.u32();
            if (r.ok() && uint64_t(scount) * sizeof(SkinVertex) <= r.left()) {
                m->skin.resize(scount);
                if (scount) r.raw(m->skin.data(), scount * sizeof(SkinVertex));
            }
        }
        m->compute_bounds();
        return Ref<Object>(o);
    }
    if (AnimationClip *c = o->cast_to<AnimationClip>()) {
        c->name = r.str();
        c->duration = r.f32();
        c->loops = r.u8() != 0;
        const uint32_t n = r.u32();
        // Each track is at least a name and three counts.
        if (!r.ok() || uint64_t(n) * 8 > r.left()) {
            delete o;
            return {};
        }
        for (uint32_t i = 0; i < n && r.ok(); i++) {
            AnimationClip::BoneTrack t;
            t.bone_name = r.str();
            const uint32_t np = r.u32();
            if (!r.ok() || uint64_t(np) * 16 > r.left()) break;
            for (uint32_t k = 0; k < np && r.ok(); k++) {
                t.position.times.push_back(r.f32());
                const float x = r.f32(), y = r.f32(), z = r.f32();
                t.position.values.push_back(Vec3(x, y, z));
            }
            const uint32_t nr = r.u32();
            if (!r.ok() || uint64_t(nr) * 20 > r.left()) break;
            for (uint32_t k = 0; k < nr && r.ok(); k++) {
                t.rotation.times.push_back(r.f32());
                const float x = r.f32(), y = r.f32(), z = r.f32(), w2 = r.f32();
                t.rotation.values.push_back(Quat(x, y, z, w2));
            }
            const uint32_t ns = r.u32();
            if (!r.ok() || uint64_t(ns) * 16 > r.left()) break;
            for (uint32_t k = 0; k < ns && r.ok(); k++) {
                t.scale.times.push_back(r.f32());
                const float x = r.f32(), y = r.f32(), z = r.f32();
                t.scale.values.push_back(Vec3(x, y, z));
            }
            c->tracks.push_back(std::move(t));
        }
        return Ref<Object>(o);
    }
    if (Skeleton *sk = o->cast_to<Skeleton>()) {
        const uint32_t n = r.u32();
        // Four fields a bone, so a count that could not possibly fit
        // is a corrupt file rather than a very large rig.
        if (!r.ok() || uint64_t(n) * 8 > r.left()) {
            delete o;
            return {};
        }
        sk->bones.resize(n);
        for (uint32_t i = 0; i < n && r.ok(); i++) {
            sk->bones[i].name = r.str();
            sk->bones[i].parent = r.i32();
            sk->bones[i].rest = r.xform();
            sk->bones[i].inverse_bind = r.xform();
        }
        return Ref<Object>(o);
    }
    const uint32_t props = r.u32();
    if (!r.ok() || props > r.left()) {
        if (o) delete o;
        return {};
    }
    for (uint32_t i = 0; i < props && r.ok(); i++) {
        const std::string pname = r.str();
        const Variant v = r.variant();
        if (o && o->has_property(pname)) o->set(pname, v);
    }
    // The resources this one holds, by table index. Written after
    // the plain properties and read the same way; the table put
    // dependencies first, so every index names something already
    // built.
    const uint32_t refs = r.u32();
    if (!r.ok() || refs > r.left()) {
        if (o) delete o;
        return {};
    }
    for (uint32_t i = 0; i < refs && r.ok(); i++) {
        const std::string pname = r.str();
        const uint32_t at = r.u32();
        if (!o || at >= done.size() || !done[at]) continue;
        if (o->has_property(pname)) o->set(pname, Variant(done[at].get()));
    }
    if (o) {
        // Materials cache GPU state keyed on their fields; loading
        // new values behind that cache would show the old ones.
        if (Material *mat = o->cast_to<Material>()) mat->touch();
    }
    return Ref<Object>(o);
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
            if (is_resource_array(*p, v)) continue;
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
            // A list of resources is written as indices into the
            // table, further down; a Variant holding raw pointers
            // would be meaningless in a file.
            if (is_resource_array(*p, v)) continue;
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

    // Resource LISTS: a name, a count, and one table index per slot
    // (0xffffffff for an empty one).
    std::vector<std::pair<std::string, std::vector<uint32_t>>> lists;
    for (ClassInfo *c = ci; c; c = c->base)
        for (const std::string &pname : c->property_order) {
            const PropertyInfo *p = c->find_property(pname);
            if (!p || !p->set || p->transient || p->type != VType::Array) continue;
            const Variant v = n->get(pname);
            if (!is_resource_array(*p, v)) continue;
            std::vector<uint32_t> idx;
            if (const Array *a = v.array_ptr())
                for (const Variant &e : *a) {
                    Object *o = e.to_object();
                    auto it = o ? table.index.find(o) : table.index.end();
                    idx.push_back(it != table.index.end() ? it->second
                                                          : 0xffffffffu);
                }
            lists.emplace_back(pname, idx);
        }
    w.u32(uint32_t(lists.size()));
    for (const auto &kv : lists) {
        w.str(kv.first);
        w.u32(uint32_t(kv.second.size()));
        for (uint32_t i : kv.second) w.u32(i);
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
        WR_WARN("scene: '%s' is not available; '%s' will be missing",
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
            WR_WARN("scene: '%s' overrides '%s', which %s no longer has",
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
        WR_WARN("scene: no class '%s'; its node '%s' is skipped", cls.c_str(),
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

    // Resource lists. A slot that names nothing comes back as a null
    // entry rather than being dropped, because a three-slot material
    // list with a hole in it is not a two-slot list.
    const uint32_t list_count = r.u32();
    for (uint32_t i = 0; i < list_count && r.ok(); i++) {
        const std::string pname = r.str();
        const uint32_t n_items = r.u32();
        if (!r.ok() || uint64_t(n_items) * 4 > r.left()) break;
        Array items;
        items.reserve(n_items);
        for (uint32_t k = 0; k < n_items && r.ok(); k++) {
            const uint32_t index = r.u32();
            items.push_back(index < resources.size()
                                    ? Variant(resources[index].get())
                                    : Variant());
        }
        if (n && n->has_property(pname)) n->set(pname, Variant(items));
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
    for (Object *o : table.objects) write_resource(w, o, &table);
    w.u32(1);
    write_node(w, root, table, root->path(), root);
    return std::move(w.bytes);
}

Node *deserialise_tree(const uint8_t *data, size_t size) {
    ByteReader r(data, size);
    if (r.u32() != kSceneMagic) {
        WR_ERROR("scene: not a Warren scene file");
        return nullptr;
    }
    const uint32_t version = r.u32();
    if (version != kSceneVersion) {
        WR_ERROR("scene: version %u, this build reads %u", version,
                 kSceneVersion);
        return nullptr;
    }
    const uint32_t resource_count = r.u32();
    if (!r.ok() || resource_count > 1u << 20) {
        WR_ERROR("scene: an implausible resource count");
        return nullptr;
    }
    std::vector<Ref<Object>> resources;
    resources.reserve(resource_count);
    for (uint32_t i = 0; i < resource_count && r.ok(); i++)
        resources.push_back(read_resource(r, resources));
    if (!r.ok()) {
        WR_ERROR("scene: the resource table is truncated");
        return nullptr;
    }
    if (r.u32() == 0) return nullptr;

    std::vector<PendingLink> links;
    Node *n = read_node(r, resources, &links);
    if (!r.ok()) {
        WR_ERROR("scene: the file ended in the middle of a node");
        if (n) n->queue_free();
        return nullptr;
    }
    for (const PendingLink &link : links) {
        if (!link.from) continue;
        Node *target = link.path == "." ? n : n->find_path(link.path);
        if (!target) {
            WR_WARN("scene: '%s' points at '%s', which is not in the file",
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
        WR_ERROR("scene: could not open '%s'", p.c_str());
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
        WR_ERROR("scene: could not write '%s'", p.c_str());
        return false;
    }
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        WR_ERROR("scene: short write to '%s'", p.c_str());
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
WR_REGISTER(register_packed_scene)

}  // namespace wr
