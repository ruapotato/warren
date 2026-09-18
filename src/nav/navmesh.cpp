#include "nav/navmesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>

#include "core/bind.h"
#include "core/log.h"
#include "core/serialize.h"

namespace wr::nav {

namespace {

constexpr uint32_t kNavMagic = 0x564e5257;  // "WRNV"
constexpr uint32_t kNavVersion = 1;

// Twice the signed area in the xz plane. The funnel is entirely
// built out of this one predicate.
float area2(const Vec3 &a, const Vec3 &b, const Vec3 &c) {
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}
float dist2_xz(const Vec3 &a, const Vec3 &b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}
bool same_xz(const Vec3 &a, const Vec3 &b) {
    return dist2_xz(a, b) < 1e-8f;
}

// The closest point to `p` on the segment ab, in the xz plane, with
// the height interpolated along the way.
Vec3 closest_on_segment(const Vec3 &p, const Vec3 &a, const Vec3 &b) {
    Vec3 ab = b - a;
    float len = ab.x * ab.x + ab.z * ab.z;
    if (len < 1e-12f) return a;
    float t = ((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / len;
    t = std::clamp(t, 0.0f, 1.0f);
    return a + ab * t;
}

}  // namespace

// --------------------------------------------------------------- bake

bool NavMesh::bake(const std::vector<Vec3> &triangles, const AABB &bounds,
                   const BakeSettings &settings, BakeStats *stats) {
    auto t0 = std::chrono::steady_clock::now();

    Heightfield hf;
    if (!hf.build(triangles, bounds, settings.cell_size, settings.cell_height,
                  settings.agent.max_slope_degrees)) {
        WR_ERROR("nav: could not rasterise %zu triangles into the bake volume",
                 triangles.size() / 3);
        return false;
    }
    CompactField field;
    if (!field.build(hf, settings.agent)) return false;
    field.erode(settings.agent.radius);

    bool ok = bake_from(field, settings, stats);
    if (stats) {
        stats->seconds = std::chrono::duration<float>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    }
    return ok;
}

bool NavMesh::bake_from(const CompactField &input, const BakeSettings &settings,
                        BakeStats *stats) {
    auto t0 = std::chrono::steady_clock::now();
    CompactField field = input;
    field.build_regions(settings.min_region_area);

    ContourSet contours;
    int max_edge_cells =
        settings.max_edge_length > 0.0f
            ? int(settings.max_edge_length / settings.cell_size)
            : 0;
    if (!build_contours(field, settings.contour_max_error, max_edge_cells,
                        &contours))
        return false;

    verts_.clear();
    polys_.clear();
    if (!build_poly_mesh(contours, &verts_, &polys_)) return false;

    bounds_ = AABB();
    for (const Vec3 &v : verts_) bounds_.expand(v);
    build_grid();
    resolve_links();
    // Pruning comes after the links are resolved, because a link is
    // an edge of the reachability graph -- a roof reached only by a
    // ladder must not be pruned for want of a walk to it.
    int pruned = prune_unreachable(settings.reachable_from);

    if (stats) {
        stats->spans = int(field.spans().size());
        stats->regions = field.region_count() - 1;
        stats->contours = int(contours.contours.size());
        stats->polys = int(polys_.size());
        stats->verts = int(verts_.size());
        stats->merged_holes = contours.merged_holes;
        stats->pruned = pruned;
        stats->seconds = std::chrono::duration<float>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    }
    return true;
}

// ------------------------------------------------------- spatial index

void NavMesh::build_grid() {
    grid_start_.clear();
    grid_items_.clear();
    grid_w_ = grid_d_ = 0;
    if (polys_.empty() || !bounds_.valid()) return;

    // Aim at a handful of polygons per cell. Too fine and the grid
    // is mostly empty; too coarse and a point query scans the level.
    Vec3 size = bounds_.size();
    float area = std::fmax(size.x * size.z, 1.0f);
    grid_cell_ = std::fmax(std::sqrt(area / float(polys_.size())) * 2.0f, 0.5f);
    grid_w_ = std::max(1, int(std::ceil(size.x / grid_cell_)));
    grid_d_ = std::max(1, int(std::ceil(size.z / grid_cell_)));
    if (int64_t(grid_w_) * int64_t(grid_d_) > 4 << 20) {
        grid_w_ = grid_d_ = 0;  // absurd; fall back to a linear scan
        return;
    }

    const size_t n = size_t(grid_w_) * size_t(grid_d_);
    std::vector<uint32_t> counts(n + 1, 0);
    auto span_of = [&](const NavPoly &p, int *x0, int *z0, int *x1, int *z1) {
        AABB b;
        for (int k = 0; k < p.count; ++k) b.expand(verts_[p.verts[k]]);
        *x0 = std::clamp(int((b.min.x - bounds_.min.x) / grid_cell_), 0,
                         grid_w_ - 1);
        *x1 = std::clamp(int((b.max.x - bounds_.min.x) / grid_cell_), 0,
                         grid_w_ - 1);
        *z0 = std::clamp(int((b.min.z - bounds_.min.z) / grid_cell_), 0,
                         grid_d_ - 1);
        *z1 = std::clamp(int((b.max.z - bounds_.min.z) / grid_cell_), 0,
                         grid_d_ - 1);
    };

    for (const NavPoly &p : polys_) {
        int x0, z0, x1, z1;
        span_of(p, &x0, &z0, &x1, &z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                ++counts[size_t(z) * size_t(grid_w_) + size_t(x) + 1];
    }
    for (size_t i = 1; i <= n; ++i) counts[i] += counts[i - 1];
    grid_items_.resize(counts[n]);
    std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
    for (size_t i = 0; i < polys_.size(); ++i) {
        int x0, z0, x1, z1;
        span_of(polys_[i], &x0, &z0, &x1, &z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                grid_items_[cursor[size_t(z) * size_t(grid_w_) + size_t(x)]++] =
                    uint16_t(i);
    }
    grid_start_.assign(counts.begin(), counts.end());
}

void NavMesh::query_cells(const Vec3 &mn, const Vec3 &mx,
                          std::vector<uint16_t> *out) const {
    out->clear();
    if (grid_w_ == 0) {
        out->resize(polys_.size());
        for (size_t i = 0; i < polys_.size(); ++i) (*out)[i] = uint16_t(i);
        return;
    }
    int x0 = std::clamp(int((mn.x - bounds_.min.x) / grid_cell_), 0, grid_w_ - 1);
    int x1 = std::clamp(int((mx.x - bounds_.min.x) / grid_cell_), 0, grid_w_ - 1);
    int z0 = std::clamp(int((mn.z - bounds_.min.z) / grid_cell_), 0, grid_d_ - 1);
    int z1 = std::clamp(int((mx.z - bounds_.min.z) / grid_cell_), 0, grid_d_ - 1);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            size_t c = size_t(z) * size_t(grid_w_) + size_t(x);
            for (uint32_t k = grid_start_[c]; k < grid_start_[c + 1]; ++k)
                out->push_back(grid_items_[k]);
        }
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
}

// ------------------------------------------------------------ queries

Vec3 NavMesh::poly_center(const NavPoly &p) const {
    Vec3 c;
    for (int k = 0; k < p.count; ++k) c = c + verts_[p.verts[k]];
    return p.count ? c * (1.0f / float(p.count)) : c;
}

bool NavMesh::height_at(uint16_t poly, const Vec3 &p, float *y) const {
    if (poly >= polys_.size()) return false;
    const NavPoly &np = polys_[poly];
    // Fan the polygon from its first vertex and find the triangle
    // containing the point. Convexity makes this safe: exactly one
    // triangle of the fan contains any interior point.
    const Vec3 &v0 = verts_[np.verts[0]];
    for (int k = 1; k + 1 < np.count; ++k) {
        const Vec3 &v1 = verts_[np.verts[k]];
        const Vec3 &v2 = verts_[np.verts[k + 1]];
        float d = area2(v0, v1, v2);
        if (std::fabs(d) < 1e-9f) continue;
        float a = area2(p, v1, v2) / d;
        float b = area2(v0, p, v2) / d;
        float c = 1.0f - a - b;
        const float eps = -1e-4f;
        if (a < eps || b < eps || c < eps) continue;
        if (y) *y = v0.y * a + v1.y * b + v2.y * c;
        return true;
    }
    return false;
}

uint16_t NavMesh::find_poly(const Vec3 &p, const Vec3 &extents,
                            const NavFilter &filter) const {
    std::vector<uint16_t> candidates;
    query_cells(p - extents, p + extents, &candidates);
    uint16_t best = kNoPoly;
    float best_dy = extents.y;
    for (uint16_t i : candidates) {
        if (!filter.passes(polys_[i].area)) continue;
        float y = 0.0f;
        if (!height_at(i, p, &y)) continue;
        // Several floors can contain the same point in plan. The one
        // meant is the one nearest in height -- which is why the
        // vertical extent is a separate number and usually small.
        float dy = std::fabs(y - p.y);
        if (dy > best_dy) continue;
        best_dy = dy;
        best = i;
    }
    return best;
}

bool NavMesh::nearest_point(const Vec3 &p, const Vec3 &extents, Vec3 *out,
                            uint16_t *poly, const NavFilter &filter) const {
    uint16_t inside = find_poly(p, extents, filter);
    if (inside != kNoPoly) {
        float y = p.y;
        height_at(inside, p, &y);
        if (out) *out = Vec3(p.x, y, p.z);
        if (poly) *poly = inside;
        return true;
    }
    // Not over any polygon: walk the edges of everything nearby. If
    // nothing is nearby, widen once to the whole mesh rather than
    // failing -- a caller asking for the nearest point wants an
    // answer, and "none" is only true for an empty mesh.
    std::vector<uint16_t> candidates;
    query_cells(p - extents, p + extents, &candidates);
    if (candidates.empty()) {
        candidates.resize(polys_.size());
        for (size_t i = 0; i < polys_.size(); ++i) candidates[i] = uint16_t(i);
    }
    float best = 1e30f;
    Vec3 best_p;
    uint16_t best_poly = kNoPoly;
    for (uint16_t i : candidates) {
        const NavPoly &np = polys_[i];
        if (!filter.passes(np.area)) continue;
        for (int k = 0; k < np.count; ++k) {
            Vec3 c = closest_on_segment(p, verts_[np.verts[k]],
                                        verts_[np.verts[(k + 1) % np.count]]);
            float d = (c - p).length_sq();
            if (d >= best) continue;
            best = d;
            best_p = c;
            best_poly = i;
        }
    }
    if (best_poly == kNoPoly) return false;
    // A HAIR INSIDE, NOT EXACTLY ON.
    //
    // The closest point on an edge is on the boundary, and computing
    // it in floats lands a fraction to one side or the other. Land
    // on the outside and every following query disagrees that the
    // point is on the mesh at all: find_poly returns nothing, a ray
    // from it goes nowhere, and a body placed there stands still for
    // ever wondering why. Which is exactly the failure this was
    // written for.
    //
    // A thousandth of the way toward the polygon's middle is far
    // below anything that matters and puts the point unambiguously
    // inside.
    {
        const NavPoly &np = polys_[best_poly];
        Vec3 c;
        for (int k = 0; k < np.count; ++k) c = c + verts_[np.verts[k]];
        if (np.count) best_p = best_p + (c * (1.0f / float(np.count)) - best_p) *
                                            1e-3f;
    }
    if (out) *out = best_p;
    if (poly) *poly = best_poly;
    return true;
}

void NavMesh::portal(uint16_t a, int e, Vec3 *left, Vec3 *right) const {
    const NavPoly &p = polys_[a];
    // WHICH END IS LEFT depends on the winding, and getting it
    // backwards does not crash -- it produces paths that hug the
    // wrong wall and cut corners through geometry. Contours come out
    // with the interior on the left of each directed edge, so
    // crossing an edge means leaving to its right, and the far
    // endpoint of the edge is then on the traveller's left.
    *left = verts_[p.verts[(e + 1) % p.count]];
    *right = verts_[p.verts[e]];
}

int NavMesh::prune_unreachable(const std::vector<Vec3> &seeds) {
    if (seeds.empty() || polys_.empty()) return 0;

    std::vector<bool> keep(polys_.size(), false);
    std::vector<uint16_t> stack;
    for (const Vec3 &s : seeds) {
        uint16_t p = kNoPoly;
        Vec3 unused;
        if (!nearest_point(s, Vec3(4.0f, 4.0f, 4.0f), &unused, &p)) continue;
        if (p == kNoPoly || keep[p]) continue;
        keep[p] = true;
        stack.push_back(p);
    }
    // Links are edges of this graph too: a roof reached only by a
    // ladder is reachable, and pruning it would delete the ladder's
    // far end along with it.
    std::vector<std::vector<uint16_t>> via_link(polys_.size());
    for (const NavLink &l : links_) {
        if (l.from_poly == kNoPoly || l.to_poly == kNoPoly) continue;
        via_link[l.from_poly].push_back(l.to_poly);
        if (l.bidirectional) via_link[l.to_poly].push_back(l.from_poly);
    }
    while (!stack.empty()) {
        uint16_t p = stack.back();
        stack.pop_back();
        for (int k = 0; k < polys_[p].count; ++k) {
            uint16_t n = polys_[p].neis[k];
            if (n == kNoPoly || keep[n]) continue;
            keep[n] = true;
            stack.push_back(n);
        }
        for (uint16_t n : via_link[p]) {
            if (keep[n]) continue;
            keep[n] = true;
            stack.push_back(n);
        }
    }

    int dropped = 0;
    for (bool k : keep)
        if (!k) ++dropped;
    if (dropped == 0) return 0;

    // A SEED IN THE WRONG PLACE DELETES THE LEVEL, quietly, and the
    // result is a game where nothing can path anywhere. It is an
    // easy mistake: the obvious point to name is the middle of the
    // map, and the middle of the map is as likely as not to be a
    // statue, a table, or the roof of whatever stands there -- a
    // small island, correctly identified as all that is reachable
    // from itself.
    //
    // There is no rule that says which answer is right, so this
    // does not refuse. It says so, loudly, with the number that
    // makes the mistake recognisable.
    if (size_t(dropped) * 5 > polys_.size() * 4) {
        WR_WARN("nav: pruning from %zu seed(s) keeps only %zu of %zu "
                "polygons. If that is not what was meant, a seed is "
                "probably standing on something small -- check it is on "
                "the ground a body would start from.",
                seeds.size(), polys_.size() - size_t(dropped), polys_.size());
    }

    // Compact the polygons, then the vertices they still use. Doing
    // it in that order means the vertex pass can simply keep what is
    // referenced.
    std::vector<uint16_t> poly_map(polys_.size(), kNoPoly);
    std::vector<NavPoly> kept;
    kept.reserve(polys_.size() - size_t(dropped));
    for (size_t i = 0; i < polys_.size(); ++i) {
        if (!keep[i]) continue;
        poly_map[i] = uint16_t(kept.size());
        kept.push_back(polys_[i]);
    }
    for (NavPoly &p : kept)
        for (int k = 0; k < p.count; ++k)
            p.neis[k] = p.neis[k] == kNoPoly ? kNoPoly : poly_map[p.neis[k]];

    std::vector<uint16_t> vert_map(verts_.size(), 0xffff);
    std::vector<Vec3> verts;
    for (NavPoly &p : kept) {
        for (int k = 0; k < p.count; ++k) {
            uint16_t v = p.verts[k];
            if (vert_map[v] == 0xffff) {
                vert_map[v] = uint16_t(verts.size());
                verts.push_back(verts_[v]);
            }
            p.verts[k] = vert_map[v];
        }
    }
    polys_.swap(kept);
    verts_.swap(verts);

    bounds_ = AABB();
    for (const Vec3 &v : verts_) bounds_.expand(v);
    build_grid();
    resolve_links();
    return dropped;
}

int NavMesh::set_area_in(const AABB &box, uint16_t set_bits,
                         uint16_t clear_bits) {
    int changed = 0;
    for (NavPoly &p : polys_) {
        if (p.count == 0) continue;
        Vec3 c = poly_center(p);
        if (!box.contains(c)) continue;
        uint16_t was = p.area;
        p.area = uint16_t((p.area | set_bits) & ~clear_bits);
        if (p.area != was) ++changed;
    }
    return changed;
}

uint16_t NavMesh::area_at(const Vec3 &p, const Vec3 &extents) const {
    uint16_t poly = find_poly(p, extents);
    return poly == kNoPoly ? 0 : polys_[poly].area;
}

void NavMesh::add_link(const NavLink &link) {
    links_.push_back(link);
    resolve_links();
}
void NavMesh::clear_links() { links_.clear(); }

void NavMesh::resolve_links() {
    for (NavLink &l : links_) {
        Vec3 ext(l.radius, std::fmax(l.radius, 1.0f), l.radius);
        Vec3 p;
        l.from_poly = kNoPoly;
        l.to_poly = kNoPoly;
        if (nearest_point(l.from, ext, &p, &l.from_poly) &&
            (p - l.from).length() > l.radius)
            l.from_poly = kNoPoly;
        if (nearest_point(l.to, ext, &p, &l.to_poly) &&
            (p - l.to).length() > l.radius)
            l.to_poly = kNoPoly;
        if (l.from_poly == kNoPoly || l.to_poly == kNoPoly) {
            WR_WARN("nav: link '%s' has an end that is not on the mesh "
                    "within %.2f m; it will not be used",
                    l.name.c_str(), double(l.radius));
        }
    }
}

// ------------------------------------------------------------ pathing

namespace {

struct SearchNode {
    float cost = 0.0f;   // cost so far
    float total = 0.0f;  // cost so far plus heuristic
    Vec3 entry;          // where the path crosses into this polygon
    uint16_t parent = kNoPoly;
    uint16_t link = 0xffff;  // the link entered by, if any
    uint8_t edge = 0xff;     // the edge entered by, if a walk
    bool closed = false;
    bool open = false;
};

struct Candidate {
    float total;
    uint16_t poly;
    bool operator>(const Candidate &o) const { return total > o.total; }
};

}  // namespace

bool NavMesh::find_path(const Vec3 &from, const Vec3 &to,
                        std::vector<PathPoint> *out, bool *partial,
                        const NavFilter &filter) const {
    if (out) out->clear();
    if (partial) *partial = false;
    if (polys_.empty()) return false;

    Vec3 start, goal;
    uint16_t start_poly = kNoPoly, goal_poly = kNoPoly;
    const Vec3 ext(2.0f, 4.0f, 2.0f);
    if (!nearest_point(from, ext, &start, &start_poly, filter)) return false;
    if (!nearest_point(to, ext, &goal, &goal_poly, filter)) return false;

    // A GOAL THAT SNAPPED A LONG WAY IS NOT THE GOAL.
    //
    // Both ends are put on the mesh before searching, which is what
    // lets a caller name a point half a metre above the floor, or
    // just inside a wall, and get a sensible answer. But the filter
    // decides what counts as the mesh, so a goal inside a zone this
    // body may not enter snaps to the nearest zone it may -- and a
    // route to THAT is not a partial route, it is a complete route
    // to somewhere else, which is worse, because the caller is told
    // it succeeded.
    //
    // Snapping further than the search box means the point asked
    // for was not on reachable ground. The path still goes as far
    // as it can, and now says so.
    const bool goal_moved =
        (goal - to).length() > std::fmax(ext.x, ext.z) * 1.5f;

    // Links leaving each polygon, gathered once. A level has few
    // links and many polygons, so scanning the link list per
    // expansion would be the whole cost of the search.
    std::vector<std::vector<std::pair<uint16_t, bool>>> out_links;
    if (!links_.empty()) {
        out_links.resize(polys_.size());
        for (size_t i = 0; i < links_.size(); ++i) {
            const NavLink &l = links_[i];
            if (l.from_poly == kNoPoly || l.to_poly == kNoPoly) continue;
            if (!filter.passes(l.area)) continue;
            out_links[l.from_poly].push_back({uint16_t(i), true});
            if (l.bidirectional)
                out_links[l.to_poly].push_back({uint16_t(i), false});
        }
    }

    std::vector<SearchNode> nodes(polys_.size());
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> queue;

    nodes[start_poly].cost = 0.0f;
    nodes[start_poly].total = (goal - start).length();
    nodes[start_poly].entry = start;
    nodes[start_poly].open = true;
    queue.push({nodes[start_poly].total, start_poly});

    // The best we managed, for the partial answer. A body that walks
    // as far as it can toward a goal it cannot reach looks like it
    // is trying; one that stands still looks broken.
    uint16_t best_poly = start_poly;
    float best_h = nodes[start_poly].total;
    bool found = false;

    while (!queue.empty()) {
        Candidate c = queue.top();
        queue.pop();
        if (nodes[c.poly].closed) continue;
        nodes[c.poly].closed = true;

        if (c.poly == goal_poly) {
            found = true;
            break;
        }
        float h = dist2_xz(nodes[c.poly].entry, goal);
        if (h < best_h * best_h) {
            best_h = std::sqrt(h);
            best_poly = c.poly;
        }

        auto relax = [&](uint16_t next, const Vec3 &entry, float step,
                         uint16_t link, uint8_t edge) {
            if (next == kNoPoly || next >= polys_.size()) return;
            if (nodes[next].closed) return;
            if (!filter.passes(polys_[next].area)) return;
            float cost = nodes[c.poly].cost +
                         step * filter.multiplier(polys_[next].area);
            if (nodes[next].open && cost >= nodes[next].cost) return;
            nodes[next].cost = cost;
            nodes[next].total = cost + (goal - entry).length();
            nodes[next].entry = entry;
            nodes[next].parent = c.poly;
            nodes[next].link = link;
            nodes[next].edge = edge;
            nodes[next].open = true;
            queue.push({nodes[next].total, next});
        };

        const NavPoly &p = polys_[c.poly];
        for (int k = 0; k < p.count; ++k) {
            if (p.neis[k] == kNoPoly) continue;
            // Entering across the middle of the shared edge is the
            // approximation Detour makes too. The funnel afterwards
            // moves the actual crossing point to wherever the taut
            // string wants it; this only has to rank corridors.
            Vec3 mid = (verts_[p.verts[k]] +
                        verts_[p.verts[(k + 1) % p.count]]) *
                       0.5f;
            relax(p.neis[k], mid, (mid - nodes[c.poly].entry).length(), 0xffff,
                  uint8_t(k));
        }
        if (!out_links.empty()) {
            for (auto [li, forward] : out_links[c.poly]) {
                const NavLink &l = links_[li];
                const Vec3 &a = forward ? l.from : l.to;
                const Vec3 &b = forward ? l.to : l.from;
                uint16_t next = forward ? l.to_poly : l.from_poly;
                float step = (a - nodes[c.poly].entry).length() +
                             (l.cost > 0.0f ? l.cost : (b - a).length());
                relax(next, b, step, li, 0xff);
            }
        }
    }

    if (goal_moved && partial) *partial = true;

    if (!found) {
        if (partial) *partial = true;
        goal_poly = best_poly;
        if (goal_poly == start_poly) {
            // Nowhere to go. Return the start, so the caller gets a
            // valid one-point path rather than an empty one it has
            // to special-case.
            if (out) out->push_back({start, start_poly, 0xffff, kPathEnd});
            return true;
        }
        goal = nodes[goal_poly].entry;
    }

    // Unwind. Each step records how it was entered, which is what
    // separates a walk from a link.
    struct Step {
        uint16_t poly;
        uint16_t link;
        uint8_t edge;
    };
    std::vector<Step> corridor;
    for (uint16_t p = goal_poly;;) {
        corridor.push_back({p, nodes[p].link, nodes[p].edge});
        if (p == start_poly) break;
        uint16_t parent = nodes[p].parent;
        if (parent == kNoPoly) break;
        p = parent;
    }
    std::reverse(corridor.begin(), corridor.end());
    if (!out) return true;

    // ------------------------------------------------- the funnel
    //
    // Pull a string taut through the corridor. The funnel is the
    // wedge between the leftmost and rightmost points still
    // reachable in a straight line from the current apex; each
    // portal narrows it. When a new portal's edge crosses the far
    // side of the wedge, the string has to bend, and it bends around
    // whichever side it crossed -- that point becomes the next apex
    // and a corner of the path.
    //
    // A link interrupts this. The string cannot be pulled through a
    // ladder, so the run ends at the link's near end, the far end
    // starts a new run, and the two points are emitted with the
    // link's mark on them.
    auto pull = [&](size_t begin, size_t end, const Vec3 &entry,
                    const Vec3 &exit) {
        std::vector<std::pair<Vec3, Vec3>> portals;
        portals.push_back({entry, entry});
        for (size_t i = begin + 1; i <= end; ++i) {
            const Step &s = corridor[i];
            if (s.edge == 0xff) break;
            Vec3 l, r;
            portal(corridor[i - 1].poly, s.edge, &l, &r);
            portals.push_back({l, r});
        }
        portals.push_back({exit, exit});

        Vec3 apex = portals[0].first, left = apex, right = apex;
        size_t apex_i = 0, left_i = 0, right_i = 0;
        for (size_t i = 1; i < portals.size(); ++i) {
            const Vec3 &l = portals[i].first;
            const Vec3 &r = portals[i].second;

            if (area2(apex, right, r) <= 0.0f) {
                if (same_xz(apex, right) || area2(apex, left, r) > 0.0f) {
                    right = r;
                    right_i = i;
                } else {
                    // The right edge has swung past the left one:
                    // the string catches on the left point.
                    out->push_back({left, corridor[std::min(left_i + begin,
                                                            corridor.size() - 1)]
                                              .poly,
                                    0xffff, kPathWalk});
                    apex = left;
                    apex_i = left_i;
                    left = apex;
                    right = apex;
                    left_i = apex_i;
                    right_i = apex_i;
                    i = apex_i;
                    continue;
                }
            }
            if (area2(apex, left, l) >= 0.0f) {
                if (same_xz(apex, left) || area2(apex, right, l) < 0.0f) {
                    left = l;
                    left_i = i;
                } else {
                    out->push_back({right, corridor[std::min(right_i + begin,
                                                             corridor.size() - 1)]
                                               .poly,
                                    0xffff, kPathWalk});
                    apex = right;
                    apex_i = right_i;
                    left = apex;
                    right = apex;
                    left_i = apex_i;
                    right_i = apex_i;
                    i = apex_i;
                    continue;
                }
            }
        }
    };

    out->push_back({start, start_poly, 0xffff, kPathWalk});
    size_t run_start = 0;
    Vec3 run_entry = start;
    for (size_t i = 1; i < corridor.size(); ++i) {
        if (corridor[i].edge != 0xff) continue;
        // A link. Finish the walk up to its near end, then jump.
        const NavLink &l = links_[corridor[i].link];
        bool forward = l.from_poly == corridor[i - 1].poly;
        const Vec3 &near_end = forward ? l.from : l.to;
        const Vec3 &far_end = forward ? l.to : l.from;
        pull(run_start, i - 1, run_entry, near_end);
        out->push_back({near_end, corridor[i - 1].poly, corridor[i].link,
                        kPathLink});
        out->push_back({far_end, corridor[i].poly, corridor[i].link,
                        kPathWalk});
        run_start = i;
        run_entry = far_end;
    }
    pull(run_start, corridor.size() - 1, run_entry, goal);
    out->push_back({goal, goal_poly, 0xffff, kPathEnd});

    // Drop points the funnel emitted twice -- an apex that lands on
    // the previous corner, which happens where two portals share an
    // endpoint.
    out->erase(std::unique(out->begin(), out->end(),
                           [](const PathPoint &a, const PathPoint &b) {
                               return a.flags == b.flags &&
                                      (a.position - b.position).length_sq() <
                                          1e-8f;
                           }),
               out->end());
    return true;
}

bool NavMesh::raycast(const Vec3 &from, const Vec3 &to, Vec3 *hit,
                      const NavFilter &filter, Vec3 *normal) const {
    if (normal) *normal = Vec3();
    if (hit) *hit = from;

    // WHICH POLYGON TO START IN, when the answer is more than one.
    //
    // A point on a shared edge, or on a vertex, is in every polygon
    // that touches it, and `find_poly` returns whichever it saw
    // first. That is fine for a height query and wrong here: if the
    // ray leaves that polygon immediately, the walk reports a wall
    // at zero distance and a perfectly good route reads as blocked.
    // Since the taut paths this is mostly asked about begin and end
    // exactly on vertices, that case is the common one, not the
    // exotic one.
    //
    // So: collect every polygon the start point touches and try
    // them. The first that does not fail on the spot is the right
    // one, and there is never more than a handful.
    std::vector<uint16_t> starts;
    {
        std::vector<uint16_t> nearby;
        query_cells(from - Vec3(0.05f, 0.0f, 0.05f),
                    from + Vec3(0.05f, 0.0f, 0.05f), &nearby);
        for (uint16_t i : nearby) {
            if (!filter.passes(polys_[i].area)) continue;
            float y = 0.0f;
            if (!height_at(i, from, &y)) continue;
            if (std::fabs(y - from.y) > 2.0f) continue;
            starts.push_back(i);
        }
    }
    if (starts.empty()) {
        uint16_t one = find_poly(from, Vec3(1.0f, 2.0f, 1.0f), filter);
        if (one == kNoPoly) return false;
        starts.push_back(one);
    }

    Vec3 best_hit = from, best_normal;
    float best_reached = -1.0f;
    for (size_t attempt = 0; attempt < starts.size(); ++attempt) {
    uint16_t cur = starts[attempt];

    // CLIP THE WHOLE SEGMENT AGAINST EACH POLYGON, rather than
    // testing it against each edge in turn.
    //
    // The pairwise test looks simpler and cannot be made to work.
    // The rays that matter most here are the ones a taut path is
    // made of, and those run along shared edges and straight
    // through vertices -- precisely where "do these two segments
    // cross" has no answer that is not a coin toss, and where
    // getting it wrong means either a body walks through a wall or
    // a correct path is reported as blocked.
    //
    // Clipping against the convex polygon has no such case. The
    // segment's intersection with a convex region is an interval;
    // grazing a vertex just means two edges agree about where that
    // interval ends, and either one leads to the same place.
    // Parameters are measured from `from` throughout, so nothing
    // accumulates across the walk.
    Vec3 dir = to - from;
    dir.y = 0.0f;
    if (dir.length_sq() < 1e-12f) {
        if (hit) *hit = to;
        return true;
    }
    auto at = [&](float t) {
        Vec3 p = from + dir * t;
        p.y = from.y + (to.y - from.y) * t;
        return p;
    };

    float reached = 0.0f;
    for (int guard = 0; guard < 512; ++guard) {
        const NavPoly &np = polys_[cur];
        float tmin = 0.0f, tmax = 1.0f;
        int exit_edge = -1;
        bool outside = false;

        for (int k = 0; k < np.count; ++k) {
            const Vec3 &a = verts_[np.verts[k]];
            const Vec3 &b = verts_[np.verts[(k + 1) % np.count]];
            Vec3 e = b - a;
            float el = std::sqrt(e.x * e.x + e.z * e.z);
            if (el < 1e-9f) continue;
            // Outward normal: the interior is on the left of a
            // directed edge, so the outside is to the right.
            Vec3 n(-e.z / el, 0.0f, e.x / el);
            float denom = dir.x * n.x + dir.z * n.z;
            float dist = (from.x - a.x) * n.x + (from.z - a.z) * n.z;

            if (std::fabs(denom) < 1e-9f) {
                // Running parallel to this edge: either inside it
                // for the whole segment, or outside it for the
                // whole segment.
                if (dist > 1e-4f) {
                    outside = true;
                    break;
                }
                continue;
            }
            float t = -dist / denom;
            if (denom < 0.0f) {
                tmin = std::fmax(tmin, t);  // entering across this edge
            } else if (t < tmax) {
                tmax = t;                   // leaving across this edge
                exit_edge = k;
            }
            if (tmin > tmax + 1e-6f) {
                outside = true;
                break;
            }
        }
        if (outside) break;
        if (exit_edge < 0 || tmax >= 1.0f) {
            if (hit) *hit = to;
            return true;
        }

        uint16_t next = np.neis[exit_edge];
        if (next == kNoPoly || !filter.passes(polys_[next].area)) {
            reached = std::fmax(reached, tmax);
            if (reached > best_reached) {
                best_reached = reached;
                best_hit = at(tmax);
                const Vec3 &a = verts_[np.verts[exit_edge]];
                const Vec3 &b = verts_[np.verts[(exit_edge + 1) % np.count]];
                Vec3 e = b - a;
                float el = std::sqrt(e.x * e.x + e.z * e.z);
                best_normal = el > 1e-9f ? Vec3(-e.z / el, 0.0f, e.x / el)
                                         : Vec3();
            }
            break;
        }
        reached = std::fmax(reached, tmax);
        cur = next;
    }
    // Ran out of guard, or stopped at a wall. Either way this start
    // got as far as `reached`; keep the furthest of all the tries.
    if (reached > best_reached) {
        best_reached = reached;
        best_hit = at(reached);
        best_normal = Vec3();
    }
    }  // next start polygon

    if (hit) *hit = best_hit;
    if (normal) *normal = best_normal;
    return false;
}

// -------------------------------------------------------- persistence

std::vector<uint8_t> NavMesh::save() const {
    ByteWriter w;
    w.u32(kNavMagic);
    w.u32(kNavVersion);
    w.u32(uint32_t(verts_.size()));
    for (const Vec3 &v : verts_) {
        w.f32(v.x);
        w.f32(v.y);
        w.f32(v.z);
    }
    w.u32(uint32_t(polys_.size()));
    for (const NavPoly &p : polys_) {
        w.u8(p.count);
        w.u16(p.region);
        w.u16(p.area);
        for (int k = 0; k < p.count; ++k) {
            w.u16(p.verts[k]);
            w.u16(p.neis[k]);
        }
    }
    w.u32(uint32_t(links_.size()));
    for (const NavLink &l : links_) {
        for (const Vec3 *v : {&l.from, &l.to}) {
            w.f32(v->x);
            w.f32(v->y);
            w.f32(v->z);
        }
        w.f32(l.radius);
        w.f32(l.cost);
        w.u16(l.area);
        w.u8(l.bidirectional ? 1 : 0);
        w.str(l.name);
    }
    return w.bytes;
}

bool NavMesh::load(const std::vector<uint8_t> &data) {
    ByteReader r(data);
    if (r.u32() != kNavMagic) return false;
    uint32_t version = r.u32();
    if (version != kNavVersion) {
        WR_ERROR("nav: mesh is version %u, this build reads %u", version,
                 kNavVersion);
        return false;
    }
    verts_.clear();
    polys_.clear();
    links_.clear();

    uint32_t nv = r.u32();
    verts_.reserve(nv);
    for (uint32_t i = 0; i < nv && r.ok(); ++i) {
        Vec3 v;
        v.x = r.f32();
        v.y = r.f32();
        v.z = r.f32();
        verts_.push_back(v);
    }
    uint32_t np = r.u32();
    polys_.reserve(np);
    for (uint32_t i = 0; i < np && r.ok(); ++i) {
        NavPoly p;
        p.count = r.u8();
        p.region = r.u16();
        p.area = r.u16();
        if (p.count > kMaxVertsPerPoly) return false;
        for (int k = 0; k < kMaxVertsPerPoly; ++k) {
            p.verts[k] = 0;
            p.neis[k] = kNoPoly;
        }
        for (int k = 0; k < p.count; ++k) {
            p.verts[k] = r.u16();
            p.neis[k] = r.u16();
        }
        polys_.push_back(p);
    }
    uint32_t nl = r.u32();
    for (uint32_t i = 0; i < nl && r.ok(); ++i) {
        NavLink l;
        l.from.x = r.f32();
        l.from.y = r.f32();
        l.from.z = r.f32();
        l.to.x = r.f32();
        l.to.y = r.f32();
        l.to.z = r.f32();
        l.radius = r.f32();
        l.cost = r.f32();
        l.area = r.u16();
        l.bidirectional = r.u8() != 0;
        l.name = r.str();
        links_.push_back(l);
    }
    if (!r.ok()) return false;

    bounds_ = AABB();
    for (const Vec3 &v : verts_) bounds_.expand(v);
    build_grid();
    resolve_links();
    return true;
}

static void register_navmesh_class() {
    ClassBuilder<NavMesh>()
        .method("poly_count", &NavMesh::poly_count)
        .method("vert_count", &NavMesh::vert_count)
        .method("link_count", &NavMesh::link_count)
        .method("empty", &NavMesh::empty);
}
WR_REGISTER(register_navmesh_class)

}  // namespace wr::nav
