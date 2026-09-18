// Warren -- reading glTF 2.0.
//
// The interchange format, so that a character modelled in Blender
// can be walked around in this engine. .gltf (JSON beside a .bin)
// and .glb (one binary container) both, because exporters produce
// whichever they were asked for and a user should not have to know
// which their engine prefers.
//
// It produces a PackedScene, like every other importer should: the
// file becomes a tree of nodes you instance, not a mesh you have to
// assemble. That is what makes "drag the character into the level"
// mean something.
//
// TWO CONVENTIONS HAVE TO BE CONVERTED AND BOTH ARE SILENT WHEN
// WRONG. glTF is right-handed with +Y up and -Z forward, which this
// engine shares, so positions pass through -- but its images are
// sRGB for colour and linear for data, its winding is
// counter-clockwise, and its UV origin is the TOP left while a
// texture sampled here has v increasing downwards already. Getting
// the last one wrong gives a model that looks fine until it has
// text on it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/json.h"
#include "core/log.h"
#include "render/material.h"
#include "anim/clip.h"
#include "anim/skeleton.h"
#include "render/mesh.h"
#include "scene/animated.h"
#include "render/texture.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/nodes.h"

namespace wr {
namespace {

struct Buffer {
    std::vector<uint8_t> bytes;
};

struct Gltf {
    Json doc;
    std::vector<Buffer> buffers;
    std::filesystem::path base;
    std::string name;
};

std::vector<uint8_t> read_file(const std::filesystem::path &p) {
    std::vector<uint8_t> out;
    FILE *f = std::fopen(p.string().c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(std::max(0L, size)));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return out;
}

// data:application/octet-stream;base64,....
bool decode_base64(const std::string &text, std::vector<uint8_t> *out) {
    static const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    uint32_t bits = 0;
    int have = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = value(c);
        if (v < 0) return false;
        bits = (bits << 6) | uint32_t(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out->push_back(uint8_t((bits >> have) & 0xFF));
        }
    }
    return true;
}

bool load_buffers(Gltf *g) {
    const Json &buffers = g->doc["buffers"];
    for (size_t i = 0; i < buffers.size(); i++) {
        const Json &b = buffers[i];
        Buffer out;
        const std::string uri = b["uri"].string();
        if (uri.empty()) {
            // The GLB chunk, already filled in by the caller.
            if (i < g->buffers.size() && !g->buffers[i].bytes.empty()) continue;
            WR_ERROR("gltf: buffer %zu has no uri and no binary chunk", i);
            return false;
        }
        if (uri.rfind("data:", 0) == 0) {
            const size_t comma = uri.find(',');
            if (comma == std::string::npos) return false;
            if (!decode_base64(uri.substr(comma + 1), &out.bytes)) {
                WR_ERROR("gltf: buffer %zu has a malformed data uri", i);
                return false;
            }
        } else {
            // Percent-decoding, because an exporter will happily
            // write "my model.bin" as "my%20model.bin".
            std::string decoded;
            for (size_t k = 0; k < uri.size(); k++) {
                if (uri[k] == '%' && k + 2 < uri.size()) {
                    const std::string hex = uri.substr(k + 1, 2);
                    decoded.push_back(char(std::strtol(hex.c_str(), nullptr, 16)));
                    k += 2;
                } else {
                    decoded.push_back(uri[k]);
                }
            }
            out.bytes = read_file(g->base / decoded);
            if (out.bytes.empty()) {
                WR_ERROR("gltf: could not read '%s'", decoded.c_str());
                return false;
            }
        }
        if (i < g->buffers.size())
            g->buffers[i] = std::move(out);
        else
            g->buffers.push_back(std::move(out));
    }
    return true;
}

int component_size(int type) {
    switch (type) {
        case 5120: case 5121: return 1;   // byte, unsigned byte
        case 5122: case 5123: return 2;   // short, unsigned short
        case 5125: case 5126: return 4;   // unsigned int, float
        default: return 0;
    }
}

int component_count(const std::string &type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    if (type == "MAT3") return 9;
    if (type == "MAT2") return 4;
    return 0;
}

// Every accessor read goes through here as floats, whatever it was
// stored as. Sixteen-bit UVs and byte-normalised colours are both
// common, and a reader with a special case per combination is a
// reader with a bug per combination.
bool read_accessor(const Gltf &g, int index, std::vector<float> *out,
                   int *out_components) {
    if (index < 0) return false;
    const Json &acc = g.doc["accessors"][size_t(index)];
    if (acc.is_null()) return false;
    const int count = acc["count"].integer();
    const int ctype = acc["componentType"].integer();
    const std::string type = acc["type"].string();
    const int comps = component_count(type);
    const int csize = component_size(ctype);
    if (!count || !comps || !csize) return false;
    const bool normalised = acc["normalized"].boolean();
    if (out_components) *out_components = comps;

    out->assign(size_t(count) * size_t(comps), 0.0f);
    if (!acc.has("bufferView")) return true;   // sparse or all zero

    const Json &view = g.doc["bufferViews"][size_t(acc["bufferView"].integer())];
    if (view.is_null()) return false;
    const size_t buffer = size_t(view["buffer"].integer());
    if (buffer >= g.buffers.size()) return false;
    const std::vector<uint8_t> &bytes = g.buffers[buffer].bytes;

    const size_t view_offset = size_t(view["byteOffset"].integer(0));
    const size_t acc_offset = size_t(acc["byteOffset"].integer(0));
    const size_t element = size_t(comps) * size_t(csize);
    // A stride of zero means tightly packed, which is most files;
    // an interleaved one gives the real distance between elements.
    const size_t stride = size_t(view["byteStride"].integer(0));
    const size_t step = stride ? stride : element;
    const size_t start = view_offset + acc_offset;
    if (start + step * size_t(count - 1) + element > bytes.size()) {
        WR_ERROR("gltf: accessor %d reads past the end of its buffer", index);
        return false;
    }

    for (int i = 0; i < count; i++) {
        const uint8_t *p = bytes.data() + start + step * size_t(i);
        for (int c = 0; c < comps; c++) {
            const uint8_t *q = p + size_t(c) * size_t(csize);
            float v = 0.0f;
            switch (ctype) {
                case 5126: std::memcpy(&v, q, 4); break;
                case 5125: {
                    uint32_t u;
                    std::memcpy(&u, q, 4);
                    v = float(u);
                    break;
                }
                case 5123: {
                    uint16_t u;
                    std::memcpy(&u, q, 2);
                    v = normalised ? float(u) / 65535.0f : float(u);
                    break;
                }
                case 5122: {
                    int16_t s;
                    std::memcpy(&s, q, 2);
                    v = normalised ? std::max(float(s) / 32767.0f, -1.0f)
                                   : float(s);
                    break;
                }
                case 5121:
                    v = normalised ? float(*q) / 255.0f : float(*q);
                    break;
                case 5120: {
                    const int8_t s = int8_t(*q);
                    v = normalised ? std::max(float(s) / 127.0f, -1.0f)
                                   : float(s);
                    break;
                }
                default: break;
            }
            (*out)[size_t(i) * size_t(comps) + size_t(c)] = v;
        }
    }
    return true;
}

// --- materials

Ref<Texture> load_texture(const Gltf &g, int texture_index, bool srgb) {
    if (texture_index < 0) return {};
    const Json &tex = g.doc["textures"][size_t(texture_index)];
    if (tex.is_null() || !tex.has("source")) return {};
    const Json &img = g.doc["images"][size_t(tex["source"].integer())];
    if (img.is_null()) return {};

    rhi::Device *dev = resource_device();
    if (!dev) return {};

    const std::string uri = img["uri"].string();
    if (!uri.empty() && uri.rfind("data:", 0) != 0) {
        const std::filesystem::path p = g.base / uri;
        Ref<Resource> r = ResourceLoader::load(p.string());
        Texture *t = r ? r->cast_to<Texture>() : nullptr;
        if (t) return Ref<Texture>(t);
        return Texture::load(dev, p.string(), srgb);
    }
    // Embedded: either a data uri or a bufferView.
    std::vector<uint8_t> bytes;
    if (!uri.empty()) {
        const size_t comma = uri.find(',');
        if (comma != std::string::npos)
            decode_base64(uri.substr(comma + 1), &bytes);
    } else if (img.has("bufferView")) {
        const Json &view = g.doc["bufferViews"][size_t(img["bufferView"].integer())];
        const size_t buffer = size_t(view["buffer"].integer());
        if (buffer < g.buffers.size()) {
            const size_t off = size_t(view["byteOffset"].integer(0));
            const size_t len = size_t(view["byteLength"].integer(0));
            const std::vector<uint8_t> &src = g.buffers[buffer].bytes;
            if (off + len <= src.size())
                bytes.assign(src.begin() + long(off), src.begin() + long(off + len));
        }
    }
    if (bytes.empty()) return {};
    return Texture::from_memory(dev, bytes.data(), bytes.size(), srgb);
}

Ref<Material> build_material(const Gltf &g, int index) {
    Ref<Material> m(new Material());
    if (index < 0) return m;
    const Json &src = g.doc["materials"][size_t(index)];
    if (src.is_null()) return m;
    m->set_resource_name(src["name"].string());

    const Json &pbr = src["pbrMetallicRoughness"];
    if (pbr.has("baseColorFactor")) {
        const Json &c = pbr["baseColorFactor"];
        // The factor is LINEAR in glTF, and Color is linear here, so
        // it passes straight through -- unlike a hex literal, which
        // Color::hex has to convert.
        m->albedo = Color(float(c[0].number(1)), float(c[1].number(1)),
                          float(c[2].number(1)), float(c[3].number(1)));
    }
    m->metallic = float(pbr["metallicFactor"].number(1.0));
    m->roughness = float(pbr["roughnessFactor"].number(1.0));
    if (pbr.has("baseColorTexture"))
        m->albedo_map =
            load_texture(g, pbr["baseColorTexture"]["index"].integer(-1), true);
    if (pbr.has("metallicRoughnessTexture"))
        m->orm_map = load_texture(
            g, pbr["metallicRoughnessTexture"]["index"].integer(-1), false);
    if (src.has("normalTexture")) {
        m->normal_map =
            load_texture(g, src["normalTexture"]["index"].integer(-1), false);
        m->normal_scale = float(src["normalTexture"]["scale"].number(1.0));
    }
    if (src.has("occlusionTexture") && !m->orm_map)
        m->orm_map =
            load_texture(g, src["occlusionTexture"]["index"].integer(-1), false);
    if (src.has("emissiveTexture"))
        m->emissive_map =
            load_texture(g, src["emissiveTexture"]["index"].integer(-1), true);
    if (src.has("emissiveFactor")) {
        const Json &e = src["emissiveFactor"];
        m->emissive = Color(float(e[0].number()), float(e[1].number()),
                            float(e[2].number()), 1.0f);
        m->emissive_strength =
            (m->emissive.r + m->emissive.g + m->emissive.b) > 0.0f ? 1.0f : 0.0f;
    }
    const std::string alpha = src["alphaMode"].string("OPAQUE");
    if (alpha == "BLEND") m->pass = MaterialPass::Transparent;
    else if (alpha == "MASK") {
        m->pass = MaterialPass::AlphaCutout;
        m->alpha_cutoff = float(src["alphaCutoff"].number(0.5));
    }
    m->double_sided = src["doubleSided"].boolean(false);
    m->touch();
    return m;
}

// --- meshes

struct BuiltMesh {
    Ref<Mesh> mesh;
    std::vector<Ref<Material>> materials;
};

// A glTF SKIN is a list of nodes that act as bones plus the matrices
// that take the mesh into each of their spaces. It is a separate
// object from the node hierarchy, which is what makes it possible for
// two meshes to share one skeleton and for a skeleton's bones to be
// ordinary nodes with children of their own.
struct BuiltSkin {
    Ref<Skeleton> skeleton;
    // glTF node index -> bone index, for the animation importer.
    std::unordered_map<int, int> node_to_bone;
    // Old bone index -> new, when the joints had to be reordered so
    // that parents come first. Empty when they already did.
    std::vector<int> remap;
};

BuiltMesh build_mesh(const Gltf &g, int index,
                     std::unordered_map<int, Ref<Material>> *material_cache) {
    BuiltMesh out;
    const Json &src = g.doc["meshes"][size_t(index)];
    if (src.is_null()) return out;

    out.mesh = Ref<Mesh>(new Mesh());
    out.mesh->set_resource_name(src["name"].string());
    const Json &primitives = src["primitives"];

    for (size_t p = 0; p < primitives.size(); p++) {
        const Json &prim = primitives[p];
        // Triangles only. A glTF may carry lines or points and an
        // importer that quietly turns them into triangles produces
        // garbage; skipping them and saying so does not.
        const int mode = prim["mode"].integer(4);
        if (mode != 4) {
            WR_WARN("gltf: primitive %zu of mesh %d is mode %d, not triangles;"
                    " skipped", p, index, mode);
            continue;
        }
        const Json &attrs = prim["attributes"];
        std::vector<float> positions, normals, tangents, uvs, colours;
        int comps = 0;
        if (!read_accessor(g, attrs["POSITION"].integer(-1), &positions, &comps))
            continue;
        const size_t count = positions.size() / 3;
        if (!count) continue;

        read_accessor(g, attrs["NORMAL"].integer(-1), &normals, &comps);
        read_accessor(g, attrs["TANGENT"].integer(-1), &tangents, &comps);
        int uv_comps = 2;
        read_accessor(g, attrs["TEXCOORD_0"].integer(-1), &uvs, &uv_comps);
        int colour_comps = 4;
        read_accessor(g, attrs["COLOR_0"].integer(-1), &colours, &colour_comps);
        // WHICH BONES MOVE THIS VERTEX, AND HOW MUCH.
        //
        // Read through the same float path as everything else, which
        // matters more here than anywhere: joints arrive as unsigned
        // bytes on a rig under 256 bones and unsigned shorts on one
        // over, and weights as bytes, shorts or floats, normalised or
        // not. Eight spellings of one attribute is eight bugs in a
        // reader that special-cases them.
        std::vector<float> joints, weights;
        int joint_comps = 0, weight_comps = 0;
        read_accessor(g, attrs["JOINTS_0"].integer(-1), &joints, &joint_comps);
        read_accessor(g, attrs["WEIGHTS_0"].integer(-1), &weights, &weight_comps);
        const bool skinned = joints.size() >= count * 4 && weights.size() >= count * 4;
        if (skinned && out.mesh->skin.size() < out.mesh->vertices.size()) {
            // A mesh whose first primitive was unskinned and whose
            // second is: pad the ones already in so the streams stay
            // the same length.
            out.mesh->skin.resize(out.mesh->vertices.size());
        }

        const uint32_t base_vertex = uint32_t(out.mesh->vertices.size());
        for (size_t i = 0; i < count; i++) {
            Vertex v;
            v.position = {positions[i * 3], positions[i * 3 + 1],
                          positions[i * 3 + 2]};
            if (normals.size() >= (i + 1) * 3)
                v.normal = {normals[i * 3], normals[i * 3 + 1],
                            normals[i * 3 + 2]};
            if (tangents.size() >= (i + 1) * 4)
                v.tangent = {tangents[i * 4], tangents[i * 4 + 1],
                             tangents[i * 4 + 2], tangents[i * 4 + 3]};
            if (uvs.size() >= (i + 1) * 2)
                v.uv = {uvs[i * 2], uvs[i * 2 + 1]};
            if (!colours.empty()) {
                const size_t stride = size_t(colour_comps);
                const size_t at = i * stride;
                if (colours.size() >= at + stride) {
                    auto ch = [](float f) {
                        return uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    };
                    v.colour[0] = ch(colours[at]);
                    v.colour[1] = ch(colours[at + 1]);
                    v.colour[2] = ch(colours[at + 2]);
                    v.colour[3] = stride >= 4 ? ch(colours[at + 3]) : 255;
                }
            }
            out.mesh->vertices.push_back(v);

            if (skinned) {
                SkinVertex sv;
                // NORMALISED HERE, NOT IN THE SHADER. Exporters are
                // casual about this -- weights that sum to 0.999 or to
                // 1.004 are both common -- and a vertex whose weights
                // do not sum to one is a vertex that shrinks towards
                // the origin when the skeleton moves.
                float total = 0.0f;
                for (int k = 0; k < 4; k++) total += weights[i * 4 + size_t(k)];
                const float inv = total > 1e-6f ? 1.0f / total : 0.0f;
                for (int k = 0; k < 4; k++) {
                    const float jf = joints[i * 4 + size_t(k)];
                    sv.joints[k] = uint8_t(std::clamp(jf, 0.0f, 255.0f));
                    sv.weights[k] = uint8_t(
                            std::clamp(weights[i * 4 + size_t(k)] * inv, 0.0f, 1.0f)
                                    * 255.0f + 0.5f);
                }
                out.mesh->skin.push_back(sv);
            } else if (!out.mesh->skin.empty()) {
                out.mesh->skin.push_back(SkinVertex());
            }
        }

        SubMesh sm;
        sm.first_index = uint32_t(out.mesh->indices.size());
        if (prim.has("indices")) {
            std::vector<float> idx;
            read_accessor(g, prim["indices"].integer(-1), &idx, &comps);
            for (float f : idx)
                out.mesh->indices.push_back(base_vertex + uint32_t(f));
        } else {
            // No index buffer: the vertices are the triangles.
            for (size_t i = 0; i < count; i++)
                out.mesh->indices.push_back(base_vertex + uint32_t(i));
        }
        sm.index_count = uint32_t(out.mesh->indices.size()) - sm.first_index;
        sm.material_slot = int32_t(out.materials.size());
        sm.name = src["name"].string();
        out.mesh->submeshes.push_back(sm);

        const int mat_index = prim["material"].integer(-1);
        auto it = material_cache->find(mat_index);
        if (it == material_cache->end())
            it = material_cache->emplace(mat_index,
                                         build_material(g, mat_index)).first;
        out.materials.push_back(it->second);
    }

    if (out.mesh->vertices.empty()) return out;
    // A file without normals is legal and common for flat-shaded
    // exports; without tangents is normal unless it has a normal
    // map. Both are cheaper to derive than to demand.
    bool has_normals = false;
    for (const Vertex &v : out.mesh->vertices)
        if (v.normal.length_sq() > 1e-6f) {
            has_normals = true;
            break;
        }
    if (!has_normals) out.mesh->compute_normals();
    out.mesh->compute_tangents();
    out.mesh->compute_bounds();
    return out;
}

// --- nodes

Transform3D node_transform(const Json &n) {
    if (n.has("matrix")) {
        const Json &m = n["matrix"];
        // Column major, sixteen floats, which is this engine's own
        // order -- so it is read straight into a Projection and the
        // affine part taken out.
        Basis b;
        for (int c = 0; c < 3; c++)
            b.col[c] = Vec3(float(m[size_t(c * 4 + 0)].number()),
                            float(m[size_t(c * 4 + 1)].number()),
                            float(m[size_t(c * 4 + 2)].number()));
        const Vec3 origin(float(m[12].number()), float(m[13].number()),
                          float(m[14].number()));
        return Transform3D(b, origin);
    }
    Vec3 translation, scale(1, 1, 1);
    Quat rotation;
    if (n.has("translation")) {
        const Json &t = n["translation"];
        translation = {float(t[0].number()), float(t[1].number()),
                       float(t[2].number())};
    }
    if (n.has("rotation")) {
        const Json &r = n["rotation"];
        rotation = Quat(float(r[0].number()), float(r[1].number()),
                        float(r[2].number()), float(r[3].number(1)));
    }
    if (n.has("scale")) {
        const Json &s = n["scale"];
        scale = {float(s[0].number(1)), float(s[1].number(1)),
                 float(s[2].number(1))};
    }
    Basis b(rotation);
    b.col[0] = b.col[0] * scale.x;
    b.col[1] = b.col[1] * scale.y;
    b.col[2] = b.col[2] * scale.z;
    return Transform3D(b, translation);
}

// ------------------------------------------------------------- skeletons

// EVERY BONE'S PARENT, FOUND BY WALKING THE SCENE BACKWARDS.
//
// glTF's skin lists its joints as node indices and says nothing about
// how they are related; the relationship is in the node tree, where a
// bone is an ordinary node with children. So the parent of joint J is
// whichever node has J in its `children` -- and that node is only a
// bone if it is in the joint list too, which is how the skeleton's
// root stops at the rig rather than running up into the scene.
void build_child_map(const Gltf &g, std::unordered_map<int, int> *parent_of) {
    const Json &nodes = g.doc["nodes"];
    for (size_t i = 0; i < nodes.size(); i++) {
        const Json &kids = nodes[i]["children"];
        for (size_t k = 0; k < kids.size(); k++)
            (*parent_of)[kids[k].integer(-1)] = int(i);
    }
}

BuiltSkin build_skin(const Gltf &g, int index) {
    BuiltSkin out;
    const Json &src = g.doc["skins"][size_t(index)];
    if (src.is_null()) return out;
    const Json &joints = src["joints"];
    if (joints.size() == 0) return out;

    std::unordered_map<int, int> parent_of;
    build_child_map(g, &parent_of);

    std::unordered_map<int, int> joint_slot;   // node -> its index in `joints`
    for (size_t i = 0; i < joints.size(); i++)
        joint_slot[joints[i].integer(-1)] = int(i);

    out.skeleton = Ref<Skeleton>(new Skeleton());
    out.skeleton->set_resource_name(src["name"].string());
    out.skeleton->bones.resize(joints.size());
    for (size_t i = 0; i < joints.size(); i++) {
        const int node = joints[i].integer(-1);
        const Json &n = g.doc["nodes"][size_t(node)];
        Skeleton::Bone &b = out.skeleton->bones[i];
        b.name = n["name"].string();
        if (b.name.empty()) b.name = "bone" + std::to_string(i);
        b.rest = node_transform(n);
        const auto p = parent_of.find(node);
        b.parent = -1;
        if (p != parent_of.end()) {
            const auto slot = joint_slot.find(p->second);
            if (slot != joint_slot.end()) b.parent = slot->second;
        }
        out.node_to_bone[node] = int(i);
    }

    // THE INVERSE BIND MATRICES, WHICH ARE NOT THE RESTS INVERTED.
    //
    // They usually are, and relying on that is a trap: a file may bind
    // its mesh in a pose that is not the rest pose, and several
    // exporters do. The accessor is authoritative where it exists.
    std::vector<float> ibm;
    int comps = 0;
    if (read_accessor(g, src["inverseBindMatrices"].integer(-1), &ibm, &comps)
            && comps == 16 && ibm.size() >= joints.size() * 16) {
        for (size_t i = 0; i < joints.size(); i++) {
            const float *m = &ibm[i * 16];
            // Column-major, as everywhere in glTF.
            Transform3D t;
            t.basis = Basis(Vec3(m[0], m[1], m[2]), Vec3(m[4], m[5], m[6]),
                            Vec3(m[8], m[9], m[10]));
            t.origin = Vec3(m[12], m[13], m[14]);
            out.skeleton->bones[i].inverse_bind = t;
        }
    } else {
        out.skeleton->compute_inverse_binds();
    }

    // The pose resolver walks forward once and needs every parent to
    // come first; most exporters already do that and none promise it.
    if (!out.skeleton->ordered()) {
        const std::vector<int> remap = out.skeleton->sort_hierarchically();
        for (auto &kv : out.node_to_bone) kv.second = remap[size_t(kv.second)];
        out.remap = remap;
    }
    return out;
}

// ------------------------------------------------------------ animations

Ref<AnimationClip> build_clip(const Gltf &g, int index, const BuiltSkin &skin) {
    const Json &src = g.doc["animations"][size_t(index)];
    if (src.is_null()) return {};
    Ref<AnimationClip> clip(new AnimationClip());
    clip->name = src["name"].string();
    if (clip->name.empty()) clip->name = "clip" + std::to_string(index);
    clip->set_resource_name(clip->name);

    const Json &channels = src["channels"];
    const Json &samplers = src["samplers"];
    // One track per bone, made on demand: a clip touches a fraction of
    // a rig and an array of empty tracks per bone is mostly nothing.
    std::unordered_map<int, size_t> track_of;

    for (size_t c = 0; c < channels.size(); c++) {
        const Json &ch = channels[c];
        const int node = ch["target"]["node"].integer(-1);
        const std::string path = ch["target"]["path"].string();
        const auto bone = skin.node_to_bone.find(node);
        if (bone == skin.node_to_bone.end()) continue;   // not part of this rig

        const Json &sm = samplers[size_t(ch["sampler"].integer(-1))];
        if (sm.is_null()) continue;
        std::vector<float> times, values;
        int tc = 0, vc = 0;
        if (!read_accessor(g, sm["input"].integer(-1), &times, &tc)) continue;
        if (!read_accessor(g, sm["output"].integer(-1), &values, &vc)) continue;
        // CUBICSPLINE stores three values per key -- in tangent, value,
        // out tangent -- and taking them as one value each plays the
        // tangents as keyframes, which looks like a body having a fit.
        // Read the middle of each triple and interpolate it linearly.
        const bool cubic = sm["interpolation"].string() == "CUBICSPLINE";
        const size_t stride = cubic ? 3u : 1u;

        auto it = track_of.find(bone->second);
        if (it == track_of.end()) {
            AnimationClip::BoneTrack tr;
            tr.bone = bone->second;
            tr.bone_name = skin.skeleton->bones[size_t(bone->second)].name;
            clip->tracks.push_back(tr);
            it = track_of.emplace(bone->second, clip->tracks.size() - 1).first;
        }
        AnimationClip::BoneTrack &tr = clip->tracks[it->second];

        const size_t n = times.size();
        if (path == "rotation") {
            if (values.size() < n * 4 * stride) continue;
            tr.rotation.times = times;
            tr.rotation.values.reserve(n);
            for (size_t i = 0; i < n; i++) {
                const size_t at = (i * stride + (cubic ? 1u : 0u)) * 4;
                tr.rotation.values.push_back(Quat(values[at], values[at + 1],
                                                  values[at + 2], values[at + 3])
                                                     .normalized());
            }
        } else if (path == "translation" || path == "scale") {
            if (values.size() < n * 3 * stride) continue;
            AnimationClip::Vec3Track &dst =
                    path == "scale" ? tr.scale : tr.position;
            dst.times = times;
            dst.values.reserve(n);
            for (size_t i = 0; i < n; i++) {
                const size_t at = (i * stride + (cubic ? 1u : 0u)) * 3;
                dst.values.push_back(Vec3(values[at], values[at + 1], values[at + 2]));
            }
        }
        // "weights" is morph-target animation, which this engine does
        // not have; skipped rather than half-read.
    }
    clip->compute_duration();
    return clip;
}

Node3D *build_node(const Gltf &g, int index,
                   std::unordered_map<int, BuiltMesh> *mesh_cache,
                   std::unordered_map<int, Ref<Material>> *material_cache,
                   std::unordered_map<int, BuiltSkin> *skin_cache,
                   const std::vector<Ref<AnimationClip>> *clips,
                   int depth) {
    if (depth > 64) return nullptr;
    const Json &src = g.doc["nodes"][size_t(index)];
    if (src.is_null()) return nullptr;

    Node3D *node = nullptr;
    const int mesh_index = src["mesh"].integer(-1);
    if (mesh_index >= 0) {
        auto it = mesh_cache->find(mesh_index);
        if (it == mesh_cache->end())
            it = mesh_cache->emplace(mesh_index,
                                     build_mesh(g, mesh_index, material_cache))
                     .first;
        // A NODE WITH A SKIN IS A DIFFERENT NODE.
        //
        // Skinned3D carries a skeleton and a pose and is what the
        // renderer skins; a plain MeshInstance3D has neither and
        // costs nothing for the ninety percent of a world that does
        // not move. Choosing here, once, is what keeps the renderer
        // from having to ask every draw whether it has bones.
        const int skin_index = src["skin"].integer(-1);
        MeshInstance3D *mi = nullptr;
        if (skin_index >= 0 && skin_cache) {
            auto sk = skin_cache->find(skin_index);
            if (sk == skin_cache->end())
                sk = skin_cache->emplace(skin_index, build_skin(g, skin_index)).first;
            if (sk->second.skeleton) {
                Skinned3D *s3 = new Skinned3D();
                s3->set_skeleton(sk->second.skeleton);
                mi = s3;
                // The clips this rig can play hang off the body that
                // plays them, so an instance of a character arrives
                // able to walk.
                if (clips && !clips->empty()) {
                    AnimationPlayer *ap = new AnimationPlayer();
                    ap->set_name("Animation");
                    for (const Ref<AnimationClip> &c : *clips) ap->add_clip(c);
                    s3->add_child(ap);
                }
            }
        }
        if (!mi) mi = new MeshInstance3D();
        mi->mesh = it->second.mesh;
        for (size_t s = 0; s < it->second.materials.size(); s++)
            mi->set_material(int(s), it->second.materials[s].get());
        node = mi;
    } else {
        node = new Node3D();
    }

    std::string name = src["name"].string();
    if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "node%d", index);
        name = buf;
    }
    node->set_name(name);
    node->set_transform(node_transform(src));

    const Json &children = src["children"];
    for (size_t i = 0; i < children.size(); i++) {
        Node3D *child = build_node(g, children[i].integer(-1), mesh_cache,
                                   material_cache, skin_cache, clips, depth + 1);
        if (child) node->add_child(child);
    }
    return node;
}

Ref<Resource> load_gltf(const std::string &path) {
    Gltf g;
    g.base = std::filesystem::path(path).parent_path();
    g.name = std::filesystem::path(path).stem().string();

    std::vector<uint8_t> bytes = read_file(path);
    if (bytes.empty()) {
        WR_ERROR("gltf: could not read '%s'", path.c_str());
        return {};
    }

    std::string json_text;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        // GLB: a header, then chunks of JSON and binary.
        uint32_t version = 0, total = 0;
        std::memcpy(&version, bytes.data() + 4, 4);
        std::memcpy(&total, bytes.data() + 8, 4);
        if (version != 2) {
            WR_ERROR("gltf: '%s' is glb version %u, not 2", path.c_str(),
                     version);
            return {};
        }
        size_t at = 12;
        while (at + 8 <= bytes.size()) {
            uint32_t length = 0, kind = 0;
            std::memcpy(&length, bytes.data() + at, 4);
            std::memcpy(&kind, bytes.data() + at + 4, 4);
            at += 8;
            if (at + length > bytes.size()) {
                WR_ERROR("gltf: '%s' has a chunk running past the end",
                         path.c_str());
                return {};
            }
            if (kind == 0x4E4F534Au)  // 'JSON'
                json_text.assign((const char *)bytes.data() + at, length);
            else if (kind == 0x004E4942u) {  // 'BIN'
                Buffer b;
                b.bytes.assign(bytes.begin() + long(at),
                               bytes.begin() + long(at + length));
                g.buffers.push_back(std::move(b));
            }
            at += length;
            at = (at + 3) & ~size_t(3);
        }
    } else {
        json_text.assign((const char *)bytes.data(), bytes.size());
    }

    std::string error;
    g.doc = Json::parse(json_text, &error);
    if (g.doc.is_null()) {
        WR_ERROR("gltf: '%s' is not valid JSON (%s)", path.c_str(),
                 error.c_str());
        return {};
    }
    const int version_major =
        std::atoi(g.doc["asset"]["version"].string("0").c_str());
    if (version_major != 2) {
        WR_ERROR("gltf: '%s' is version '%s'; this reads 2.x", path.c_str(),
                 g.doc["asset"]["version"].string("?").c_str());
        return {};
    }
    if (!load_buffers(&g)) return {};

    // The default scene, or the first, or every root node there is.
    Node3D *root = new Node3D();
    root->set_name(g.name.empty() ? "gltf" : g.name);

    std::unordered_map<int, BuiltMesh> mesh_cache;
    std::unordered_map<int, Ref<Material>> material_cache;
    std::unordered_map<int, BuiltSkin> skin_cache;

    // THE SKINS FIRST, BECAUSE THE CLIPS NEED THEM.
    //
    // An animation channel targets a NODE, and turning that into a
    // bone index needs the skin's joint list. Building them up front
    // also means a file whose two meshes share one skeleton gets one
    // Skeleton resource rather than two identical ones.
    const Json &skins = g.doc["skins"];
    for (size_t i = 0; i < skins.size(); i++)
        skin_cache.emplace(int(i), build_skin(g, int(i)));

    std::vector<Ref<AnimationClip>> clips;
    const Json &anims = g.doc["animations"];
    if (anims.size() > 0 && !skin_cache.empty()) {
        // Clips are resolved against the FIRST skin. A file with two
        // rigs and animations for both is a file this importer does
        // not try to be clever about: the clip keeps its bone NAMES,
        // and AnimationPlayer re-resolves them against whatever
        // skeleton it is actually played on.
        const BuiltSkin &first = skin_cache.begin()->second;
        for (size_t i = 0; i < anims.size(); i++)
            if (Ref<AnimationClip> c = build_clip(g, int(i), first))
                clips.push_back(c);
    }

    const int scene_index = g.doc["scene"].integer(0);
    const Json &scene = g.doc["scenes"][size_t(scene_index)];
    if (!scene.is_null()) {
        const Json &roots = scene["nodes"];
        for (size_t i = 0; i < roots.size(); i++)
            if (Node3D *n = build_node(g, roots[i].integer(-1), &mesh_cache,
                                       &material_cache, &skin_cache, &clips, 0))
                root->add_child(n);
    } else {
        // No scene at all: take every node with no parent.
        std::vector<bool> is_child(g.doc["nodes"].size(), false);
        for (size_t i = 0; i < g.doc["nodes"].size(); i++) {
            const Json &kids = g.doc["nodes"][i]["children"];
            for (size_t k = 0; k < kids.size(); k++) {
                const int c = kids[k].integer(-1);
                if (c >= 0 && size_t(c) < is_child.size()) is_child[size_t(c)] = true;
            }
        }
        for (size_t i = 0; i < is_child.size(); i++)
            if (!is_child[i])
                if (Node3D *n = build_node(g, int(i), &mesh_cache,
                                           &material_cache, &skin_cache, &clips, 0))
                    root->add_child(n);
    }

    root->set_owner_recursive(root);
    root->set_owner(nullptr);

    Ref<PackedScene> packed(new PackedScene());
    const bool ok = packed->pack(root);
    root->queue_free();
    delete root;
    if (!ok) return {};

    size_t triangles = 0;
    for (const auto &kv : mesh_cache)
        if (kv.second.mesh) triangles += kv.second.mesh->indices.size() / 3;
    WR_INFO("gltf: %s -- %zu meshes, %zu materials, %zu triangles",
            std::filesystem::path(path).filename().string().c_str(),
            mesh_cache.size(), material_cache.size(), triangles);
    return Ref<Resource>(packed.get());
}

struct RegisterGltf {
    RegisterGltf() {
        ResourceLoader::register_loader({"gltf", "glb"}, &load_gltf);
    }
};
RegisterGltf g_register_gltf;

}  // namespace
}  // namespace wr
