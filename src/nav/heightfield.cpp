#include "nav/heightfield.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wr::nav {

namespace {

constexpr uint16_t kMaxHeightCells = 0xfff0;
constexpr int kSpanMax = int(kMaxHeightCells);

// CLIP A CONVEX POLYGON against the axis-aligned plane `axis == v`,
// putting the below-or-on part in `lo` and the above-or-on part in
// `hi`. Both halves keep the crossing points, so the two outputs
// share an edge exactly and a vertex that lands on the plane is not
// duplicated into a sliver.
//
// This is the whole of triangle rasterisation. Clip to a row of z,
// clip that to a column of x, and whatever survives is the part of
// the triangle inside one cell -- its y range is the span.
void split(const Vec3 *in, int n, Vec3 *lo, int *nlo, Vec3 *hi, int *nhi,
           float v, int axis) {
    float d[12];
    for (int i = 0; i < n; ++i) d[i] = v - in[i][axis];

    int a = 0, b = 0;
    for (int i = 0, j = n - 1; i < n; j = i, ++i) {
        // An edge crosses when the two ends sit on opposite sides.
        // Endpoints exactly on the plane count as on both sides,
        // which is what keeps the halves watertight.
        bool crosses = (d[i] >= 0.0f) != (d[j] >= 0.0f);
        if (crosses) {
            float t = d[j] / (d[j] - d[i]);
            Vec3 p = in[j] + (in[i] - in[j]) * t;
            lo[a++] = p;
            hi[b++] = p;
        }
        if (d[i] > 0.0f) {
            lo[a++] = in[i];
        } else if (d[i] < 0.0f) {
            hi[b++] = in[i];
        } else {
            // On the plane: belongs to both, but only once each, and
            // the crossing branch above has not already added it
            // (a zero d[i] makes `crosses` false on this edge).
            lo[a++] = in[i];
            hi[b++] = in[i];
        }
    }
    *nlo = a;
    *nhi = b;
}

}  // namespace

// ------------------------------------------------------------- rasterise

void Heightfield::add_span(int x, int z, uint16_t bottom, uint16_t top,
                           bool slope_ok) {
    std::vector<Span> &col = columns_[size_t(z) * size_t(width_) + size_t(x)];

    // Absorb every existing span this one touches or overlaps. The
    // survivor keeps the highest top, and takes its slope flag from
    // whoever owns that top -- the surface a body stands on is the
    // top one, so a non-walkable span merging in from below (the
    // underside of a railing, say) must not clear the floor's flag.
    size_t out = 0;
    for (size_t i = 0; i < col.size(); ++i) {
        const Span &s = col[i];
        if (s.top < bottom || s.bottom > top) {
            col[out++] = s;  // disjoint, keep as-is
            continue;
        }
        if (s.bottom < bottom) bottom = s.bottom;
        if (s.top > top) {
            top = s.top;
            slope_ok = s.slope_ok;
        } else if (s.top == top) {
            slope_ok = slope_ok || s.slope_ok;
        }
    }
    col.resize(out);

    Span merged;
    merged.bottom = bottom;
    merged.top = top;
    merged.slope_ok = slope_ok;
    auto at = std::lower_bound(
        col.begin(), col.end(), merged,
        [](const Span &a, const Span &b) { return a.bottom < b.bottom; });
    col.insert(at, merged);
}

bool Heightfield::build(const std::vector<Vec3> &triangles, const AABB &bounds,
                        float cell_size, float cell_height,
                        float max_slope_degrees) {
    if (cell_size <= 0.0f || cell_height <= 0.0f) return false;
    if (!bounds.valid()) return false;

    bounds_ = bounds;
    cell_size_ = cell_size;
    cell_height_ = cell_height;

    Vec3 span = bounds.size();
    width_ = int(std::ceil(span.x / cell_size));
    depth_ = int(std::ceil(span.z / cell_size));
    height_ = int(std::ceil(span.y / cell_height)) + 1;
    if (width_ <= 0 || depth_ <= 0) return false;
    if (height_ > kSpanMax) return false;

    columns_.assign(size_t(width_) * size_t(depth_), {});

    const float walkable_cos = std::cos(max_slope_degrees * DEG);
    const float inv_cs = 1.0f / cell_size;
    const float inv_ch = 1.0f / cell_height;

    // Buffers for the clip cascade. Seven vertices is the most a
    // triangle can have after two axis clips; twelve is slack.
    Vec3 buf[4][12];

    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const Vec3 &v0 = triangles[t];
        const Vec3 &v1 = triangles[t + 1];
        const Vec3 &v2 = triangles[t + 2];

        Vec3 n = cross(v1 - v0, v2 - v0);
        float nl = n.length();
        if (nl < EPS) continue;  // degenerate, contributes nothing
        bool slope_ok = (n.y / nl) > walkable_cos;

        AABB tri;
        tri.expand(v0);
        tri.expand(v1);
        tri.expand(v2);
        if (!tri.intersects(bounds)) continue;

        int z0 = int(std::floor((tri.min.z - bounds.min.z) * inv_cs));
        int z1 = int(std::floor((tri.max.z - bounds.min.z) * inv_cs));
        z0 = std::max(z0, 0);
        z1 = std::min(z1, depth_ - 1);
        if (z0 > z1) continue;

        int nin = 3;
        buf[0][0] = v0;
        buf[0][1] = v1;
        buf[0][2] = v2;

        for (int z = z0; z <= z1; ++z) {
            // Slice off the row [z, z+1) in world z.
            float row_max = bounds.min.z + float(z + 1) * cell_size;
            int n_row = 0, n_rest = 0;
            split(buf[0], nin, buf[1], &n_row, buf[2], &n_rest, row_max, 2);
            // buf[2] is everything above this row; it becomes the
            // input to the next iteration, so the triangle shrinks
            // as the sweep advances rather than being re-clipped
            // from scratch.
            std::memcpy(buf[0], buf[2], size_t(n_rest) * sizeof(Vec3));
            nin = n_rest;
            if (n_row < 3) continue;

            float xmin = buf[1][0].x, xmax = buf[1][0].x;
            for (int i = 1; i < n_row; ++i) {
                xmin = std::min(xmin, buf[1][i].x);
                xmax = std::max(xmax, buf[1][i].x);
            }
            int x0 = int(std::floor((xmin - bounds.min.x) * inv_cs));
            int x1 = int(std::floor((xmax - bounds.min.x) * inv_cs));
            x0 = std::max(x0, 0);
            x1 = std::min(x1, width_ - 1);
            if (x0 > x1) continue;

            int n_cur = n_row;
            std::memcpy(buf[3], buf[1], size_t(n_row) * sizeof(Vec3));

            for (int x = x0; x <= x1; ++x) {
                float col_max = bounds.min.x + float(x + 1) * cell_size;
                int n_cell = 0, n_keep = 0;
                split(buf[3], n_cur, buf[1], &n_cell, buf[2], &n_keep, col_max,
                      0);
                std::memcpy(buf[3], buf[2], size_t(n_keep) * sizeof(Vec3));
                n_cur = n_keep;
                if (n_cell < 3) continue;

                // A triangle whose edge lands exactly on a cell
                // boundary leaves a zero-area strip on the far side
                // of the cut -- three or more vertices, all
                // collinear. Rasterising it puts a phantom floor in
                // the next cell out, which is how a navmesh comes to
                // extend a quarter of a cell past the world.
                //
                // Measured relative to the first vertex, so the test
                // is on the polygon's own size and not on how far
                // from the origin it happens to sit. A vertical wall
                // still has area here, which it would not if this
                // measured the footprint.
                {
                    Vec3 a0 = buf[1][0], nsum;
                    for (int i = 1; i + 1 < n_cell; ++i)
                        nsum = nsum + cross(buf[1][i] - a0, buf[1][i + 1] - a0);
                    if (nsum.length() < 1e-6f * cell_size * cell_size) continue;
                }

                float ymin = buf[1][0].y, ymax = buf[1][0].y;
                for (int i = 1; i < n_cell; ++i) {
                    ymin = std::min(ymin, buf[1][i].y);
                    ymax = std::max(ymax, buf[1][i].y);
                }
                ymin -= bounds.min.y;
                ymax -= bounds.min.y;
                if (ymax < 0.0f) continue;
                if (ymin > span.y) continue;
                ymin = std::max(ymin, 0.0f);
                ymax = std::min(ymax, span.y);

                int bot = int(std::floor(ymin * inv_ch));
                int top = int(std::ceil(ymax * inv_ch));
                bot = std::clamp(bot, 0, kSpanMax);
                top = std::clamp(top, 0, kSpanMax);
                // A flat floor clips to zero thickness; give it one
                // cell, or there is nothing to stand on.
                if (top <= bot) top = bot + 1;
                add_span(x, z, uint16_t(bot), uint16_t(top), slope_ok);
            }
        }
    }
    return true;
}

size_t Heightfield::span_count() const {
    size_t n = 0;
    for (const auto &c : columns_) n += c.size();
    return n;
}

// --------------------------------------------------------------- compact

bool CompactField::build(const Heightfield &hf, const AgentSpec &agent) {
    width_ = hf.width();
    depth_ = hf.depth();
    cell_size_ = hf.cell_size();
    cell_height_ = hf.cell_height();
    bounds_ = hf.bounds();
    region_count_ = 0;
    max_dist_ = 0;
    if (width_ <= 0 || depth_ <= 0) return false;

    // QUANTISING THE BODY, with a nudge, and the nudge is not
    // fussiness. A 0.45 m climb in 0.15 m cells is exactly three
    // cells in arithmetic and 2.9999998 in float, so a bare floor()
    // hands back a body that can climb 0.30 m. That body cannot use
    // stairs, and the failure surfaces as "the zombies won't go
    // upstairs" rather than as anything about rounding.
    const int height_cells = std::max(
        1, int(std::ceil(agent.height / cell_height_ - 1e-3f)));
    const int climb_cells =
        int(std::floor(agent.max_climb / cell_height_ + 1e-3f));

    // A surface is a candidate if it is not too steep and has the
    // headroom. Whether a body can GET there is the ledge pass, and
    // that needs to see the neighbouring columns.
    auto floor_of = [](const std::vector<Span> &col, size_t i) {
        return int(col[i].top);
    };
    auto ceil_of = [&](const std::vector<Span> &col, size_t i) {
        return i + 1 < col.size() ? int(col[i + 1].bottom) : kSpanMax;
    };

    // IS THIS A LEDGE? The rule is less obvious than "is there a drop
    // next to me", because the interesting cases are the two that
    // look the same from the triangle:
    //
    //   Next to a wall, the neighbouring column's surface is metres
    //   ABOVE this one and there is no gap to step through. Nothing
    //   is reachable that way, and that is fine -- a floor beside a
    //   wall is not a ledge, it is a floor.
    //
    //   On a roof edge, the neighbouring column's surface is metres
    //   BELOW and wide open. That is a ledge, and a body steered onto
    //   it walks off the building.
    //
    // So the test is on the DROP to whatever is reachable, not on
    // whether anything is reachable. Two ways to fail: some neighbour
    // is further down than one step (a cliff), or the reachable
    // neighbours disagree with each other by more than one step,
    // which means this cell straddles two levels and a body standing
    // on it is standing on neither.
    auto is_ledge = [&](int x, int z, const std::vector<Span> &col, size_t i) {
        const int bot = floor_of(col, i);
        const int top = ceil_of(col, i);
        int min_drop = std::numeric_limits<int>::max();
        int reach_lo = bot, reach_hi = bot;

        for (int dir = 0; dir < 4; ++dir) {
            int nx = x + kDirX[dir], nz = z + kDirZ[dir];
            if (!hf.inside(nx, nz)) {
                // Off the edge of the bake is an unbounded drop. A
                // navmesh must not run to the boundary of its own
                // volume, or bodies walk into the void at the seam.
                min_drop = std::min(min_drop, -climb_cells - 1 - bot);
                continue;
            }
            const std::vector<Span> &nc = hf.column(nx, nz);

            // The open space UNDER the neighbour's lowest span. This
            // is the hole case: a neighbouring column whose first
            // solid starts above us is a pit, not a wall.
            {
                int nbot = -climb_cells - 1;
                int ntop = nc.empty() ? kSpanMax : int(nc[0].bottom);
                if (std::min(top, ntop) - std::max(bot, nbot) >= height_cells)
                    min_drop = std::min(min_drop, nbot - bot);
            }
            for (size_t j = 0; j < nc.size(); ++j) {
                int nbot = floor_of(nc, j);
                int ntop = ceil_of(nc, j);
                if (std::min(top, ntop) - std::max(bot, nbot) < height_cells)
                    continue;
                min_drop = std::min(min_drop, nbot - bot);
                if (std::abs(nbot - bot) <= climb_cells) {
                    reach_lo = std::min(reach_lo, nbot);
                    reach_hi = std::max(reach_hi, nbot);
                }
            }
        }
        if (min_drop != std::numeric_limits<int>::max() &&
            min_drop < -climb_cells)
            return true;
        return (reach_hi - reach_lo) > climb_cells;
    };

    cells_.assign(size_t(width_) * size_t(depth_), {});
    spans_.clear();

    for (int z = 0; z < depth_; ++z) {
        for (int x = 0; x < width_; ++x) {
            const std::vector<Span> &col = hf.column(x, z);
            Cell &c = cells_[size_t(z) * size_t(width_) + size_t(x)];
            c.start = uint32_t(spans_.size());
            c.count = 0;
            for (size_t i = 0; i < col.size(); ++i) {
                if (!col[i].slope_ok) continue;
                int bot = floor_of(col, i);
                int gap = ceil_of(col, i) - bot;
                if (gap < height_cells) continue;
                if (is_ledge(x, z, col, i)) continue;
                CompactSpan s;
                s.y = uint16_t(std::min(bot, kSpanMax));
                s.clearance = uint16_t(std::min(gap, kSpanMax));
                spans_.push_back(s);
                ++c.count;
            }
        }
    }

    height_cells_ = height_cells;
    climb_cells_ = climb_cells;
    link_neighbours();
    return true;
}

void CompactField::link_neighbours() {
    for (int z = 0; z < depth_; ++z) {
        for (int x = 0; x < width_; ++x) {
            const Cell &c = cell(x, z);
            for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                CompactSpan &s = spans_[i];
                for (int dir = 0; dir < 4; ++dir) {
                    s.neighbour[dir] = kNoNeighbour;
                    int nx = x + kDirX[dir], nz = z + kDirZ[dir];
                    if (!inside(nx, nz)) continue;
                    const Cell &nc = cell(nx, nz);
                    for (uint32_t j = nc.start; j < nc.start + nc.count; ++j) {
                        const CompactSpan &ns = spans_[j];
                        // Reachable means both: the step is small
                        // enough, and the body fits through the gap
                        // the two surfaces share.
                        int lo = std::max(int(s.y), int(ns.y));
                        int hi = std::min(int(s.y) + int(s.clearance),
                                          int(ns.y) + int(ns.clearance));
                        if (hi - lo < height_cells_) continue;
                        if (std::abs(int(ns.y) - int(s.y)) > climb_cells_)
                            continue;
                        s.neighbour[dir] = int32_t(j);
                        break;
                    }
                }
            }
        }
    }
}

void CompactField::rebuild(const std::vector<bool> &keep) {
    std::vector<Cell> cells(size_t(width_) * size_t(depth_));
    std::vector<CompactSpan> spans;
    spans.reserve(spans_.size());

    for (int z = 0; z < depth_; ++z) {
        for (int x = 0; x < width_; ++x) {
            const Cell &old = cell(x, z);
            Cell &c = cells[size_t(z) * size_t(width_) + size_t(x)];
            c.start = uint32_t(spans.size());
            c.count = 0;
            for (uint32_t i = old.start; i < old.start + old.count; ++i) {
                if (!keep[i]) continue;
                CompactSpan s = spans_[i];
                s.dist = 0;
                s.region = kNoRegion;
                spans.push_back(s);
                ++c.count;
            }
        }
    }
    cells_.swap(cells);
    spans_.swap(spans);
    max_dist_ = 0;
    region_count_ = 0;
    link_neighbours();
}

// --------------------------------------------------------- distance field

void CompactField::build_distance_field() {
    const size_t n = spans_.size();
    if (n == 0) {
        max_dist_ = 0;
        return;
    }
    std::vector<uint16_t> d(n, 0xffff);

    // A span missing any of its four neighbours is on the border, and
    // the border is distance zero. Everything else is measured from
    // there.
    for (size_t i = 0; i < n; ++i) {
        const CompactSpan &s = spans_[i];
        bool border = false;
        for (int dir = 0; dir < 4; ++dir)
            if (s.neighbour[dir] == kNoNeighbour) border = true;
        if (border) d[i] = 0;
    }

    // Chamfer 2-3: a straight step costs 2 and a diagonal 3, which
    // approximates Euclidean distance to about 4% -- good enough to
    // erode by, and far cheaper than the true transform. Distances
    // come out in half-cells, so one cell is 2.
    //
    // A diagonal is reached as two straight links, which is also how
    // the walk checks the diagonal is actually open: cutting a corner
    // between two walls is not a step a body can take.
    auto diag = [&](size_t i, int32_t via, int dir) {
        if (via == kNoNeighbour) return;
        int32_t k = spans_[size_t(via)].neighbour[dir];
        if (k == kNoNeighbour) return;
        if (uint32_t(d[size_t(k)]) + 3 < d[i]) d[i] = uint16_t(d[size_t(k)] + 3);
    };
    auto straight = [&](size_t i, int32_t j) {
        if (j == kNoNeighbour) return;
        if (uint32_t(d[size_t(j)]) + 2 < d[i]) d[i] = uint16_t(d[size_t(j)] + 2);
    };

    for (int z = 0; z < depth_; ++z) {
        for (int x = 0; x < width_; ++x) {
            const Cell &c = cell(x, z);
            for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                const CompactSpan &s = spans_[i];
                straight(i, s.neighbour[0]);
                diag(i, s.neighbour[0], 3);  // (-1,-1)
                straight(i, s.neighbour[3]);
                diag(i, s.neighbour[3], 2);  // (+1,-1)
            }
        }
    }
    for (int z = depth_ - 1; z >= 0; --z) {
        for (int x = width_ - 1; x >= 0; --x) {
            const Cell &c = cell(x, z);
            for (uint32_t i = c.start + c.count; i-- > c.start;) {
                const CompactSpan &s = spans_[i];
                straight(i, s.neighbour[2]);
                diag(i, s.neighbour[2], 1);  // (+1,+1)
                straight(i, s.neighbour[1]);
                diag(i, s.neighbour[1], 0);  // (-1,+1)
            }
        }
    }

    max_dist_ = 0;
    for (size_t i = 0; i < n; ++i) {
        spans_[i].dist = d[i];
        max_dist_ = std::max(max_dist_, d[i]);
    }
}

void CompactField::erode(float radius) {
    if (radius <= 0.0f) return;
    build_distance_field();
    // dist is in half-cells, so the threshold doubles.
    const uint16_t limit = uint16_t(std::lround(radius / cell_size_ * 2.0f));
    std::vector<bool> keep(spans_.size());
    for (size_t i = 0; i < spans_.size(); ++i) keep[i] = spans_[i].dist >= limit;
    rebuild(keep);
}

}  // namespace wr::nav
