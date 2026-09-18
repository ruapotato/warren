// Warren -- contours into convex polygons.
//
// Three steps, and the middle one is pure economy.
//
//   Triangulate. Ear clipping on each contour, which always works
//   and always gives convex pieces, because a triangle cannot be
//   anything else.
//
//   Merge. A room that came out as forty triangles is a room a path
//   search walks forty nodes across. Glueing neighbours back
//   together while they stay convex turns it into six or seven. The
//   mesh means exactly the same thing afterwards; there is just far
//   less of it.
//
//   Match edges. Two polygons that share two vertices share an edge,
//   and an edge is the only place a path can cross between them.
//   This is the step that turns a pile of polygons into a graph.
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "nav/navmesh.h"

namespace wr::nav {

namespace {

constexpr uint16_t kNoVert = 0xffff;

struct IVert {
    int32_t x, y, z;
    bool operator==(const IVert &o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};
struct IVertHash {
    size_t operator()(const IVert &v) const {
        // Three smallish integers into one bucket. The constants are
        // odd and unrelated, which is all a hash of grid coordinates
        // needs to avoid piling every row into one bucket.
        size_t h = size_t(uint32_t(v.x)) * 73856093u;
        h ^= size_t(uint32_t(v.y)) * 19349663u;
        h ^= size_t(uint32_t(v.z)) * 83492791u;
        return h;
    }
};

int prev_i(int i, int n) { return i - 1 >= 0 ? i - 1 : n - 1; }
int next_i(int i, int n) { return i + 1 < n ? i + 1 : 0; }

int64_t tri_area2(const IVert &a, const IVert &b, const IVert &c) {
    return int64_t(b.x - a.x) * int64_t(c.z - a.z) -
           int64_t(c.x - a.x) * int64_t(b.z - a.z);
}
bool left(const IVert &a, const IVert &b, const IVert &c) {
    return tri_area2(a, b, c) < 0;
}
bool left_on(const IVert &a, const IVert &b, const IVert &c) {
    return tri_area2(a, b, c) <= 0;
}
bool collinear(const IVert &a, const IVert &b, const IVert &c) {
    return tri_area2(a, b, c) == 0;
}
bool vequal(const IVert &a, const IVert &b) { return a.x == b.x && a.z == b.z; }

// Do segments ab and cd cross, not merely touch?
bool intersect_proper(const IVert &a, const IVert &b, const IVert &c,
                      const IVert &d) {
    if (collinear(a, b, c) || collinear(a, b, d) || collinear(c, d, a) ||
        collinear(c, d, b))
        return false;
    return (left(a, b, c) != left(a, b, d)) && (left(c, d, a) != left(c, d, b));
}
// Is c on the closed segment ab, given the three are collinear?
bool between(const IVert &a, const IVert &b, const IVert &c) {
    if (!collinear(a, b, c)) return false;
    if (a.x != b.x)
        return ((a.x <= c.x) && (c.x <= b.x)) || ((a.x >= c.x) && (c.x >= b.x));
    return ((a.z <= c.z) && (c.z <= b.z)) || ((a.z >= c.z) && (c.z >= b.z));
}
bool intersect(const IVert &a, const IVert &b, const IVert &c, const IVert &d) {
    if (intersect_proper(a, b, c, d)) return true;
    return between(a, b, c) || between(a, b, d) || between(c, d, a) ||
           between(c, d, b);
}

// Is the segment from vertex i to vertex j a diagonal -- inside the
// polygon and crossing no edge? Ear clipping is nothing but this
// test applied over and over.
bool in_cone(int i, int j, int n, const std::vector<IVert> &v,
             const std::vector<int> &idx) {
    const IVert &a = v[size_t(idx[size_t(i)] & 0x0fffffff)];
    const IVert &b = v[size_t(idx[size_t(j)] & 0x0fffffff)];
    const IVert &a1 = v[size_t(idx[size_t(next_i(i, n))] & 0x0fffffff)];
    const IVert &a0 = v[size_t(idx[size_t(prev_i(i, n))] & 0x0fffffff)];
    if (left_on(a0, a, a1))  // a is convex
        return left(a, b, a0) && left(b, a, a1);
    // a is reflex: the cone is the complement
    return !(left_on(a, b, a1) && left_on(b, a, a0));
}
bool diagonal_open(int i, int j, int n, const std::vector<IVert> &v,
                   const std::vector<int> &idx) {
    const IVert &d0 = v[size_t(idx[size_t(i)] & 0x0fffffff)];
    const IVert &d1 = v[size_t(idx[size_t(j)] & 0x0fffffff)];
    for (int k = 0; k < n; ++k) {
        int k1 = next_i(k, n);
        if (k == i || k1 == i || k == j || k1 == j) continue;
        const IVert &p0 = v[size_t(idx[size_t(k)] & 0x0fffffff)];
        const IVert &p1 = v[size_t(idx[size_t(k1)] & 0x0fffffff)];
        if (vequal(d0, p0) || vequal(d1, p0) || vequal(d0, p1) ||
            vequal(d1, p1))
            continue;
        if (intersect(d0, d1, p0, p1)) return false;
    }
    return true;
}
bool diagonal(int i, int j, int n, const std::vector<IVert> &v,
              const std::vector<int> &idx) {
    return in_cone(i, j, n, v, idx) && diagonal_open(i, j, n, v, idx);
}

// Loosened versions, for the fallback below.
bool in_cone_loose(int i, int j, int n, const std::vector<IVert> &v,
                   const std::vector<int> &idx) {
    const IVert &a = v[size_t(idx[size_t(i)] & 0x0fffffff)];
    const IVert &b = v[size_t(idx[size_t(j)] & 0x0fffffff)];
    const IVert &a1 = v[size_t(idx[size_t(next_i(i, n))] & 0x0fffffff)];
    const IVert &a0 = v[size_t(idx[size_t(prev_i(i, n))] & 0x0fffffff)];
    if (left_on(a0, a, a1)) return left_on(a, b, a0) && left_on(b, a, a1);
    return !(left_on(a, b, a1) && left_on(b, a, a0));
}
bool diagonal_loose(int i, int j, int n, const std::vector<IVert> &v,
                    const std::vector<int> &idx) {
    if (!in_cone_loose(i, j, n, v, idx)) return false;
    const IVert &d0 = v[size_t(idx[size_t(i)] & 0x0fffffff)];
    const IVert &d1 = v[size_t(idx[size_t(j)] & 0x0fffffff)];
    for (int k = 0; k < n; ++k) {
        int k1 = next_i(k, n);
        if (k == i || k1 == i || k == j || k1 == j) continue;
        const IVert &p0 = v[size_t(idx[size_t(k)] & 0x0fffffff)];
        const IVert &p1 = v[size_t(idx[size_t(k1)] & 0x0fffffff)];
        if (vequal(d0, p0) || vequal(d1, p0) || vequal(d0, p1) ||
            vequal(d1, p1))
            continue;
        if (intersect_proper(d0, d1, p0, p1)) return false;
    }
    return true;
}

// EAR CLIPPING, shortest ear first. Taking the shortest available
// diagonal each time is what keeps the triangles fat: clipping in
// index order on a long thin contour produces a fan of slivers, and
// a sliver is a polygon whose interior is nowhere near its vertices.
int triangulate(int n, const std::vector<IVert> &verts, std::vector<int> &idx,
                std::vector<int> &tris) {
    tris.clear();
    for (int i = 0; i < n; ++i) {
        int i1 = next_i(i, n);
        int i2 = next_i(i1, n);
        if (diagonal(i, i2, n, verts, idx)) idx[size_t(i1)] |= 0x40000000;
    }

    while (n > 3) {
        int64_t min_len = -1;
        int mini = -1;
        for (int i = 0; i < n; ++i) {
            int i1 = next_i(i, n);
            if (!(idx[size_t(i1)] & 0x40000000)) continue;
            const IVert &p0 = verts[size_t(idx[size_t(i)] & 0x0fffffff)];
            const IVert &p2 =
                verts[size_t(idx[size_t(next_i(i1, n))] & 0x0fffffff)];
            int64_t dx = p2.x - p0.x, dz = p2.z - p0.z;
            int64_t len = dx * dx + dz * dz;
            if (min_len < 0 || len < min_len) {
                min_len = len;
                mini = i;
            }
        }
        if (mini == -1) {
            // No ear by the strict test. This happens on contours
            // with coincident vertices -- which is exactly what a
            // hole bridge is, two edges lying on top of each other.
            // Retry allowing the degenerate touches the strict test
            // rejects.
            for (int i = 0; i < n; ++i) {
                int i1 = next_i(i, n);
                int i2 = next_i(i1, n);
                if (!diagonal_loose(i, i2, n, verts, idx)) continue;
                const IVert &p0 = verts[size_t(idx[size_t(i)] & 0x0fffffff)];
                const IVert &p2 = verts[size_t(idx[size_t(i2)] & 0x0fffffff)];
                int64_t dx = p2.x - p0.x, dz = p2.z - p0.z;
                int64_t len = dx * dx + dz * dz;
                if (min_len < 0 || len < min_len) {
                    min_len = len;
                    mini = i;
                }
            }
            if (mini == -1) return -int(tris.size() / 3);
        }

        int i = mini, i1 = next_i(i, n), i2 = next_i(i1, n);
        tris.push_back(idx[size_t(i)] & 0x0fffffff);
        tris.push_back(idx[size_t(i1)] & 0x0fffffff);
        tris.push_back(idx[size_t(i2)] & 0x0fffffff);

        --n;
        for (int k = i1; k < n; ++k) idx[size_t(k)] = idx[size_t(k + 1)];
        if (i1 >= n) i1 = 0;
        i = prev_i(i1, n);
        if (diagonal(prev_i(i, n), i1, n, verts, idx))
            idx[size_t(i)] |= 0x40000000;
        else
            idx[size_t(i)] &= 0x0fffffff;
        if (diagonal(i, next_i(i1, n), n, verts, idx))
            idx[size_t(i1)] |= 0x40000000;
        else
            idx[size_t(i1)] &= 0x0fffffff;
    }
    tris.push_back(idx[0] & 0x0fffffff);
    tris.push_back(idx[1] & 0x0fffffff);
    tris.push_back(idx[2] & 0x0fffffff);
    return int(tris.size() / 3);
}

int poly_verts(const uint16_t *p) {
    for (int i = 0; i < kMaxVertsPerPoly; ++i)
        if (p[i] == kNoVert) return i;
    return kMaxVertsPerPoly;
}

bool turns_left(const IVert &a, const IVert &b, const IVert &c) {
    return tri_area2(a, b, c) < 0;
}

// Would merging these two polygons leave a convex one, and if so,
// how good a merge is it? The score is the shared edge's squared
// length, so long shared edges go first -- merging across the long
// seam of two halves of a room beats nibbling at a corner.
int64_t merge_value(const uint16_t *pa, const uint16_t *pb,
                    const std::vector<IVert> &verts, int *ea, int *eb) {
    const int na = poly_verts(pa), nb = poly_verts(pb);
    if (na + nb - 4 > kMaxVertsPerPoly) return -1;

    *ea = -1;
    *eb = -1;
    for (int i = 0; i < na; ++i) {
        uint16_t a0 = pa[i], a1 = pa[(i + 1) % na];
        if (a0 > a1) std::swap(a0, a1);
        for (int j = 0; j < nb; ++j) {
            uint16_t b0 = pb[j], b1 = pb[(j + 1) % nb];
            if (b0 > b1) std::swap(b0, b1);
            if (a0 == b0 && a1 == b1) {
                *ea = i;
                *eb = j;
            }
        }
    }
    if (*ea == -1 || *eb == -1) return -1;

    // The two vertices where the merged outline turns must still
    // turn the same way, or the result is a bow tie.
    uint16_t va = pa[(*ea + na - 1) % na];
    uint16_t vb = pa[*ea];
    uint16_t vc = pb[(*eb + 2) % nb];
    if (!turns_left(verts[va], verts[vb], verts[vc])) return -1;
    va = pb[(*eb + nb - 1) % nb];
    vb = pb[*eb];
    vc = pa[(*ea + 2) % na];
    if (!turns_left(verts[va], verts[vb], verts[vc])) return -1;

    va = pa[*ea];
    vb = pa[(*ea + 1) % na];
    int64_t dx = verts[va].x - verts[vb].x;
    int64_t dz = verts[va].z - verts[vb].z;
    return dx * dx + dz * dz;
}

void merge_into(uint16_t *out, const uint16_t *pa, const uint16_t *pb, int ea,
                int eb) {
    const int na = poly_verts(pa), nb = poly_verts(pb);
    uint16_t tmp[kMaxVertsPerPoly * 2];
    int n = 0;
    // Round `pa` from just past the shared edge back to it, then the
    // same for `pb`. The shared edge's two vertices each appear once.
    for (int i = 0; i < na - 1; ++i) tmp[n++] = pa[(ea + 1 + i) % na];
    for (int i = 0; i < nb - 1; ++i) tmp[n++] = pb[(eb + 1 + i) % nb];
    for (int i = 0; i < kMaxVertsPerPoly; ++i)
        out[i] = i < n ? tmp[i] : kNoVert;
}

}  // namespace

bool build_poly_mesh(const ContourSet &contours, std::vector<Vec3> *out_verts,
                     std::vector<NavPoly> *out_polys) {
    if (!out_verts || !out_polys) return false;
    out_verts->clear();
    out_polys->clear();

    std::vector<IVert> verts;
    std::unordered_map<IVert, uint16_t, IVertHash> lookup;
    auto intern = [&](const ContourVert &c) -> uint16_t {
        IVert v{c.x, c.y, c.z};
        auto it = lookup.find(v);
        if (it != lookup.end()) return it->second;
        uint16_t id = uint16_t(verts.size());
        verts.push_back(v);
        lookup.emplace(v, id);
        return id;
    };

    struct BuiltPoly {
        uint16_t v[kMaxVertsPerPoly];
        uint16_t region;
    };
    std::vector<BuiltPoly> built;

    std::vector<int> idx, tris;
    std::vector<IVert> local;
    std::vector<uint16_t> local_global;

    for (const Contour &c : contours.contours) {
        const int n = int(c.verts.size());
        if (n < 3) continue;

        // The ear clipper wants one winding; normalise to it. The
        // region recorded on a vertex belongs to the edge LEAVING
        // it, so reversing the points has to shift those along by
        // one -- forget that and every polygon ends up believing its
        // neighbour is the one on the far side of a different edge.
        std::vector<ContourVert> poly(c.verts.begin(), c.verts.end());
        int64_t a2 = 0;
        for (int i = 0, j = n - 1; i < n; j = i++)
            a2 += int64_t(poly[size_t(j)].x) * int64_t(poly[size_t(i)].z) -
                  int64_t(poly[size_t(i)].x) * int64_t(poly[size_t(j)].z);
        if (a2 > 0) {
            // Reversing sends position i to n-1-i, so the edge
            // leaving the new vertex i is the old edge that ARRIVED
            // at old vertex n-1-i -- that is, the edge leaving old
            // vertex n-2-i.
            std::vector<uint16_t> regs(size_t(n), kNoRegion);
            for (int i = 0; i < n; ++i)
                regs[size_t(i)] = poly[size_t((n - 2 - i + n) % n)].region;
            std::reverse(poly.begin(), poly.end());
            for (int i = 0; i < n; ++i) poly[size_t(i)].region = regs[size_t(i)];
        }

        local.clear();
        local_global.clear();
        idx.clear();
        for (int i = 0; i < n; ++i) {
            local.push_back(IVert{poly[size_t(i)].x, poly[size_t(i)].y,
                                  poly[size_t(i)].z});
            local_global.push_back(intern(poly[size_t(i)]));
            idx.push_back(i);
        }

        int ntris = triangulate(n, local, idx, tris);
        if (ntris <= 0) {
            WR_WARN("nav: region %u would not triangulate (%d of %d verts)",
                    unsigned(c.region), -ntris, n);
            if (ntris == 0) continue;
            ntris = -ntris;
        }

        // Start as triangles, in the shared vertex numbering.
        std::vector<BuiltPoly> parts;
        parts.reserve(size_t(ntris));
        for (int t = 0; t < ntris; ++t) {
            BuiltPoly p;
            for (int k = 0; k < kMaxVertsPerPoly; ++k) p.v[k] = kNoVert;
            p.v[0] = local_global[size_t(tris[size_t(t * 3 + 0)])];
            p.v[1] = local_global[size_t(tris[size_t(t * 3 + 1)])];
            p.v[2] = local_global[size_t(tris[size_t(t * 3 + 2)])];
            p.region = c.region;
            // A triangle whose three corners interned to fewer than
            // three vertices has no area; it comes from a bridge.
            if (p.v[0] == p.v[1] || p.v[1] == p.v[2] || p.v[2] == p.v[0])
                continue;
            parts.push_back(p);
        }

        // Greedy merge, best pair first, until nothing more is
        // convex. Quadratic in the pieces of ONE region, which is
        // tens, not in the mesh.
        for (;;) {
            int64_t best = -1;
            size_t bi = 0, bj = 0;
            int bea = 0, beb = 0;
            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                for (size_t j = i + 1; j < parts.size(); ++j) {
                    int ea = 0, eb = 0;
                    int64_t v = merge_value(parts[i].v, parts[j].v, verts, &ea,
                                            &eb);
                    if (v <= best) continue;
                    best = v;
                    bi = i;
                    bj = j;
                    bea = ea;
                    beb = eb;
                }
            }
            if (best < 0) break;
            uint16_t merged[kMaxVertsPerPoly];
            merge_into(merged, parts[bi].v, parts[bj].v, bea, beb);
            std::memcpy(parts[bi].v, merged, sizeof(merged));
            parts.erase(parts.begin() + long(bj));
        }
        for (const BuiltPoly &p : parts) built.push_back(p);
    }

    if (built.size() > 0xfffe) {
        WR_ERROR("nav: %zu polygons is more than the index space holds; "
                 "bake in tiles or use a larger cell size",
                 built.size());
        return false;
    }

    // ------------------------------------------------ edge matching
    //
    // Every edge, keyed by its two vertices in sorted order. Two
    // polygons that produced the same key share that edge, and each
    // records the other. An edge that shows up once is a wall; an
    // edge that shows up three times means the contours overlap,
    // which is a bug worth hearing about rather than one to route
    // paths through.
    struct EdgeRef {
        uint16_t poly = kNoPoly;
        uint8_t edge = 0;
        int count = 0;
    };
    std::unordered_map<uint32_t, EdgeRef> edges;
    edges.reserve(built.size() * 4);

    out_polys->resize(built.size());
    for (size_t i = 0; i < built.size(); ++i) {
        NavPoly &np = (*out_polys)[i];
        int n = poly_verts(built[i].v);
        np.count = uint8_t(n);
        np.region = built[i].region;
        np.area = 1;
        for (int k = 0; k < kMaxVertsPerPoly; ++k) {
            np.verts[k] = built[i].v[k];
            np.neis[k] = kNoPoly;
        }
    }

    int overlaps = 0;
    for (size_t i = 0; i < built.size(); ++i) {
        NavPoly &np = (*out_polys)[i];
        for (int k = 0; k < np.count; ++k) {
            uint16_t a = np.verts[k];
            uint16_t b = np.verts[(k + 1) % np.count];
            uint32_t key = a < b ? (uint32_t(a) << 16 | b)
                                 : (uint32_t(b) << 16 | a);
            EdgeRef &e = edges[key];
            ++e.count;
            if (e.count == 1) {
                e.poly = uint16_t(i);
                e.edge = uint8_t(k);
            } else if (e.count == 2) {
                np.neis[k] = e.poly;
                (*out_polys)[e.poly].neis[e.edge] = uint16_t(i);
            } else {
                ++overlaps;
            }
        }
    }
    if (overlaps)
        WR_WARN("nav: %d edges are shared by more than two polygons; "
                "the contours overlap somewhere",
                overlaps);

    // Cell coordinates into world space, last, so that everything
    // above could compare vertices for exact equality.
    out_verts->resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        (*out_verts)[i] =
            Vec3(contours.bounds.min.x + float(verts[i].x) * contours.cell_size,
                 contours.bounds.min.y + float(verts[i].y) * contours.cell_height,
                 contours.bounds.min.z + float(verts[i].z) * contours.cell_size);
    }
    return true;
}

}  // namespace wr::nav
