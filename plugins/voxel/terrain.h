// Manifold voxel -- the terrain node.
//
// Streams chunks of a signed distance field around a viewer, meshes
// them on worker threads, gives them colliders, and lets them be dug.
#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/jobs.h"
#include "density.h"
#include "mesher.h"
#include "scene/bodies.h"
#include "scene/nodes.h"

namespace mf::voxel {

struct ChunkCoord {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord &o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord &c) const {
        // Three odd primes: cheap, and good enough that a plane of
        // chunks does not all land in one bucket.
        return size_t(uint32_t(c.x) * 0x9E3779B1u ^ uint32_t(c.y) * 0x85EBCA77u ^
                      uint32_t(c.z) * 0xC2B2AE3Du);
    }
};

class VoxelTerrain3D : public Node3D {
    MF_CLASS(VoxelTerrain3D, Node3D)

public:
    VoxelTerrain3D();
    ~VoxelTerrain3D() override;

    // --- shape of the world ---------------------------------------------
    // Cells along each axis of a chunk. 32 is the sweet spot: enough
    // that a chunk is worth a draw call, few enough that one dug hole
    // does not re-mesh a room.
    int chunk_resolution = 32;
    float cell_size = 1.0f;
    float chunk_size() const { return float(chunk_resolution) * cell_size; }

    // --- streaming --------------------------------------------------------
    float view_distance = 192.0f;
    // How many chunks may be handed to the workers in one frame.
    // Unbounded, a teleport queues the whole world at once and the
    // frame that discovers it takes a second.
    int queue_per_frame = 8;
    // AND HOW MANY MAY BE IN FLIGHT AT ONCE.
    //
    // The per-frame limit alone does not bound anything: if meshing
    // is slower than the budget, the queue grows every frame for
    // ever, memory with it, and work is done in the order it was
    // asked for rather than the order it is needed. Capping what is
    // outstanding means the nearest chunk is always near the front.
    // 0 picks three per worker thread.
    int max_in_flight = 0;
    // How many finished meshes may be uploaded in one frame. Uploads
    // touch the GPU and so must happen on the main thread, which
    // makes this the one part that cannot be spread out.
    int upload_per_frame = 4;
    bool generate_collision = true;
    // Collision only near the viewer; distant chunks are scenery.
    float collision_distance = 64.0f;

    // The thing to stream around. Usually the player; falls back to
    // the active camera.
    Node3D *viewer = nullptr;
    void set_viewer(Node3D *n) { viewer = n; }

    // --- the field -----------------------------------------------------------
    // The terrain owns its density source. `terrain()` is the default
    // one, so its parameters can be tuned without replacing it.
    void set_density(std::unique_ptr<DensitySource> d);
    DensitySource *density() const { return edited_.get(); }
    TerrainDensity *terrain_density() const { return terrain_; }
    void set_seed(int64_t seed);

    void set_material(Material *m);
    Material *material() const { return material_.get(); }

    // --- editing --------------------------------------------------------------
    // Remove or add a sphere of world, and remesh what it touched.
    void dig(const Vec3 &centre, float radius);
    void build(const Vec3 &centre, float radius, int64_t material);
    void paint(const Vec3 &centre, float radius, int64_t material);
    void apply_edit(const Edit &e);
    void clear_edits();

    // --- queries ----------------------------------------------------------------
    // The height of the ground under a point, ignoring caves.
    float height_at(float x, float z) const;
    // Sample the field directly.
    float distance_at(const Vec3 &p) const;
    int64_t material_at(const Vec3 &p) const;

    // --- the loop ------------------------------------------------------------------
    void on_process(float dt) override;
    // Block until everything currently queued is meshed and uploaded.
    // For a loading screen, and for tests.
    void wait_for_chunks();
    // Throw everything away and start again.
    void rebuild();

    struct Stats {
        uint32_t chunks_live = 0;
        uint32_t chunks_meshing = 0;
        uint32_t chunks_queued = 0;
        uint32_t chunks_empty = 0;
        uint32_t triangles = 0;
        uint64_t chunks_meshed_total = 0;
        double last_mesh_ms = 0.0;
    };
    const Stats &stats() const { return stats_; }
    std::string report() const;

private:
    enum class State : uint8_t { Queued, Meshing, Ready, Empty };

    struct Chunk {
        ChunkCoord coord;
        std::atomic<State> state{State::Queued};
        // Written by a worker, read by the main thread once the state
        // says Ready. The state is the handshake.
        MeshResult mesh;
        MeshInstance3D *instance = nullptr;
        StaticBody3D *body = nullptr;
        Ref<Mesh> gpu_mesh;
        float distance = 0.0f;
        bool has_collision = false;
    };

    AABB chunk_bounds(const ChunkCoord &c) const;
    Vec3 chunk_origin(const ChunkCoord &c) const;
    ChunkCoord coord_of(const Vec3 &p) const;
    void queue_chunk(const ChunkCoord &c);
    void upload_chunk(Chunk &chunk);
    void drop_chunk(Chunk &chunk);
    void remesh_region(const AABB &box);
    Vec3 viewer_position() const;

    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> chunks_;
    std::unique_ptr<EditedDensity> edited_;
    std::unique_ptr<DensitySource> base_density_;
    TerrainDensity *terrain_ = nullptr;
    Ref<Material> material_;
    JobCounterRef counter_;
    Stats stats_;
    std::vector<ChunkCoord> scratch_;
    bool dirty_ = true;
};

}  // namespace mf::voxel
