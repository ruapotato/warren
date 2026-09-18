#include "terrain.h"

#include <algorithm>
#include <cstdio>

#include "app/engine.h"
#include "core/log.h"
#include "physics/world.h"
#include "scene/scene_tree.h"

namespace mf::voxel {

namespace {
// The physics world the terrain registers its colliders with. Set by
// the plugin at start-up: a node cannot reach the engine's services
// through the tree, and threading one through every constructor is
// worse than one pointer set once.
PhysicsWorld *g_physics = nullptr;
rhi::Device *g_device = nullptr;
}  // namespace

void voxel_set_services(PhysicsWorld *physics, rhi::Device *device) {
    g_physics = physics;
    g_device = device;
}

VoxelTerrain3D::VoxelTerrain3D() {
    set_name("VoxelTerrain3D");
    auto terrain = std::make_unique<TerrainDensity>();
    terrain_ = terrain.get();
    base_density_ = std::move(terrain);
    edited_ = std::make_unique<EditedDensity>(base_density_.get());
    counter_ = Jobs::make_counter();

    material_ = Material::make(Color::white(), 0.92f, 0.0f);
    // The mesher writes the material colour into the vertex colour
    // and the roughness into its alpha, so one material draws the
    // whole world and a chunk needs no per-material split.
    material_->albedo = Color::white();
}

VoxelTerrain3D::~VoxelTerrain3D() {
    // A worker may still be writing into a chunk's mesh. Waiting is
    // the only correct thing to do; detaching would free the memory
    // underneath it.
    Jobs::wait(counter_);
    for (auto &kv : chunks_) drop_chunk(*kv.second);
    chunks_.clear();
}

void VoxelTerrain3D::set_density(std::unique_ptr<DensitySource> d) {
    Jobs::wait(counter_);
    base_density_ = std::move(d);
    terrain_ = dynamic_cast<TerrainDensity *>(base_density_.get());
    edited_ = std::make_unique<EditedDensity>(base_density_.get());
    rebuild();
}

void VoxelTerrain3D::set_seed(int64_t seed) {
    if (!terrain_) return;
    terrain_->params().seed = uint32_t(seed);
    rebuild();
}

void VoxelTerrain3D::set_material(Material *m) {
    material_ = Ref<Material>(m);
    for (auto &kv : chunks_)
        if (kv.second->instance) kv.second->instance->set_material(0, m);
}

// ------------------------------------------------------------- geometry

Vec3 VoxelTerrain3D::chunk_origin(const ChunkCoord &c) const {
    const float s = chunk_size();
    return global_position() + Vec3(float(c.x), float(c.y), float(c.z)) * s;
}

AABB VoxelTerrain3D::chunk_bounds(const ChunkCoord &c) const {
    Vec3 o = chunk_origin(c);
    return {o, o + Vec3(chunk_size())};
}

ChunkCoord VoxelTerrain3D::coord_of(const Vec3 &p) const {
    const float s = chunk_size();
    Vec3 local = p - global_position();
    return {int32_t(std::floor(local.x / s)), int32_t(std::floor(local.y / s)),
            int32_t(std::floor(local.z / s))};
}

Vec3 VoxelTerrain3D::viewer_position() const {
    if (viewer) return viewer->global_position();
    if (tree())
        if (Camera3D *c = tree()->active_camera()) return c->global_position();
    return global_position();
}

// -------------------------------------------------------------- meshing

void VoxelTerrain3D::queue_chunk(const ChunkCoord &c) {
    auto chunk = std::make_unique<Chunk>();
    chunk->coord = c;
    chunk->state.store(State::Queued, std::memory_order_relaxed);
    Chunk *raw = chunk.get();
    chunks_[c] = std::move(chunk);

    MeshRequest req;
    req.density = edited_.get();
    req.origin = chunk_origin(c);
    req.cell_size = cell_size;
    req.resolution = chunk_resolution;

    raw->state.store(State::Meshing, std::memory_order_release);
    Jobs::submit(
        [raw, req]() {
            mesh_chunk(req, &raw->mesh);
            // RELEASE, so the main thread that reads Ready also sees
            // every byte the mesh was written with. Without it the
            // vertex data is a data race that happens to work on x86
            // and does not on anything else.
            raw->state.store(raw->mesh.empty ? State::Empty : State::Ready,
                             std::memory_order_release);
        },
        counter_);
    stats_.chunks_meshed_total++;
}

void VoxelTerrain3D::upload_chunk(Chunk &chunk) {
    if (chunk.mesh.vertices.empty() || chunk.mesh.indices.empty()) return;

    Ref<Mesh> mesh(new Mesh());
    mesh->vertices = std::move(chunk.mesh.vertices);
    mesh->indices = std::move(chunk.mesh.indices);
    // Dual contouring gives no tangents, and a normal-mapped terrain
    // material needs them.
    mesh->compute_tangents();
    mesh->compute_bounds();

    MeshInstance3D *mi = new MeshInstance3D();
    char name[48];
    std::snprintf(name, sizeof(name), "chunk_%d_%d_%d", chunk.coord.x,
                  chunk.coord.y, chunk.coord.z);
    mi->set_name(name);
    mi->mesh = mesh;
    mi->set_material(0, material_.get());
    add_child(mi);
    chunk.instance = mi;
    chunk.gpu_mesh = mesh;

    if (generate_collision && g_physics && chunk.distance < collision_distance) {
        StaticBody3D *body = new StaticBody3D();
        body->set_name("collider");
        mi->add_child(body);
        body->build_from_mesh(g_physics, mesh.get(), 1);
        chunk.body = body;
        chunk.has_collision = true;
    }
}

void VoxelTerrain3D::drop_chunk(Chunk &chunk) {
    if (chunk.body) {
        chunk.body->release();
        chunk.body = nullptr;
    }
    if (chunk.instance) {
        chunk.instance->free_from_parent();
        chunk.instance = nullptr;
    }
    chunk.gpu_mesh.reset();
    chunk.has_collision = false;
}

// ------------------------------------------------------------ the stream

void VoxelTerrain3D::on_process(float dt) {
    Node3D::on_process(dt);
    if (!edited_) return;

    const Vec3 eye = viewer_position();
    const float s = chunk_size();
    const int radius = std::max(1, int(std::ceil(view_distance / s)));
    const ChunkCoord centre = coord_of(eye);

    // --- collect what should exist, nearest first
    scratch_.clear();
    for (int z = -radius; z <= radius; z++)
        for (int y = -radius; y <= radius; y++)
            for (int x = -radius; x <= radius; x++) {
                ChunkCoord c{centre.x + x, centre.y + y, centre.z + z};
                AABB box = chunk_bounds(c);
                float d = (box.center() - eye).length();
                if (d > view_distance + s) continue;
                // THE FIELD'S OWN BOUND, WHICH IS WHAT MAKES THIS
                // AFFORDABLE. Most of a world is solid rock or open
                // sky, and a source that can say so for a whole box
                // saves meshing thirty-odd thousand samples to
                // discover it.
                if (edited_->bound(box) > s * 0.87f) continue;
                scratch_.push_back(c);
            }
    std::sort(scratch_.begin(), scratch_.end(),
              [&](const ChunkCoord &a, const ChunkCoord &b) {
                  return (chunk_bounds(a).center() - eye).length_sq() <
                         (chunk_bounds(b).center() - eye).length_sq();
              });

    // --- queue what is missing, up to the budget and the cap
    int in_flight = 0;
    for (auto &kv : chunks_) {
        const State st = kv.second->state.load(std::memory_order_relaxed);
        if (st == State::Queued || st == State::Meshing) in_flight++;
    }
    const int cap = max_in_flight > 0
                        ? max_in_flight
                        : std::max(4, Jobs::worker_count() * 3);
    int queued = 0;
    for (const ChunkCoord &c : scratch_) {
        if (queued >= queue_per_frame || in_flight >= cap) break;
        if (chunks_.count(c)) continue;
        queue_chunk(c);
        queued++;
        in_flight++;
    }

    // --- take delivery of what the workers finished
    int uploaded = 0;
    // The running total survives the per-frame reset; everything
    // else is recounted.
    const uint64_t meshed_total = stats_.chunks_meshed_total;
    stats_ = Stats();
    stats_.chunks_meshed_total = meshed_total;
    std::vector<ChunkCoord> expired;
    for (auto &kv : chunks_) {
        Chunk &chunk = *kv.second;
        chunk.distance = (chunk_bounds(chunk.coord).center() - eye).length();
        const State state = chunk.state.load(std::memory_order_acquire);
        switch (state) {
            case State::Queued:
                stats_.chunks_queued++;
                break;
            case State::Meshing:
                stats_.chunks_meshing++;
                break;
            case State::Empty:
                stats_.chunks_empty++;
                break;
            case State::Ready:
                if (!chunk.instance && uploaded < upload_per_frame) {
                    upload_chunk(chunk);
                    uploaded++;
                }
                break;
        }
        if (chunk.instance) {
            stats_.chunks_live++;
            if (chunk.gpu_mesh) stats_.triangles += chunk.gpu_mesh->triangle_count();
            // Collision comes and goes with distance, so a world can
            // be large without every chunk paying for a BVH.
            if (generate_collision && g_physics) {
                const bool want = chunk.distance < collision_distance;
                if (want && !chunk.has_collision && chunk.gpu_mesh) {
                    StaticBody3D *body = new StaticBody3D();
                    body->set_name("collider");
                    chunk.instance->add_child(body);
                    body->build_from_mesh(g_physics, chunk.gpu_mesh.get(), 1);
                    chunk.body = body;
                    chunk.has_collision = true;
                } else if (!want && chunk.has_collision) {
                    if (chunk.body) chunk.body->release();
                    chunk.body = nullptr;
                    chunk.has_collision = false;
                }
            }
        }
        if (chunk.distance > view_distance + s * 2.0f &&
            state != State::Meshing && state != State::Queued)
            expired.push_back(chunk.coord);
    }

    for (const ChunkCoord &c : expired) {
        auto it = chunks_.find(c);
        if (it == chunks_.end()) continue;
        drop_chunk(*it->second);
        chunks_.erase(it);
    }
}

void VoxelTerrain3D::wait_for_chunks() {
    Jobs::wait(counter_);
    for (auto &kv : chunks_) {
        Chunk &chunk = *kv.second;
        if (chunk.state.load(std::memory_order_acquire) == State::Ready &&
            !chunk.instance)
            upload_chunk(chunk);
    }
}

void VoxelTerrain3D::rebuild() {
    Jobs::wait(counter_);
    for (auto &kv : chunks_) drop_chunk(*kv.second);
    chunks_.clear();
}

// ---------------------------------------------------------------- edits

void VoxelTerrain3D::remesh_region(const AABB &box) {
    // Everything the edit touched, plus a cell of slack: a chunk one
    // cell away still samples into the edited region through its
    // border.
    AABB grown = box.grown(cell_size * 2.0f);
    std::vector<ChunkCoord> hit;
    for (auto &kv : chunks_) {
        if (!chunk_bounds(kv.first).intersects(grown)) continue;
        hit.push_back(kv.first);
    }
    Jobs::wait(counter_);
    for (const ChunkCoord &c : hit) {
        auto it = chunks_.find(c);
        if (it == chunks_.end()) continue;
        drop_chunk(*it->second);
        chunks_.erase(it);
    }
    // They will be re-queued by the next stream pass, nearest first.
}

void VoxelTerrain3D::apply_edit(const Edit &e) {
    if (!edited_) return;
    edited_->add_edit(e);
    remesh_region(e.bounds());
}

void VoxelTerrain3D::dig(const Vec3 &centre, float radius) {
    Edit e;
    e.shape = Edit::Shape::Sphere;
    e.op = Edit::Op::Subtract;
    e.centre = centre;
    e.radius = radius;
    apply_edit(e);
}

void VoxelTerrain3D::build(const Vec3 &centre, float radius, int64_t material) {
    Edit e;
    e.shape = Edit::Shape::Sphere;
    e.op = Edit::Op::Add;
    e.centre = centre;
    e.radius = radius;
    e.material = MaterialId(material);
    apply_edit(e);
}

void VoxelTerrain3D::paint(const Vec3 &centre, float radius, int64_t material) {
    Edit e;
    e.shape = Edit::Shape::Sphere;
    e.op = Edit::Op::Paint;
    e.centre = centre;
    e.radius = radius;
    e.material = MaterialId(material);
    apply_edit(e);
}

void VoxelTerrain3D::clear_edits() {
    if (!edited_) return;
    edited_->clear_edits();
    rebuild();
}

// -------------------------------------------------------------- queries

// THE HEIGHT OF THE GROUND, FOUND BY LOOKING FOR IT.
//
// The generator has an analytic height function, and it is the wrong
// answer: the field warps the sample point before consulting it, and
// carves caves out of the result. Trusting the analytic value drops
// things through the floor or leaves them hanging in the air, by
// however much the warp happened to be worth there.
//
// So the real field is searched: down in coarse steps until it goes
// solid, then bisected. Forty samples, once, when something is
// placed.
float VoxelTerrain3D::height_at(float x, float z) const {
    if (!edited_) return 0.0f;
    // Start above anything the generator could produce.
    float top = 512.0f;
    if (terrain_) top = terrain_->surface_height(x, z) + 64.0f;
    const float bottom = top - 640.0f;

    float y = top;
    float step = 8.0f;
    while (y > bottom && edited_->sample({x, y, z}).distance > 0.0f) y -= step;
    if (y <= bottom) return bottom;
    // Bisect between the last air and the first solid.
    float air = y + step, solid = y;
    for (int i = 0; i < 24; i++) {
        const float mid = (air + solid) * 0.5f;
        if (edited_->sample({x, mid, z}).distance > 0.0f)
            air = mid;
        else
            solid = mid;
    }
    return air;
}

float VoxelTerrain3D::distance_at(const Vec3 &p) const {
    return edited_ ? edited_->sample(p).distance : 1.0f;
}

int64_t VoxelTerrain3D::material_at(const Vec3 &p) const {
    return edited_ ? int64_t(edited_->sample(p).material) : 0;
}

std::string VoxelTerrain3D::report() const {
    char b[256];
    std::snprintf(b, sizeof(b),
                  "voxel: %u live, %u meshing, %u queued, %u empty, %u triangles, "
                  "%llu meshed in total",
                  stats_.chunks_live, stats_.chunks_meshing, stats_.chunks_queued,
                  stats_.chunks_empty, stats_.triangles,
                  (unsigned long long)stats_.chunks_meshed_total);
    return b;
}

// ------------------------------------------------------------ reflection

static void register_voxel_classes() {
    ClassBuilder<VoxelTerrain3D>()
        .field("chunk_resolution", &VoxelTerrain3D::chunk_resolution, "range:8,64")
        .field("cell_size", &VoxelTerrain3D::cell_size, "range:0.1,8")
        .field("view_distance", &VoxelTerrain3D::view_distance, "range:32,2048")
        .field("queue_per_frame", &VoxelTerrain3D::queue_per_frame, "range:1,64")
        .field("max_in_flight", &VoxelTerrain3D::max_in_flight, "range:0,256")
        .field("upload_per_frame", &VoxelTerrain3D::upload_per_frame, "range:1,32")
        .field("generate_collision", &VoxelTerrain3D::generate_collision)
        .field("collision_distance", &VoxelTerrain3D::collision_distance)
        .method("set_viewer", &VoxelTerrain3D::set_viewer).args("node")
        .method("set_seed", &VoxelTerrain3D::set_seed).args("seed")
        .method("dig", &VoxelTerrain3D::dig).args("centre", "radius")
        .method("build", &VoxelTerrain3D::build, {Variant(int64_t(3))})
        .args("centre", "radius", "material")
        .method("paint", &VoxelTerrain3D::paint, {Variant(int64_t(3))})
        .args("centre", "radius", "material")
        .method("clear_edits", &VoxelTerrain3D::clear_edits)
        .method("rebuild", &VoxelTerrain3D::rebuild)
        .method("wait_for_chunks", &VoxelTerrain3D::wait_for_chunks)
        .method("height_at", &VoxelTerrain3D::height_at).args("x", "z")
        .method("distance_at", &VoxelTerrain3D::distance_at).args("point")
        .method("material_at", &VoxelTerrain3D::material_at).args("point")
        .method("set_material", &VoxelTerrain3D::set_material).args("material")
        .method("report", &VoxelTerrain3D::report);
}
MF_REGISTER(register_voxel_classes)

}  // namespace mf::voxel
