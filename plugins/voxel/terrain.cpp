#include "terrain.h"

#include <algorithm>
#include <cstdio>

#include "app/engine.h"
#include "core/log.h"
#include "physics/world.h"
#include "scene/scene_tree.h"

namespace wr::voxel {

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

void VoxelTerrain3D::set_density(std::unique_ptr<Field> d) {
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
    const float s = chunk_size_at(c.lod);
    return global_position() + Vec3(float(c.x), float(c.y), float(c.z)) * s;
}

AABB VoxelTerrain3D::chunk_bounds(const ChunkCoord &c) const {
    Vec3 o = chunk_origin(c);
    return {o, o + Vec3(chunk_size_at(c.lod))};
}

ChunkCoord VoxelTerrain3D::coord_of(const Vec3 &p) const {
    const float s = chunk_size();
    Vec3 local = p - global_position();
    return {int32_t(std::floor(local.x / s)), int32_t(std::floor(local.y / s)),
            int32_t(std::floor(local.z / s)), 0};
}

float VoxelTerrain3D::lod_range(int lod) const {
    if (max_lod <= 0) return view_distance;
    const float r = lod_distance * float(1 << std::max(0, lod));
    return std::min(r, view_distance);
}

int VoxelTerrain3D::lod_at(float distance) const {
    for (int l = 0; l < max_lod; l++)
        if (distance < lod_range(l)) return l;
    return max_lod;
}

// A SHELL PER LEVEL, AND NO GAP BETWEEN THEM.
//
// A chunk of level L belongs if it reaches into L's shell -- its
// nearest point is within L's range -- and is not already covered
// entirely by the finer level inside it. Testing the NEAREST point
// for the outer edge and the FARTHEST for the inner one is
// deliberately generous at both: two levels may overlap by a chunk
// along a boundary, which the depth buffer settles, whereas the
// tight version leaves a ring of missing ground that it does not.
bool VoxelTerrain3D::wanted(const ChunkCoord &c, const Vec3 &eye) const {
    if (c.lod < 0 || c.lod > std::max(0, max_lod)) return false;
    const AABB box = chunk_bounds(c);
    const float nearest = std::sqrt(box.distance_squared_to(eye));
    if (nearest > lod_range(c.lod)) return false;
    if (c.lod == 0) return true;
    // The farthest corner: only when the whole chunk is inside the
    // finer shell has that shell taken it over.
    float farthest = 0.0f;
    for (int i = 0; i < 8; i++)
        farthest = std::max(farthest, (box.corner(i) - eye).length());
    return farthest > lod_range(c.lod - 1);
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

    ContourRequest req;
    req.field = edited_.get();
    req.origin = chunk_origin(c);
    // THE ONLY LINE LEVEL OF DETAIL NEEDS IN THE MESHER. The same
    // field, sampled at twice the spacing over twice the box; the
    // dual contouring does not know or care which level it is on.
    req.cell_size = cell_size_at(c.lod);
    req.resolution = chunk_resolution;

    raw->state.store(State::Meshing, std::memory_order_release);
    Jobs::submit(
        [raw, req]() {
            contour_field(req, &raw->mesh);
            // TANGENTS ON THE WORKER, NOT AT UPLOAD. Deriving them
            // is O(vertices) over ten thousand of them and needs
            // nothing but the arrays, so it belongs beside the
            // meshing rather than in the frame that takes delivery.
            //
            // Measured: it did NOT move the numbers. The streaming
            // spike is elsewhere -- p99 stayed at ~31 ms either way,
            // and the frame is not CPU-bound in the renderer (3 ms
            // of 12). It stays on the worker because that is where
            // it belongs, not because it bought anything.
            if (!raw->mesh.vertices.empty() && !raw->mesh.indices.empty())
                compute_tangents(raw->mesh.vertices, raw->mesh.indices);
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
    // The tangents were derived on the worker that meshed it; the
    // bounds the mesher already knew. All that is left here is the
    // scene node, which is the only part that has to be on this
    // thread.
    mesh->set_bounds(chunk.mesh.bounds);

    MeshInstance3D *mi = new MeshInstance3D();
    char name[48];
    // The level is part of the identity: two chunks at different
    // levels can share x/y/z, and a debug capture full of duplicate
    // names is a capture you cannot read.
    std::snprintf(name, sizeof(name), "chunk_%d_%d_%d_l%d", chunk.coord.x,
                  chunk.coord.y, chunk.coord.z, chunk.coord.lod);
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
    const int levels = std::max(0, max_lod);

    // --- collect what should exist, nearest first
    //
    // ONE PASS PER LEVEL, each over its own shell. Only the level a
    // piece of ground is meant to be drawn at is visited, so the
    // whole sweep is a few thousand boxes however large the view
    // distance is -- the inner levels are small because their range
    // is small, and the outer ones are small because their chunks
    // are huge.
    scratch_.clear();
    for (int lod = 0; lod <= levels; lod++) {
        const float ls = chunk_size_at(lod);
        const float reach = lod_range(lod) + ls;
        const int radius = std::max(1, int(std::ceil(reach / ls)));
        const Vec3 local = eye - global_position();
        const ChunkCoord centre{int32_t(std::floor(local.x / ls)),
                                int32_t(std::floor(local.y / ls)),
                                int32_t(std::floor(local.z / ls)), lod};
        for (int z = -radius; z <= radius; z++)
            for (int y = -radius; y <= radius; y++)
                for (int x = -radius; x <= radius; x++) {
                    ChunkCoord c{centre.x + x, centre.y + y, centre.z + z, lod};
                    if (!wanted(c, eye)) continue;
                    // THE FIELD'S OWN BOUND, WHICH IS WHAT MAKES THIS
                    // AFFORDABLE. Most of a world is solid rock or
                    // open sky, and a source that can say so for a
                    // whole box saves meshing thirty-odd thousand
                    // samples to discover it.
                    if (edited_->bound(chunk_bounds(c)) > ls * 0.87f) continue;
                    scratch_.push_back(c);
                }
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
        const bool keep = wanted(chunk.coord, eye);
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
            if (chunk.coord.lod >= 0 && chunk.coord.lod < 8)
                stats_.chunks_by_lod[chunk.coord.lod]++;
            if (chunk.gpu_mesh) stats_.triangles += chunk.gpu_mesh->triangle_count();
            // Collision comes and goes with distance, so a world can
            // be large without every chunk paying for a BVH. Only at
            // level 0: a coarse chunk's surface is metres away from
            // where the fine one puts it, and standing on the wrong
            // one is worse than standing on nothing.
            if (generate_collision && g_physics) {
                const bool want = chunk.coord.lod == 0 &&
                                  chunk.distance < collision_distance;
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
        // A chunk goes when its level no longer covers where it is --
        // because the viewer moved away and a coarser level took
        // over, or closer and a finer one did.
        if (!keep && state != State::Meshing && state != State::Queued)
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
    // border. THE SLACK IS THE CHUNK'S OWN CELL, not the finest one:
    // a level-4 chunk's cell is sixteen metres across, and growing
    // the edit by the level-0 cell would leave it holding stale
    // geometry a dug tunnel had already removed.
    std::vector<ChunkCoord> hit;
    for (auto &kv : chunks_) {
        const AABB grown = box.grown(cell_size_at(kv.first.lod) * 2.0f);
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

bool VoxelTerrain3D::is_loaded(const Vec3 &p) const { return loaded_lod(p) >= 0; }

int VoxelTerrain3D::loaded_lod(const Vec3 &p) const {
    int best = -1;
    for (const auto &kv : chunks_) {
        const State st = kv.second->state.load(std::memory_order_acquire);
        if (st != State::Ready && st != State::Empty) continue;
        if (!chunk_bounds(kv.first).contains(p)) continue;
        // The finest level wins where two overlap, which is what is
        // actually being drawn in front.
        if (best < 0 || kv.first.lod < best) best = kv.first.lod;
    }
    return best;
}

std::string VoxelTerrain3D::report() const {
    char lods[64] = "";
    size_t at = 0;
    for (int l = 0; l <= std::min(max_lod, 7) && at + 8 < sizeof(lods); l++)
        at += size_t(std::snprintf(lods + at, sizeof(lods) - at, "%s%u",
                                   l ? "/" : "", stats_.chunks_by_lod[l]));
    char b[320];
    std::snprintf(b, sizeof(b),
                  "voxel: %u live (lod %s), %u meshing, %u queued, %u empty, "
                  "%u triangles, %llu meshed in total",
                  stats_.chunks_live, lods, stats_.chunks_meshing,
                  stats_.chunks_queued, stats_.chunks_empty, stats_.triangles,
                  (unsigned long long)stats_.chunks_meshed_total);
    return b;
}

// ------------------------------------------------------------ reflection

static void register_voxel_classes() {
    ClassBuilder<VoxelTerrain3D>()
        .field("chunk_resolution", &VoxelTerrain3D::chunk_resolution, "range:8,64")
        .field("cell_size", &VoxelTerrain3D::cell_size, "range:0.1,8")
        .field("view_distance", &VoxelTerrain3D::view_distance, "range:32,4096")
        .field("max_lod", &VoxelTerrain3D::max_lod, "range:0,7")
        .field("lod_distance", &VoxelTerrain3D::lod_distance, "range:8,512")
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
        .method("is_loaded", &VoxelTerrain3D::is_loaded).args("point")
        .method("lod_at", &VoxelTerrain3D::lod_at).args("distance")
        .method("lod_range", &VoxelTerrain3D::lod_range).args("lod")
        .method("loaded_lod", &VoxelTerrain3D::loaded_lod).args("point")
        .method("material_at", &VoxelTerrain3D::material_at).args("point")
        .method("set_material", &VoxelTerrain3D::set_material).args("material")
        .method("report", &VoxelTerrain3D::report);
}
WR_REGISTER(register_voxel_classes)

}  // namespace wr::voxel
