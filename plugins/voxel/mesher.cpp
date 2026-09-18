#include "mesher.h"

#include <algorithm>
#include <algorithm>
#include <cstring>

namespace mf::voxel {

Palette::Palette() {
    for (int i = 0; i < 256; i++) {
        colours[i] = Color::hex(0xB0B0B0);
        roughness[i] = 0.9f;
    }
    colours[0] = Color::clear();
    colours[1] = Color::hex(0x5E8C3A);  // grass
    colours[2] = Color::hex(0x6B5334);  // dirt
    colours[3] = Color::hex(0x8A8D91);  // rock
    colours[4] = Color::hex(0xD9C89A);  // sand
    colours[5] = Color::hex(0xF2F5F7);  // snow
    roughness[1] = 0.95f;
    roughness[2] = 0.98f;
    roughness[3] = 0.75f;
    roughness[4] = 0.9f;
    roughness[5] = 0.6f;
}

Palette &Palette::instance() {
    static Palette p;
    return p;
}

namespace {

// A symmetric 3x3 least-squares solve, regularised.
//
// The system is often rank deficient -- on a flat wall every normal
// points the same way, so two of the three directions are
// unconstrained -- and inverting it directly gives infinities. Adding
// a small multiple of the identity, and biasing towards the average
// of the crossing points, makes the unconstrained directions resolve
// to "the middle of the surface", which is exactly what is wanted.
Vec3 solve_qef(const float ata[6], const Vec3 &atb, const Vec3 &mass_point,
               float regularisation) {
    // A' = A + r*I,  b' = b + r*mass
    const float r = regularisation;
    float a00 = ata[0] + r, a01 = ata[1], a02 = ata[2];
    float a11 = ata[3] + r, a12 = ata[4], a22 = ata[5] + r;
    Vec3 b = atb + mass_point * r;

    // Cofactor inverse; the regularisation guarantees it exists.
    const float c00 = a11 * a22 - a12 * a12;
    const float c01 = a02 * a12 - a01 * a22;
    const float c02 = a01 * a12 - a02 * a11;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-12f) return mass_point;
    const float inv = 1.0f / det;
    const float c11 = a00 * a22 - a02 * a02;
    const float c12 = a02 * a01 - a00 * a12;
    const float c22 = a00 * a11 - a01 * a01;
    return {(c00 * b.x + c01 * b.y + c02 * b.z) * inv,
            (c01 * b.x + c11 * b.y + c12 * b.z) * inv,
            (c02 * b.x + c12 * b.y + c22 * b.z) * inv};
}

// The twelve edges of a cell, as pairs of its eight corners. Corner i
// has offset (i & 1, (i >> 1) & 1, (i >> 2) & 1).
const int kEdgeCorners[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},   // along x
                                 {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along y
                                 {0, 4}, {1, 5}, {2, 6}, {3, 7}};  // along z

}  // namespace

void mesh_chunk(const MeshRequest &req, MeshResult *out) {
    out->vertices.clear();
    out->indices.clear();
    out->bounds = AABB();
    out->empty = true;
    out->surface_cells = 0;
    out->samples = 0;
    if (!req.density || req.resolution <= 0) return;

    const int res = req.resolution;
    const float cs = req.cell_size;

    // ONE BORDER CELL ON EVERY SIDE.
    //
    // A quad is shared by the four cells around one edge, so a chunk
    // that only knows its own cells cannot close the face on its
    // boundary -- and every chunk leaves a one-cell crack around
    // itself. Sampling one cell into each neighbour costs about a
    // fifth more samples at 32 cells and makes the result watertight.
    const int cells = res + 1;      // cell indices -1 .. res-1
    const int corners = res + 2;    // corner indices -1 .. res
    const int cs3 = corners * corners * corners;

    // Two arguments, not one: `std::vector<float> field(size_t(cs3));`
    // is parsed as a function declaration, and everything after it
    // then fails in a way that does not mention the real cause.
    // A CHEAP PROBE BEFORE THE EXPENSIVE ONE.
    //
    // Most chunks are solid rock or open sky, and discovering that
    // by sampling all thirty-odd thousand corners is the single
    // biggest waste in the system. A 5x5x5 probe is 125 samples --
    // a third of a percent of the work -- and if every one of them
    // agrees on the sign and the nearest is further away than the
    // probe's own spacing could hide a surface within, there is
    // nothing here.
    {
        const float span = float(res) * cs;
        const float probe_step = span / 4.0f;
        // A surface can only hide between probes if the field gets
        // closer to zero than half the diagonal of a probe cell.
        const float safe = probe_step * 0.87f;
        bool sign_first = false, mixed = false;
        float nearest = 1e30f;
        for (int k = 0; k <= 4 && !mixed; k++)
            for (int j = 0; j <= 4 && !mixed; j++)
                for (int i = 0; i <= 4; i++) {
                    Vec3 p = req.origin + Vec3(float(i), float(j), float(k)) *
                                              (span / 4.0f);
                    float d = req.density->sample(p).distance;
                    nearest = std::min(nearest, std::fabs(d));
                    const bool neg = d <= 0.0f;
                    if (i == 0 && j == 0 && k == 0) sign_first = neg;
                    else if (neg != sign_first) { mixed = true; break; }
                }
        out->samples = 125;
        if (!mixed && nearest > safe) return;
    }

    std::vector<float> field(size_t(cs3), 0.0f);
    std::vector<MaterialId> mats(size_t(cs3), MaterialId(0));
    auto corner_index = [&](int x, int y, int z) {
        return size_t((z + 1) * corners * corners + (y + 1) * corners + (x + 1));
    };
    auto corner_pos = [&](int x, int y, int z) {
        return req.origin + Vec3(float(x), float(y), float(z)) * cs;
    };

    bool any_negative = false, any_positive = false;
    for (int z = -1; z <= res; z++)
        for (int y = -1; y <= res; y++)
            for (int x = -1; x <= res; x++) {
                Sample s = req.density->sample(corner_pos(x, y, z));
                const size_t i = corner_index(x, y, z);
                field[i] = s.distance;
                mats[i] = s.material;
                if (s.distance <= 0.0f) any_negative = true;
                else any_positive = true;
            }
    out->samples += uint32_t(cs3);

    // Entirely inside or entirely outside: nothing to draw, and this
    // is most chunks in any world worth having.
    if (!any_negative || !any_positive) return;

    // --- one vertex per cell that the surface passes through
    const int c3 = cells * cells * cells;
    std::vector<int32_t> cell_vertex(size_t(c3), int32_t(-1));
    auto cell_index = [&](int x, int y, int z) {
        return size_t((z + 1) * cells * cells + (y + 1) * cells + (x + 1));
    };

    // Trilinearly interpolated central differences over the sampled
    // corners. `p` is in cell-local 0..1.
    auto grid_gradient = [&](int cx, int cy, int cz, const Vec3 &p) {
        auto at = [&](int x, int y, int z) {
            x = std::clamp(x, -1, res);
            y = std::clamp(y, -1, res);
            z = std::clamp(z, -1, res);
            return field[corner_index(x, y, z)];
        };
        // Sample the two faces of the cell along each axis and blend
        // by the crossing's position on the other two.
        auto face = [&](int axis, int side) {
            float acc = 0.0f;
            for (int i = 0; i < 4; i++) {
                const int a = i & 1, b = (i >> 1) & 1;
                float w = 1.0f;
                int o[3] = {0, 0, 0};
                o[axis] = side;
                if (axis == 0) {
                    o[1] = a; o[2] = b;
                    w = (a ? p.y : 1.0f - p.y) * (b ? p.z : 1.0f - p.z);
                } else if (axis == 1) {
                    o[0] = a; o[2] = b;
                    w = (a ? p.x : 1.0f - p.x) * (b ? p.z : 1.0f - p.z);
                } else {
                    o[0] = a; o[1] = b;
                    w = (a ? p.x : 1.0f - p.x) * (b ? p.y : 1.0f - p.y);
                }
                acc += at(cx + o[0], cy + o[1], cz + o[2]) * w;
            }
            return acc;
        };
        return Vec3(face(0, 1) - face(0, 0), face(1, 1) - face(1, 0),
                    face(2, 1) - face(2, 0));
    };

    for (int z = -1; z < res; z++)
        for (int y = -1; y < res; y++)
            for (int x = -1; x < res; x++) {
                float corner_field[8];
                for (int i = 0; i < 8; i++)
                    corner_field[i] = field[corner_index(x + (i & 1), y + ((i >> 1) & 1),
                                                         z + ((i >> 2) & 1))];
                // Any sign change at all?
                bool inside = corner_field[0] <= 0.0f;
                bool crossing = false;
                for (int i = 1; i < 8; i++)
                    if ((corner_field[i] <= 0.0f) != inside) crossing = true;
                if (!crossing) continue;

                float ata[6] = {0, 0, 0, 0, 0, 0};
                Vec3 atb, mass, normal_sum;
                int count = 0;
                for (const auto &e : kEdgeCorners) {
                    const float a = corner_field[e[0]];
                    const float b = corner_field[e[1]];
                    if ((a <= 0.0f) == (b <= 0.0f)) continue;
                    // Where along the edge the surface is. The field
                    // is close enough to linear over one cell that a
                    // straight interpolation is within a fraction of
                    // a texel of the truth.
                    const float t = a / (a - b);
                    Vec3 ca(float((e[0] & 1)), float((e[0] >> 1) & 1),
                            float((e[0] >> 2) & 1));
                    Vec3 cb(float((e[1] & 1)), float((e[1] >> 1) & 1),
                            float((e[1] >> 2) & 1));
                    // In cell-local units, 0..1.
                    Vec3 p = lerp(ca, cb, t);
                    // THE NORMAL COMES FROM THE GRID WE ALREADY HAVE.
                    //
                    // Asking the density source costs six more
                    // samples per crossing, and there are several
                    // crossings per surface cell and tens of
                    // thousands of surface cells -- which for a
                    // generator built out of fractal noise is most
                    // of the meshing time. Central differences on
                    // the corner field are one subtraction each and
                    // are the gradient of exactly the function the
                    // vertices are being fitted to, which matters
                    // more than being the gradient of the true
                    // field.
                    Vec3 n = grid_gradient(x, y, z, p);
                    if (n.length_sq() < 1e-12f) n = Vec3::up();
                    n = n.normalized();

                    ata[0] += n.x * n.x;
                    ata[1] += n.x * n.y;
                    ata[2] += n.x * n.z;
                    ata[3] += n.y * n.y;
                    ata[4] += n.y * n.z;
                    ata[5] += n.z * n.z;
                    const float d = dot(n, p);
                    atb += n * d;
                    mass += p;
                    normal_sum += n;
                    count++;
                }
                if (!count) continue;
                mass /= float(count);

                Vec3 v = solve_qef(ata, atb, mass, req.qef_regularisation);
                // CLAMPED TO ITS OWN CELL. A rank-deficient solve can
                // put the vertex a long way away, and a vertex
                // outside its cell tangles the quads that reference
                // it into a knot.
                v.x = clampf(v.x, -0.2f, 1.2f);
                v.y = clampf(v.y, -0.2f, 1.2f);
                v.z = clampf(v.z, -0.2f, 1.2f);

                Vertex vert;
                vert.position = req.origin +
                                (Vec3(float(x), float(y), float(z)) + v) * cs;
                Vec3 n = req.smooth_normals && normal_sum.length_sq() > 1e-12f
                             ? normal_sum.normalized()
                             : grid_gradient(x, y, z, v).normalized();
                vert.normal = n.length_sq() > 1e-12f ? n : Vec3::up();
                // Triplanar is done in the shader; this is a stable
                // fallback so a plain textured material still works.
                vert.uv = {vert.position.x * 0.25f, vert.position.z * 0.25f};

                // ASKED AT THE SURFACE, WITH ITS NORMAL.
                //
                // Taking the deepest solid corner's material instead
                // makes the answer flip between neighbouring cells
                // wherever a depth band crosses them, and the whole
                // landscape comes out in contour stripes.
                MaterialId best_mat =
                    req.density->surface_material(vert.position, vert.normal);
                if (best_mat == 0) {
                    // The field says air at the vertex, which happens
                    // a fraction of a cell out. Fall back to whatever
                    // solid corner is nearest.
                    float deepest = 0.0f;
                    for (int i = 0; i < 8; i++) {
                        if (corner_field[i] > deepest) continue;
                        const size_t ci = corner_index(
                            x + (i & 1), y + ((i >> 1) & 1), z + ((i >> 2) & 1));
                        if (mats[ci] == 0) continue;
                        deepest = corner_field[i];
                        best_mat = mats[ci];
                    }
                }
                const Palette &pal = Palette::instance();
                Color c = pal.colours[best_mat];
                vert.colour[0] = uint8_t(clampf(c.r, 0, 1) * 255.0f + 0.5f);
                vert.colour[1] = uint8_t(clampf(c.g, 0, 1) * 255.0f + 0.5f);
                vert.colour[2] = uint8_t(clampf(c.b, 0, 1) * 255.0f + 0.5f);
                vert.colour[3] = uint8_t(clampf(pal.roughness[best_mat], 0, 1) *
                                             255.0f + 0.5f);

                cell_vertex[cell_index(x, y, z)] = int32_t(out->vertices.size());
                out->vertices.push_back(vert);
                out->bounds.expand(vert.position);
                out->surface_cells++;
            }

    if (out->vertices.empty()) return;

    // --- one quad per crossing edge, joining the four cells round it
    //
    // OWNERSHIP. Each chunk emits the quads for edges whose corner
    // lies in its own [0, res) block, so every face in the world is
    // emitted exactly once and neighbouring chunks neither crack nor
    // overlap.
    auto emit = [&](int32_t a, int32_t b, int32_t c, int32_t d, bool flip) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        if (flip) std::swap(b, d);
        out->indices.push_back(uint32_t(a));
        out->indices.push_back(uint32_t(b));
        out->indices.push_back(uint32_t(c));
        out->indices.push_back(uint32_t(a));
        out->indices.push_back(uint32_t(c));
        out->indices.push_back(uint32_t(d));
    };

    for (int z = 0; z < res; z++)
        for (int y = 0; y < res; y++)
            for (int x = 0; x < res; x++) {
                const float here = field[corner_index(x, y, z)];
                const bool inside = here <= 0.0f;

                // Along x: the four cells sharing it differ in y and z.
                if ((field[corner_index(x + 1, y, z)] <= 0.0f) != inside)
                    emit(cell_vertex[cell_index(x, y - 1, z - 1)],
                         cell_vertex[cell_index(x, y, z - 1)],
                         cell_vertex[cell_index(x, y, z)],
                         cell_vertex[cell_index(x, y - 1, z)], !inside);
                // Along y: differ in x and z.
                if ((field[corner_index(x, y + 1, z)] <= 0.0f) != inside)
                    emit(cell_vertex[cell_index(x - 1, y, z - 1)],
                         cell_vertex[cell_index(x - 1, y, z)],
                         cell_vertex[cell_index(x, y, z)],
                         cell_vertex[cell_index(x, y, z - 1)], !inside);
                // Along z: differ in x and y.
                if ((field[corner_index(x, y, z + 1)] <= 0.0f) != inside)
                    emit(cell_vertex[cell_index(x - 1, y - 1, z)],
                         cell_vertex[cell_index(x, y - 1, z)],
                         cell_vertex[cell_index(x, y, z)],
                         cell_vertex[cell_index(x - 1, y, z)], !inside);
            }

    out->empty = out->indices.empty();
}

}  // namespace mf::voxel
