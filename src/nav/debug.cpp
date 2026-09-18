#include "nav/debug.h"

#include <cmath>

namespace wr::nav {

namespace {

// A spread of hues that stay distinct next to each other. Cycling a
// hue by the golden angle is the cheap way to get that: consecutive
// ids land far apart on the wheel and never repeat until the wheel
// is full.
void region_colour(uint32_t id, uint8_t *out) {
    if (id == 0) {
        out[0] = out[1] = out[2] = 90;
        out[3] = 255;
        return;
    }
    float h = std::fmod(float(id) * 0.61803399f, 1.0f) * 6.0f;
    int sector = int(h);
    float f = h - float(sector);
    const float s = 0.72f, v = 0.92f;
    float p = v * (1.0f - s), q = v * (1.0f - s * f),
          t = v * (1.0f - s * (1.0f - f));
    float r = v, g = v, b = v;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    out[0] = uint8_t(r * 255.0f);
    out[1] = uint8_t(g * 255.0f);
    out[2] = uint8_t(b * 255.0f);
    out[3] = 255;
}

Vertex flat_vertex(const Vec3 &p, const uint8_t *c) {
    Vertex v;
    v.position = p;
    v.normal = Vec3(0, 1, 0);
    v.tangent = Vec4(1, 0, 0, 1);
    for (int i = 0; i < 4; ++i) v.colour[i] = c[i];
    return v;
}

void push_quad(Mesh &m, const Vec3 &a, const Vec3 &b, const Vec3 &c,
               const Vec3 &d, const uint8_t *col) {
    uint32_t base = uint32_t(m.vertices.size());
    m.vertices.push_back(flat_vertex(a, col));
    m.vertices.push_back(flat_vertex(b, col));
    m.vertices.push_back(flat_vertex(c, col));
    m.vertices.push_back(flat_vertex(d, col));
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i : idx) m.indices.push_back(base + i);
}

void finish(Mesh &m, const char *name) {
    SubMesh sm;
    sm.first_index = 0;
    sm.index_count = uint32_t(m.indices.size());
    sm.name = name;
    m.submeshes.push_back(sm);
    m.compute_tangents();
    m.compute_bounds();
}

// A flat bar along a segment, `width` across, lying in the xz plane.
void push_bar(Mesh &m, const Vec3 &a, const Vec3 &b, float width,
              const uint8_t *col) {
    Vec3 d = b - a;
    float len = std::sqrt(d.x * d.x + d.z * d.z);
    Vec3 side = len > 1e-5f ? Vec3(-d.z / len, 0.0f, d.x / len) * (width * 0.5f)
                            : Vec3(width * 0.5f, 0.0f, 0.0f);
    push_quad(m, a - side, b - side, b + side, a + side, col);
}

}  // namespace

Ref<Mesh> debug_surface(const NavMesh &mesh, float lift, DebugColour colour) {
    Ref<Mesh> out(new Mesh());
    const Vec3 up(0.0f, lift, 0.0f);
    for (size_t i = 0; i < mesh.polys().size(); ++i) {
        const NavPoly &p = mesh.polys()[i];
        if (p.count < 3) continue;
        uint8_t col[4];
        switch (colour) {
            case DebugColour::Region: region_colour(p.region, col); break;
            case DebugColour::Polygon:
                region_colour(uint32_t(i) + 1, col);
                break;
            default:
                col[0] = 80;
                col[1] = 160;
                col[2] = 220;
                col[3] = 255;
                break;
        }
        // Fanned from the first vertex, which is exact because the
        // polygons are convex -- the same reason a path can cross
        // one in a straight line.
        uint32_t base = uint32_t(out->vertices.size());
        for (int k = 0; k < p.count; ++k)
            out->vertices.push_back(
                flat_vertex(mesh.verts()[p.verts[k]] + up, col));
        for (int k = 1; k + 1 < p.count; ++k) {
            out->indices.push_back(base);
            out->indices.push_back(base + uint32_t(k));
            out->indices.push_back(base + uint32_t(k) + 1);
        }
    }
    finish(*out, "navmesh");
    return out;
}

Ref<Mesh> debug_edges(const NavMesh &mesh, float lift, float width) {
    Ref<Mesh> out(new Mesh());
    const Vec3 up(0.0f, lift, 0.0f);
    const uint8_t wall[4] = {245, 90, 70, 255};
    const uint8_t seam[4] = {40, 40, 50, 255};
    for (size_t i = 0; i < mesh.polys().size(); ++i) {
        const NavPoly &p = mesh.polys()[i];
        for (int k = 0; k < p.count; ++k) {
            uint16_t n = p.neis[k];
            // An interior edge is drawn once, by the lower-numbered
            // polygon, or every seam is drawn twice and z-fights
            // with itself.
            if (n != kNoPoly && n < i) continue;
            push_bar(*out, mesh.verts()[p.verts[k]] + up,
                     mesh.verts()[p.verts[(k + 1) % p.count]] + up,
                     n == kNoPoly ? width * 1.6f : width,
                     n == kNoPoly ? wall : seam);
        }
    }
    finish(*out, "navmesh edges");
    return out;
}

Ref<Mesh> debug_links(const NavMesh &mesh, float width) {
    Ref<Mesh> out(new Mesh());
    const uint8_t good[4] = {250, 210, 60, 255};
    const uint8_t broken[4] = {255, 40, 40, 255};
    for (const NavLink &l : mesh.links()) {
        const bool attached =
            l.from_poly != kNoPoly && l.to_poly != kNoPoly;
        const uint8_t *col = attached ? good : broken;
        // A bar across each end, so the radius is visible, and a
        // ribbon between them. The ribbon is vertical, since a
        // ladder is mostly vertical and a flat one would be a line.
        Vec3 d = l.to - l.from;
        float len = std::sqrt(d.x * d.x + d.z * d.z);
        Vec3 side = len > 1e-5f
                        ? Vec3(-d.z / len, 0.0f, d.x / len) * l.radius
                        : Vec3(l.radius, 0.0f, 0.0f);
        const Vec3 up(0.0f, 0.02f, 0.0f);
        push_bar(*out, l.from - side + up, l.from + side + up, width, col);
        push_bar(*out, l.to - side + up, l.to + side + up, width, col);

        Vec3 thin = len > 1e-5f
                        ? Vec3(-d.z / len, 0.0f, d.x / len) * (width * 0.5f)
                        : Vec3(width * 0.5f, 0.0f, 0.0f);
        push_quad(*out, l.from - thin, l.to - thin, l.to + thin, l.from + thin,
                  col);
    }
    finish(*out, "navmesh links");
    return out;
}

}  // namespace wr::nav
