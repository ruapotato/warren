#include "agent/variant_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/object.h"
#include "scene/node.h"
#include "scene/nodes.h"

namespace wr {
namespace {

Json vec_json(const float *v, int n) {
    static const char *kNames[4] = {"x", "y", "z", "w"};
    Json j = Json::object();
    for (int i = 0; i < n; i++) j.set(kNames[i], double(v[i]));
    return j;
}

Json rgba_json(const Color &c) {
    Json j = Json::object();
    j.set("r", double(c.r)).set("g", double(c.g)).set("b", double(c.b));
    j.set("a", double(c.a));
    return j;
}

Json floats_json(const float *v, int n) {
    Json j = Json::array();
    for (int i = 0; i < n; i++) j.push(Json(double(v[i])));
    return j;
}

// A component by name, then by index, then by a scalar broadcast.
// Three shapes because a caller writes all three and none of them is
// wrong.
bool read_components(const Json &j, const char *const *names, int n,
                     float *out, std::string *error) {
    if (j.type() == Json::Type::Number) {
        for (int i = 0; i < n; i++) out[i] = float(j.number());
        return true;
    }
    if (j.type() == Json::Type::Array) {
        if (int(j.size()) != n) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "expected %d numbers, got %d", n,
                          int(j.size()));
            *error = buf;
            return false;
        }
        for (int i = 0; i < n; i++) out[i] = float(j[i].number());
        return true;
    }
    if (j.type() == Json::Type::Object) {
        for (int i = 0; i < n; i++) {
            const Json &c = j[names[i]];
            // A missing component keeps whatever the caller passed in
            // `out`, so a partial object patches rather than zeroes:
            // {"y": 2} on a position moves it up and leaves x and z.
            if (c.type() == Json::Type::Number) out[i] = float(c.number());
        }
        return true;
    }
    *error = "expected a number, an array of numbers, or an object";
    return false;
}

// The three columns, named, because a bare nine numbers leaves a
// caller guessing whether they are rows or columns and it will guess
// wrong half the time.
Json basis_json(const Basis &b) {
    Json j = Json::object();
    static const char *kAxis[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) j.set(kAxis[i], vec_json(&b.col[i].x, 3));
    return j;
}

const char *const kXYZW[4] = {"x", "y", "z", "w"};

// A rotation in degrees, written any of the three ways a caller
// writes one: by axis name, by the axis the turn is ABOUT (x is
// pitch, which is what every other engine means by it), or as an
// array in that same x, y, z order. Out in radians as yaw, pitch,
// roll, which is what from_euler_yxz takes.
bool read_euler(const Json &j, float *ypr, std::string *error) {
    const float k = 0.017453292519943295f;
    if (j.type() == Json::Type::Object &&
        (j.has("yaw") || j.has("pitch") || j.has("roll"))) {
        static const char *const kYPR[3] = {"yaw", "pitch", "roll"};
        float v[3] = {0, 0, 0};
        if (!read_components(j, kYPR, 3, v, error)) return false;
        for (int i = 0; i < 3; i++) ypr[i] = v[i] * k;
        return true;
    }
    float v[3] = {0, 0, 0};
    if (!read_components(j, kXYZW, 3, v, error)) return false;
    ypr[0] = v[1] * k;  // yaw is about Y
    ypr[1] = v[0] * k;  // pitch is about X
    ypr[2] = v[2] * k;  // roll is about Z
    return true;
}
const char *const kRGBA[4] = {"r", "g", "b", "a"};

}  // namespace

std::string type_label(VType t, const std::string &class_name) {
    if (t == VType::Object) return class_name.empty() ? "Object" : class_name;
    switch (t) {
        case VType::Nil: return "null";
        case VType::Bool: return "bool";
        case VType::Int: return "int";
        case VType::Float: return "float";
        case VType::String: return "string";
        case VType::Vec2: return "vec2";
        case VType::Vec3: return "vec3";
        case VType::Vec4: return "vec4";
        case VType::Color: return "color";
        case VType::Quat: return "quat";
        case VType::Basis: return "basis";
        case VType::Transform: return "transform";
        case VType::Plane: return "plane";
        case VType::AABB: return "aabb";
        case VType::Rect2: return "rect2";
        case VType::Projection: return "projection";
        case VType::Array: return "array";
        case VType::Dict: return "dict";
        default: return vtype_name(t);
    }
}

Json variant_to_json(const Variant &v) {
    switch (v.type()) {
        case VType::Nil: return Json();
        case VType::Bool: return Json(v.to_bool());
        case VType::Int: return Json(double(v.to_int()));
        case VType::Float: return Json(double(v.to_float()));
        case VType::String: return Json(v.to_string());
        case VType::Vec2: { Vec2 a = v.to_vec2(); return vec_json(&a.x, 2); }
        case VType::Vec3: { Vec3 a = v.to_vec3(); return vec_json(&a.x, 3); }
        case VType::Vec4: { Vec4 a = v.to_vec4(); return vec_json(&a.x, 4); }
        case VType::Color: return rgba_json(v.to_color());
        case VType::Quat: {
            Quat q = v.to_quat();
            Json j = Json::object();
            j.set("x", double(q.x)).set("y", double(q.y)).set("z", double(q.z));
            j.set("w", double(q.w));
            return j;
        }
        case VType::Basis: return basis_json(v.to_basis());
        case VType::Transform: {
            // Position and basis are what it is; the euler angles are
            // added because that is what the caller will want to read
            // back after writing them, and a reply that cannot be
            // compared with the request is a reply that invites a
            // second request.
            Transform3D t = v.to_transform();
            Json j = Json::object();
            j.set("position", vec_json(&t.origin.x, 3));
            j.set("basis", basis_json(t.basis));
            // NAMED AXES, because x/y/z here is a trap.
            //
            // to_euler_yxz packs (yaw, pitch, roll) into a Vec3, so
            // the engine's own `euler` property has yaw in its .x --
            // a rotation about Y stored in the X component. That is
            // defensible inside the maths and indefensible on a wire
            // protocol, where a caller reading "x: 90" will write
            // back a pitch. So the wire spells them out.
            const Vec3 e = t.basis.orthonormalized().to_euler_yxz();
            const double deg = 57.29577951308232;
            Json rot = Json::object();
            rot.set("yaw", double(e.x) * deg);
            rot.set("pitch", double(e.y) * deg);
            rot.set("roll", double(e.z) * deg);
            j.set("rotation_degrees", rot);
            j.set("scale", double(t.basis.uniform_scale()));
            return j;
        }
        case VType::Plane: {
            Plane p = v.to_plane();
            Json j = Json::object();
            j.set("normal", vec_json(&p.normal.x, 3)).set("d", double(p.d));
            return j;
        }
        case VType::AABB: {
            AABB b = v.to_aabb();
            Json j = Json::object();
            j.set("min", vec_json(&b.min.x, 3)).set("max", vec_json(&b.max.x, 3));
            return j;
        }
        case VType::Rect2: {
            Rect2 r = v.to_rect2();
            Json j = Json::object();
            j.set("x", double(r.position.x)).set("y", double(r.position.y));
            j.set("width", double(r.size.x)).set("height", double(r.size.y));
            return j;
        }
        case VType::Projection: {
            Projection p = v.to_projection();
            Json j = Json::array();
            for (int i = 0; i < 4; i++) {
                Json row = Json::array();
                for (int k = 0; k < 4; k++) row.push(Json(double(p.m[i][k])));
                j.push(row);
            }
            return j;
        }
        case VType::Object: {
            Object *o = v.to_object();
            if (!o) return Json();
            Json j = Json::object();
            j.set("object", o->get_class_name());
            if (Node *n = o->cast_to<Node>()) j.set("path", n->path());
            return j;
        }
        case VType::Array: {
            Json j = Json::array();
            if (const Array *a = v.array_ptr())
                for (const Variant &e : *a) j.push(variant_to_json(e));
            return j;
        }
        case VType::Dict: {
            Json j = Json::object();
            if (const Dict *d = v.dict_ptr())
                for (const auto &kv : *d) j.set(kv.first, variant_to_json(kv.second));
            return j;
        }
        default: return Json();
    }
}

bool json_to_variant(const Json &j, VType want, Variant *out,
                     std::string *error, const Variant *current) {
    // No declared type: take the JSON at face value. Used for vararg
    // method calls, where reflection has nothing to say.
    if (want == VType::Nil) {
        switch (j.type()) {
            case Json::Type::Null: *out = Variant(); return true;
            case Json::Type::Bool: *out = Variant(j.boolean()); return true;
            case Json::Type::Number: {
                double d = j.number();
                if (d == std::floor(d) && std::fabs(d) < 9e15)
                    *out = Variant(int64_t(d));
                else
                    *out = Variant(float(d));
                return true;
            }
            case Json::Type::String: *out = Variant(j.string()); return true;
            case Json::Type::Array: {
                // Three or four bare numbers is overwhelmingly a
                // vector, and a caller who meant an array of numbers
                // can say so by naming the type.
                if (j.size() == 3 && j[0].type() == Json::Type::Number)
                    return json_to_variant(j, VType::Vec3, out, error);
                Array a;
                for (size_t i = 0; i < j.size(); i++) {
                    Variant e;
                    if (!json_to_variant(j[i], VType::Nil, &e, error)) return false;
                    a.push_back(std::move(e));
                }
                *out = Variant(a);
                return true;
            }
            case Json::Type::Object: {
                Dict d;
                for (const auto &kv : j.fields()) {
                    Variant e;
                    if (!json_to_variant(kv.second, VType::Nil, &e, error)) return false;
                    d[kv.first] = std::move(e);
                }
                *out = Variant(d);
                return true;
            }
        }
    }

    switch (want) {
        case VType::Bool:
            if (j.type() == Json::Type::Bool) { *out = Variant(j.boolean()); return true; }
            if (j.type() == Json::Type::Number) { *out = Variant(j.number() != 0.0); return true; }
            *error = "expected true or false";
            return false;
        case VType::Int:
            if (j.type() == Json::Type::Number) { *out = Variant(int64_t(j.number())); return true; }
            if (j.type() == Json::Type::Bool) { *out = Variant(int64_t(j.boolean())); return true; }
            *error = "expected a number";
            return false;
        case VType::Float:
            if (j.type() == Json::Type::Number) { *out = Variant(float(j.number())); return true; }
            *error = "expected a number";
            return false;
        case VType::String:
            if (j.type() == Json::Type::String) { *out = Variant(j.string()); return true; }
            if (j.type() == Json::Type::Number || j.type() == Json::Type::Bool) {
                *out = Variant(j.to_string());
                return true;
            }
            *error = "expected a string";
            return false;
        case VType::Vec2: {
            float v[2] = {0, 0};
            if (current && current->type() == VType::Vec2) {
                const Vec2 c = current->to_vec2();
                for (int i = 0; i < 2; i++) v[i] = (&c.x)[i];
            }
            if (!read_components(j, kXYZW, 2, v, error)) return false;
            *out = Variant(Vec2(v[0], v[1]));
            return true;
        }
        case VType::Vec3: {
            float v[3] = {0, 0, 0};
            if (current && current->type() == VType::Vec3) {
                const Vec3 c = current->to_vec3();
                for (int i = 0; i < 3; i++) v[i] = (&c.x)[i];
            }
            if (!read_components(j, kXYZW, 3, v, error)) return false;
            *out = Variant(Vec3(v[0], v[1], v[2]));
            return true;
        }
        case VType::Vec4: {
            float v[4] = {0, 0, 0, 0};
            if (current && current->type() == VType::Vec4) {
                const Vec4 c = current->to_vec4();
                for (int i = 0; i < 4; i++) v[i] = (&c.x)[i];
            }
            if (!read_components(j, kXYZW, 4, v, error)) return false;
            *out = Variant(Vec4(v[0], v[1], v[2], v[3]));
            return true;
        }
        case VType::Color: {
            if (j.type() == Json::Type::String) {
                // "#ff8800", "ff8800", or with an alpha byte on the end.
                const std::string &h = j.string();
                const char *c = h.c_str() + (h[0] == '#' ? 1 : 0);
                char *end = nullptr;
                const unsigned long bits = std::strtoul(c, &end, 16);
                const size_t digits = size_t(end - c);
                if ((digits != 6 && digits != 8) || *end) {
                    *error = "expected a colour like \"#ff8800\"";
                    return false;
                }
                *out = digits == 8
                           ? Variant(Color::hex(uint32_t(bits >> 8),
                                                float(bits & 0xff) / 255.0f))
                           : Variant(Color::hex(uint32_t(bits)));
                return true;
            }
            // Alpha defaults to opaque, which is what a caller who
            // wrote three numbers meant.
            float v[4] = {0, 0, 0, 1};
            if (current && current->type() == VType::Color) {
                const Color c = current->to_color();
                v[0] = c.r; v[1] = c.g; v[2] = c.b; v[3] = c.a;
            }
            if (j.type() == Json::Type::Array && j.size() == 3) {
                if (!read_components(j, kRGBA, 3, v, error)) return false;
            } else if (!read_components(j, kRGBA, 4, v, error)) {
                return false;
            }
            *out = Variant(Color(v[0], v[1], v[2], v[3]));
            return true;
        }
        case VType::Quat: {
            // Euler degrees are the readable spelling and the one a
            // caller reaches for; xyzw is the storage.
            if (j.type() == Json::Type::Object && !j.has("w")) {
                float ypr[3] = {0, 0, 0};
                if (!read_euler(j, ypr, error)) return false;
                *out = Variant(Quat::from_euler_yxz(ypr[0], ypr[1], ypr[2]));
                return true;
            }
            float v[4] = {0, 0, 0, 1};
            if (!read_components(j, kXYZW, 4, v, error)) return false;
            *out = Variant(Quat(v[0], v[1], v[2], v[3]));
            return true;
        }
        case VType::Basis: {
            const bool named = j.type() == Json::Type::Object && j.has("x") &&
                               j["x"].type() != Json::Type::Number;
            if (named || (j.type() == Json::Type::Array && j.size() == 3)) {
                Basis b;
                static const char *kAxis[3] = {"x", "y", "z"};
                for (int i = 0; i < 3; i++) {
                    float r[3] = {0, 0, 0};
                    const Json &axis = named ? j[kAxis[i]] : j[i];
                    if (!read_components(axis, kXYZW, 3, r, error)) return false;
                    b.col[i] = Vec3(r[0], r[1], r[2]);
                }
                *out = Variant(b);
                return true;
            }
            Variant q;
            if (!json_to_variant(j, VType::Quat, &q, error)) return false;
            *out = Variant(q.to_quat().to_basis());
            return true;
        }
        case VType::Transform: {
            if (j.type() != Json::Type::Object) {
                *error = "expected an object with position/rotation/scale or basis";
                return false;
            }
            Transform3D t;
            if (j.has("position") || j.has("origin")) {
                float p[3] = {0, 0, 0};
                if (!read_components(j.has("position") ? j["position"] : j["origin"],
                                     kXYZW, 3, p, error))
                    return false;
                t.origin = Vec3(p[0], p[1], p[2]);
            }
            if (j.has("basis")) {
                Variant b;
                if (!json_to_variant(j["basis"], VType::Basis, &b, error)) return false;
                t.basis = b.to_basis();
            } else if (j.has("rotation_degrees") || j.has("rotation")) {
                float ypr[3] = {0, 0, 0};
                if (!read_euler(j.has("rotation_degrees") ? j["rotation_degrees"]
                                                          : j["rotation"],
                                ypr, error))
                    return false;
                t.basis = Basis::from_euler_yxz(ypr[0], ypr[1], ypr[2]);
            }
            if (j.has("scale")) {
                const Json &s = j["scale"];
                if (s.type() == Json::Type::Number) {
                    t.basis = t.basis * float(s.number());
                } else {
                    float v[3] = {1, 1, 1};
                    if (!read_components(s, kXYZW, 3, v, error)) return false;
                    t.basis = t.basis * Basis::scaled(Vec3(v[0], v[1], v[2]));
                }
            }
            *out = Variant(t);
            return true;
        }
        case VType::Plane: {
            if (j.type() != Json::Type::Object || !j.has("normal")) {
                *error = "expected {\"normal\": [x,y,z], \"d\": number}";
                return false;
            }
            float n[3] = {0, 1, 0};
            if (!read_components(j["normal"], kXYZW, 3, n, error)) return false;
            *out = Variant(Plane(Vec3(n[0], n[1], n[2]), float(j["d"].number())));
            return true;
        }
        case VType::AABB: {
            if (j.type() != Json::Type::Object) {
                *error = "expected {\"min\": [x,y,z], \"max\": [x,y,z]}";
                return false;
            }
            float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
            if (!read_components(j["min"], kXYZW, 3, lo, error)) return false;
            if (!read_components(j["max"], kXYZW, 3, hi, error)) return false;
            *out = Variant(AABB(Vec3(lo[0], lo[1], lo[2]), Vec3(hi[0], hi[1], hi[2])));
            return true;
        }
        case VType::Rect2: {
            float v[4] = {0, 0, 0, 0};
            if (j.type() == Json::Type::Object) {
                static const char *const kRect[4] = {"x", "y", "width", "height"};
                if (!read_components(j, kRect, 4, v, error)) return false;
            } else if (!read_components(j, kXYZW, 4, v, error)) {
                return false;
            }
            *out = Variant(Rect2(Vec2(v[0], v[1]), Vec2(v[2], v[3])));
            return true;
        }
        case VType::Array: {
            if (j.type() != Json::Type::Array) { *error = "expected an array"; return false; }
            Array a;
            for (size_t i = 0; i < j.size(); i++) {
                Variant e;
                if (!json_to_variant(j[i], VType::Nil, &e, error)) return false;
                a.push_back(std::move(e));
            }
            *out = Variant(a);
            return true;
        }
        case VType::Dict: {
            if (j.type() != Json::Type::Object) { *error = "expected an object"; return false; }
            Dict d;
            for (const auto &kv : j.fields()) {
                Variant e;
                if (!json_to_variant(kv.second, VType::Nil, &e, error)) return false;
                d[kv.first] = std::move(e);
            }
            *out = Variant(d);
            return true;
        }
        case VType::Object:
            // Resolved by the caller, which is the only one that knows
            // what a path is relative to.
            *error = "expected an object reference";
            return false;
        default:
            *error = std::string("cannot convert to ") + type_label(want);
            return false;
    }
}

}  // namespace wr
