#include "net/replication.h"

#include <algorithm>
#include <cstdio>

#include "core/bind.h"
#include "core/log.h"
#include "core/serialize.h"
#include "scene/scene_tree.h"

namespace wr {
namespace {
constexpr uint32_t kSnapshotMagic = 0x534E4150u;  // 'SNAP'
constexpr uint32_t kSpawnMagic = 0x53504E57u;     // 'SPNW'
}  // namespace

void Replicator::init(SceneTree *tree, Role role) {
    tree_ = tree;
    role_ = role;
    by_id_.clear();
    next_id_ = 1;
    last_tick_ = 0;
}

void Replicator::collect() {
    scratch_.clear();
    if (!tree_ || !tree_->root()) return;
    std::vector<Node *> stack{tree_->root()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        for (const auto &c : n->children())
            if (c) stack.push_back(c.get());
        if (NetSync *s = n->cast_to<NetSync>())
            if (s->target()) scratch_.push_back(s);
    }
}

Replicator::Tracked *Replicator::find(uint32_t id) {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------- the server

std::vector<uint8_t> Replicator::build_snapshot(uint32_t tick) {
    ByteWriter w;
    w.u32(kSnapshotMagic);
    w.u32(tick);
    collect();

    // Count first, so the reader can size its loop without trusting
    // a terminator it might never find.
    uint32_t count = 0;
    for (NetSync *s : scratch_)
        if (s->target()) count++;
    w.u32(count);

    for (NetSync *s : scratch_) {
        Node *target = s->target();
        if (!target) continue;
        if (!s->net_id) s->net_id = next_id_++;
        w.u32(s->net_id);
        w.u32(uint32_t(s->properties.size()));
        for (const std::string &name : s->properties) {
            w.str(name);
            // A property the class does not have writes as nil
            // rather than being skipped: the client is reading a
            // fixed shape, and a hole in it would desynchronise
            // every field after it.
            w.variant(target->has_property(name) ? target->get(name)
                                                 : Variant());
        }
        by_id_[s->net_id].sync = s;
    }
    last_tick_ = tick;
    return std::move(w.bytes);
}

std::vector<uint8_t> Replicator::build_spawns() {
    ByteWriter w;
    w.u32(kSpawnMagic);
    collect();
    uint32_t count = 0;
    for (NetSync *s : scratch_)
        if (!s->spawn_class.empty()) count++;
    w.u32(count);
    for (NetSync *s : scratch_) {
        if (s->spawn_class.empty()) continue;
        if (!s->net_id) s->net_id = next_id_++;
        w.u32(s->net_id);
        w.str(s->spawn_class);
        w.u32(uint32_t(s->properties.size()));
        for (const std::string &name : s->properties) w.str(name);
    }
    return std::move(w.bytes);
}

// ---------------------------------------------------------- the client

void Replicator::apply_spawns(const uint8_t *data, size_t size) {
    ByteReader r(data, size);
    if (r.u32() != kSpawnMagic) return;
    const uint32_t count = r.u32();
    if (!r.ok() || count > 65536) return;
    for (uint32_t i = 0; i < count && r.ok(); i++) {
        const uint32_t id = r.u32();
        const std::string cls = r.str();
        const uint32_t props = r.u32();
        std::vector<std::string> names;
        for (uint32_t p = 0; p < props && r.ok(); p++) names.push_back(r.str());
        if (!r.ok()) break;
        if (find(id)) continue;   // already have it

        Node *node = nullptr;
        if (on_spawn) {
            node = on_spawn(cls, id);
        } else if (tree_ && tree_->root()) {
            Object *o = ClassDB::instantiate(cls);
            node = o ? o->cast_to<Node>() : nullptr;
            if (node) {
                char name[48];
                std::snprintf(name, sizeof(name), "%s_%u", cls.c_str(), id);
                node->set_name(name);
                tree_->root()->add_child(node);
            } else if (o) {
                // Instantiated but not a Node: nothing sensible to
                // do with it, and leaking it would be worse.
                delete o;
            }
        }
        if (!node) {
            WR_WARN("net: cannot spawn '%s' for id %u", cls.c_str(), id);
            continue;
        }
        NetSync *s = new NetSync();
        s->set_name("NetSync");
        s->net_id = id;
        s->properties = names;
        s->spawn_class = cls;
        node->add_child(s);
        by_id_[id].sync = s;
    }
}

void Replicator::apply_snapshot(const uint8_t *data, size_t size) {
    ByteReader r(data, size);
    if (r.u32() != kSnapshotMagic) return;
    const uint32_t tick = r.u32();
    const uint32_t count = r.u32();
    if (!r.ok() || count > 65536) return;

    // The interval between snapshots is how long the client has to
    // cover the distance between them. Measured rather than
    // configured, so a server that changes its tick rate mid-game --
    // or slows down under load -- does not make every client stutter.
    if (last_snapshot_time_ > 0.0) {
        const float interval = float(clock_ - last_snapshot_time_);
        if (interval > 1e-4f && interval < 1.0f)
            blend_time_ = blend_time_ * 0.8f + interval * 0.2f;
    }
    last_snapshot_time_ = clock_;
    last_tick_ = tick;

    for (uint32_t i = 0; i < count && r.ok(); i++) {
        const uint32_t id = r.u32();
        const uint32_t props = r.u32();
        Tracked *t = find(id);
        std::vector<Variant> values;
        std::vector<std::string> names;
        for (uint32_t p = 0; p < props && r.ok(); p++) {
            names.push_back(r.str());
            values.push_back(r.variant());
        }
        if (!r.ok() || !t || !t->sync || !t->sync->target()) continue;

        Node *node = t->sync->target();
        if (!t->sync->interpolate) {
            for (size_t k = 0; k < names.size(); k++)
                if (node->has_property(names[k])) node->set(names[k], values[k]);
            continue;
        }
        // Where it was becomes where it is now, so the blend starts
        // from what the player is actually looking at rather than
        // from the previous snapshot -- which would snap backwards
        // whenever one is late.
        t->previous.resize(names.size());
        for (size_t k = 0; k < names.size(); k++)
            t->previous[k] = node->has_property(names[k]) ? node->get(names[k])
                                                          : values[k];
        t->target = values;
        t->sync->properties = names;
        t->blend = 0.0f;
    }
}

void Replicator::interpolate(float dt) {
    clock_ += double(dt);
    if (role_ != Role::Client) return;
    const float rate = blend_time_ > 1e-4f ? dt / blend_time_ : 1.0f;
    for (auto &kv : by_id_) {
        Tracked &t = kv.second;
        if (!t.sync || !t.sync->target() || t.blend >= 1.0f) continue;
        t.blend = std::min(1.0f, t.blend + rate);
        Node *node = t.sync->target();
        for (size_t k = 0;
             k < t.target.size() && k < t.sync->properties.size(); k++) {
            const std::string &name = t.sync->properties[k];
            if (!node->has_property(name)) continue;
            const Variant &a = k < t.previous.size() ? t.previous[k] : t.target[k];
            node->set(name, Variant::lerp(a, t.target[k], t.blend));
        }
    }
}

std::string Replicator::report() const {
    char b[160];
    std::snprintf(b, sizeof(b), "replication: %s, %zu nodes, tick %u",
                  role_ == Role::Server ? "server" : "client", by_id_.size(),
                  last_tick_);
    return b;
}

static void register_net_classes() {
    ClassBuilder<NetSync>()
        .field("net_id", &NetSync::net_id)
        .field("spawn_class", &NetSync::spawn_class)
        .field("interpolate", &NetSync::interpolate)
        .method("add_property", &NetSync::add_property).args("name");
}
WR_REGISTER(register_net_classes)

}  // namespace wr
