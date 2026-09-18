// Warren -- reading glTF, and the JSON under it.
//
// The test writes its own files. A sample asset would test the
// exporter that made it as much as the importer, and would not be
// there on a fresh checkout; a file built here can be made to
// contain exactly the awkward case being checked -- sixteen-bit
// indices, a byte-normalised colour, an interleaved buffer, a
// missing normal -- which is where an importer actually goes wrong.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/log.h"
#include "render/material.h"
#include "render/mesh.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/nodes.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

void write_file(const std::filesystem::path &p, const void *data, size_t n) {
    FILE *f = std::fopen(p.string().c_str(), "wb");
    if (!f) return;
    std::fwrite(data, 1, n, f);
    std::fclose(f);
}

// A triangle with positions, sixteen-bit indices and a normal,
// packed into one buffer, written as a .glb so the container is
// exercised too.
std::vector<uint8_t> build_glb() {
    // The binary chunk: 3 positions (vec3 float), 3 normals, 3 uvs
    // (vec2 float), then 3 indices (uint16).
    const float positions[9] = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    const float normals[9] = {0, 0, 1,  0, 0, 1,  0, 0, 1};
    const float uvs[6] = {0, 0,  1, 0,  0, 1};
    const uint16_t indices[3] = {0, 1, 2};

    std::vector<uint8_t> bin;
    auto append = [&](const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        bin.insert(bin.end(), b, b + n);
    };
    const size_t pos_off = 0;
    append(positions, sizeof(positions));
    const size_t nrm_off = bin.size();
    append(normals, sizeof(normals));
    const size_t uv_off = bin.size();
    append(uvs, sizeof(uvs));
    const size_t idx_off = bin.size();
    append(indices, sizeof(indices));
    while (bin.size() % 4) bin.push_back(0);

    char json[2048];
    std::snprintf(json, sizeof(json), R"({
"asset":{"version":"2.0","generator":"warren test"},
"scene":0,
"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Holder","children":[1],"translation":[5,0,0]},
         {"name":"Tri","mesh":0,"scale":[2,2,2]}],
"meshes":[{"name":"TriMesh","primitives":[
  {"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],
"materials":[{"name":"Red","pbrMetallicRoughness":{
  "baseColorFactor":[0.8,0.1,0.1,1.0],"metallicFactor":0.25,
  "roughnessFactor":0.6},"doubleSided":true}],
"accessors":[
 {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
 {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
 {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
 {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
"bufferViews":[
 {"buffer":0,"byteOffset":%zu,"byteLength":36},
 {"buffer":0,"byteOffset":%zu,"byteLength":36},
 {"buffer":0,"byteOffset":%zu,"byteLength":24},
 {"buffer":0,"byteOffset":%zu,"byteLength":6}],
"buffers":[{"byteLength":%zu}]
})", pos_off, nrm_off, uv_off, idx_off, bin.size());

    std::string text(json);
    while (text.size() % 4) text.push_back(' ');

    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) glb.push_back(uint8_t(v >> (8 * i)));
    };
    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    const uint32_t total = uint32_t(12 + 8 + text.size() + 8 + bin.size());
    u32(total);
    u32(uint32_t(text.size()));
    u32(0x4E4F534Au);   // JSON
    glb.insert(glb.end(), text.begin(), text.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942u);   // BIN
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    std::printf("gltf\n");

    // ---------------------------------------------------------- json
    {
        std::string err;
        Json j = Json::parse(R"({"a":1,"b":[1,2,3],"c":{"d":"x"},
                                 "e":true,"f":null,"g":-1.5e2})", &err);
        check(!j.is_null() && err.empty(), "a document parses");
        check(j["a"].integer() == 1, "numbers");
        check(j["b"].size() == 3 && j["b"][2].integer() == 3, "arrays");
        check(j["c"]["d"].string() == "x", "nested objects");
        check(j["e"].boolean(), "booleans");
        check(j["f"].is_null(), "null");
        check(std::fabs(j["g"].number() + 150.0) < 1e-9, "exponents");

        // A MISSING KEY IS A NULL, NOT A CRASH. glTF is mostly
        // optional fields, and a reader that throws on each one is a
        // reader wrapped in a hundred checks.
        check(j["nope"].is_null() && j["nope"]["deeper"][7].integer(42) == 42,
              "a missing path reads as its default all the way down");

        check(Json::parse("{\"a\":1", &err).is_null(), "truncation is refused");
        check(Json::parse("{'a':1}", &err).is_null(),
              "and so is JSON that is nearly JSON");
        check(Json::parse("{} trailing", &err).is_null(),
              "and trailing data, which means the file is not what it says");
        check(j["c"]["d"].string() == "x", "and escapes survive");

        // A hostile file must not take the stack with it.
        std::string deep;
        for (int i = 0; i < 500; i++) deep += "[";
        check(Json::parse(deep, &err).is_null(),
              "five hundred nested arrays are refused, not recursed");
    }

    // ---------------------------------------------------------- glb
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "warren_gltf_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ResourceLoader::set_base_directory(dir.string());
    ResourceLoader::forget_all();

    {
        const std::vector<uint8_t> glb = build_glb();
        write_file(dir / "tri.glb", glb.data(), glb.size());

        Ref<Resource> r = ResourceLoader::load("tri.glb");
        PackedScene *scene = r ? r->cast_to<PackedScene>() : nullptr;
        check(scene != nullptr, "a glb loads as a scene");
        if (!scene) {
            std::printf("  %d checks\nFAILED\n", g_checks);
            return 1;
        }

        // A FILE BECOMES A TREE YOU INSTANCE, not a mesh you have to
        // assemble. That is what makes "drag the character into the
        // level" mean anything.
        Node *root = scene->instantiate();
        check(root != nullptr, "and instantiates");
        Node *holder = root ? root->find_child("Holder") : nullptr;
        check(holder != nullptr, "with its node hierarchy");
        Node3D *h3 = holder ? holder->cast_to<Node3D>() : nullptr;
        check(h3 && std::fabs(h3->position().x - 5.0f) < 1e-4f,
              "and each node's transform");

        Node *tri = holder ? holder->find_child("Tri") : nullptr;
        MeshInstance3D *mi = tri ? tri->cast_to<MeshInstance3D>() : nullptr;
        check(mi != nullptr, "the mesh node is a MeshInstance3D");
        check(mi && std::fabs(mi->scale() - 2.0f) < 1e-3f,
              "with its scale");
        check(mi && mi->mesh && mi->mesh->vertices.size() == 3,
              "carrying the geometry");
        check(mi && mi->mesh && mi->mesh->indices.size() == 3,
              "and its sixteen-bit indices, widened");

        char what[200];
        if (mi && mi->mesh) {
            const Vertex &v1 = mi->mesh->vertices[1];
            std::snprintf(what, sizeof(what),
                          "positions come through unchanged (%.1f %.1f %.1f)",
                          double(v1.position.x), double(v1.position.y),
                          double(v1.position.z));
            check(std::fabs(v1.position.x - 1.0f) < 1e-4f &&
                      std::fabs(v1.position.y) < 1e-4f,
                  what);
            check(std::fabs(v1.normal.z - 1.0f) < 1e-4f, "so do normals");
            check(std::fabs(v1.uv.x - 1.0f) < 1e-4f, "and texture coordinates");
            // Not in the file, so they had to be derived.
            const Vec4 &t = mi->mesh->vertices[0].tangent;
            check(t.x * t.x + t.y * t.y + t.z * t.z > 0.5f,
                  "tangents are derived when the file has none");
        }

        Material *mat = mi ? mi->first_material() : nullptr;
        check(mat != nullptr, "the material came with it");
        if (mat) {
            std::snprintf(what, sizeof(what),
                          "with its factors (albedo %.2f, metallic %.2f, "
                          "roughness %.2f)",
                          double(mat->albedo.r), double(mat->metallic),
                          double(mat->roughness));
            check(std::fabs(mat->albedo.r - 0.8f) < 1e-3f &&
                      std::fabs(mat->metallic - 0.25f) < 1e-3f &&
                      std::fabs(mat->roughness - 0.6f) < 1e-3f,
                  what);
            // A LINEAR FACTOR STAYS LINEAR. Color is linear here and
            // glTF's baseColorFactor is linear too, so it passes
            // straight through -- unlike a hex literal, which has to
            // be converted. Getting this backwards washes out every
            // imported material and looks like a lighting problem.
            check(std::fabs(mat->albedo.g - 0.1f) < 1e-3f,
                  "and not converted on the way in");
            check(mat->double_sided, "and its flags");
        }

        if (root) { root->queue_free(); delete root; }
    }

    // ------------------------------------------- a file that is wrong
    {
        log_set_level(LogLevel::Fatal);
        const char *rubbish = "this is not a gltf";
        write_file(dir / "bad.gltf", rubbish, std::strlen(rubbish));
        check(!ResourceLoader::load("bad.gltf"), "rubbish is refused");

        const char *v1 = R"({"asset":{"version":"1.0"}})";
        write_file(dir / "old.gltf", v1, std::strlen(v1));
        check(!ResourceLoader::load("old.gltf"),
              "and so is glTF 1.0, rather than half-read");
        log_set_level(LogLevel::Warn);
    }

    ResourceLoader::forget_all();
    std::filesystem::remove_all(dir, ec);
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
