// Manifold -- keeping two machines' worlds the same.
//
// REPLICATION IS A LIST OF PROPERTY NAMES, and that is only possible
// because the engine already knows what its classes have. A node says
// which of its properties matter over the wire; the server reads them
// through ClassDB, writes them as Variants, and the client sets them
// back the same way. Nothing here knows what a Vec3 is or that
// Node3D has a position.
//
// That is the same argument as the Python bindings and the property
// inspector: an engine that declares its classes once can generate
// everything that has to walk them, and the alternative -- a
// hand-written serialiser per class -- is the thing that silently
// falls behind the class it serialises.
//
// THE MODEL IS SERVER-AUTHORITATIVE SNAPSHOTS. The server owns the
// world, sends what changed on an unreliable-sequenced channel, and
// the client interpolates between the last two it received. Spawns
// and despawns go reliably, because missing one leaves a client with
// a node the server does not have or without one it does.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/object.h"
#include "net/connection.h"
#include "scene/node.h"

namespace mf {

class SceneTree;

// Attached as a child of the node it replicates. A child rather than
// a base class so that anything -- an engine class, a plugin's class,
// a Python subclass -- can be replicated without changing what it
// derives from.
class NetSync : public Node {
    MF_CLASS(NetSync, Node)

public:
    // Assigned by the server and quoted in every snapshot. Zero
    // means "not registered yet".
    uint32_t net_id = 0;
    // Which of the parent's properties go over the wire, by name.
    std::vector<std::string> properties = {"position", "rotation"};
    // What to spawn on a client that has not seen this id before.
    // Empty means the client is expected to have it already, which
    // is the case for level geometry.
    std::string spawn_class;
    // Smooth between the last two snapshots rather than snapping.
    // Off for anything the client owns and predicts itself.
    bool interpolate = true;

    void add_property(const std::string &name) { properties.push_back(name); }
    Node *target() const { return parent(); }
};

// One end of a replicated world.
class Replicator {
public:
    enum class Role { Server, Client };

    void init(SceneTree *tree, Role role);
    Role role() const { return role_; }

    // --- server -------------------------------------------------------
    // Assigns ids to any NetSync that has not got one, and writes a
    // snapshot of everything. Returns the bytes to send on the
    // Sequenced channel.
    std::vector<uint8_t> build_snapshot(uint32_t tick);
    // Everything a client joining now needs: the spawn list.
    std::vector<uint8_t> build_spawns();

    // --- client -------------------------------------------------------
    void apply_snapshot(const uint8_t *data, size_t size);
    void apply_spawns(const uint8_t *data, size_t size);
    // Advance the interpolation. `dt` is real time; the snapshots
    // carry the server's tick, so this does not need to know the
    // server's rate.
    void interpolate(float dt);

    // What a client does when told to spawn a class it must create.
    // Defaults to ClassDB::instantiate under the tree's root.
    std::function<Node *(const std::string &, uint32_t)> on_spawn;
    std::function<void(Node *, uint32_t)> on_despawn;

    uint32_t last_tick() const { return last_tick_; }
    size_t tracked() const { return by_id_.size(); }
    std::string report() const;

private:
    struct Tracked {
        NetSync *sync = nullptr;
        // The two most recent snapshots of each property, so the
        // client can sit between them.
        std::vector<Variant> previous;
        std::vector<Variant> target;
        float blend = 1.0f;
    };

    void collect();
    Tracked *find(uint32_t id);

    SceneTree *tree_ = nullptr;
    Role role_ = Role::Server;
    uint32_t next_id_ = 1;
    uint32_t last_tick_ = 0;
    std::unordered_map<uint32_t, Tracked> by_id_;
    std::vector<NetSync *> scratch_;
    // Seconds a client takes to reach the newest snapshot. Set from
    // the observed interval between snapshots, so a server running
    // at 20 Hz and one at 60 both look smooth.
    float blend_time_ = 0.05f;
    double last_snapshot_time_ = 0.0;
    double clock_ = 0.0;
};

}  // namespace mf
