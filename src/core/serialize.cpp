#include "core/serialize.h"

#include <cstring>

#include "core/log.h"

namespace wr {

void ByteWriter::raw(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    bytes.insert(bytes.end(), b, b + n);
}

void ByteWriter::str(const std::string &s) {
    // A length that cannot be trusted on the way back in is the
    // classic hole; capping it at write time keeps the reader's
    // check to one comparison.
    const uint32_t n = uint32_t(s.size() > 0xFFFF ? 0xFFFF : s.size());
    u32(n);
    if (n) raw(s.data(), n);
}

void ByteWriter::variant(const Variant &v) {
    u8(uint8_t(v.type()));
    switch (v.type()) {
        case VType::Nil: break;
        case VType::Bool: u8(v.to_bool() ? 1 : 0); break;
        case VType::Int: i64(v.to_int()); break;
        case VType::Float: f32(v.to_float()); break;
        case VType::String: str(v.to_string()); break;
        case VType::Vec2: { Vec2 a = v.to_vec2(); f32(a.x); f32(a.y); break; }
        case VType::Vec3: { Vec3 a = v.to_vec3(); f32(a.x); f32(a.y); f32(a.z); break; }
        case VType::Vec4: { Vec4 a = v.to_vec4(); f32(a.x); f32(a.y); f32(a.z); f32(a.w); break; }
        case VType::Color: { Color c = v.to_color(); f32(c.r); f32(c.g); f32(c.b); f32(c.a); break; }
        case VType::Quat: { Quat q = v.to_quat(); f32(q.x); f32(q.y); f32(q.z); f32(q.w); break; }
        case VType::Basis: {
            Basis b = v.to_basis();
            for (int c = 0; c < 3; c++) {
                Vec3 col = b.col[c];
                f32(col.x); f32(col.y); f32(col.z);
            }
            break;
        }
        case VType::Transform: {
            Transform3D t = v.to_transform();
            for (int c = 0; c < 3; c++) {
                Vec3 col = t.basis.col[c];
                f32(col.x); f32(col.y); f32(col.z);
            }
            f32(t.origin.x); f32(t.origin.y); f32(t.origin.z);
            break;
        }
        case VType::Plane: {
            Plane p = v.to_plane();
            f32(p.normal.x); f32(p.normal.y); f32(p.normal.z); f32(p.d);
            break;
        }
        case VType::AABB: {
            AABB a = v.to_aabb();
            f32(a.min.x); f32(a.min.y); f32(a.min.z);
            f32(a.max.x); f32(a.max.y); f32(a.max.z);
            break;
        }
        case VType::Rect2: {
            Rect2 r = v.to_rect2();
            f32(r.position.x); f32(r.position.y);
            f32(r.size.x); f32(r.size.y);
            break;
        }
        case VType::Projection: {
            Projection p = v.to_projection();
            for (int i = 0; i < 16; i++) f32(p.data()[i]);
            break;
        }
        case VType::Object:
            // A POINTER MEANS NOTHING TO THE OTHER END. Anything
            // that has to send a reference sends an agreed id.
            break;
        case VType::Array: {
            const Array *a = v.array_ptr();
            const uint32_t n = a ? uint32_t(a->size()) : 0;
            u32(n);
            for (uint32_t i = 0; i < n; i++) variant((*a)[i]);
            break;
        }
        case VType::Dict: {
            const Dict *d = v.dict_ptr();
            u32(d ? uint32_t(d->size()) : 0);
            if (d)
                for (const auto &kv : *d) {
                    str(kv.first);
                    variant(kv.second);
                }
            break;
        }
        default: break;
    }
}

bool ByteReader::raw(void *out, size_t n) {
    if (!ok_ || left_ < n) {
        ok_ = false;
        std::memset(out, 0, n);
        return false;
    }
    std::memcpy(out, p_, n);
    p_ += n;
    left_ -= n;
    return true;
}

std::string ByteReader::str() {
    const uint32_t n = u32();
    if (!ok_ || n > left_) {
        ok_ = false;
        return {};
    }
    std::string s((const char *)p_, n);
    p_ += n;
    left_ -= n;
    return s;
}

Variant ByteReader::variant() {
    const uint8_t tag = u8();
    if (!ok_ || tag >= uint8_t(VType::Count)) {
        ok_ = false;
        return {};
    }
    switch (VType(tag)) {
        case VType::Nil: return {};
        case VType::Bool: return Variant(u8() != 0);
        case VType::Int: return Variant(i64());
        case VType::Float: return Variant(double(f32()));
        case VType::String: return Variant(str());
        case VType::Vec2: { float x = f32(), y = f32(); return Variant(Vec2(x, y)); }
        case VType::Vec3: { float x = f32(), y = f32(), z = f32(); return Variant(Vec3(x, y, z)); }
        case VType::Vec4: { float x = f32(), y = f32(), z = f32(), w = f32(); return Variant(Vec4(x, y, z, w)); }
        case VType::Color: { float r = f32(), g = f32(), b = f32(), a = f32(); return Variant(Color(r, g, b, a)); }
        case VType::Quat: { float x = f32(), y = f32(), z = f32(), w = f32(); return Variant(Quat(x, y, z, w)); }
        case VType::Basis: {
            Vec3 c[3];
            for (int i = 0; i < 3; i++) {
                c[i].x = f32(); c[i].y = f32(); c[i].z = f32();
            }
            return Variant(Basis(c[0], c[1], c[2]));
        }
        case VType::Transform: {
            Vec3 c[3];
            for (int i = 0; i < 3; i++) {
                c[i].x = f32(); c[i].y = f32(); c[i].z = f32();
            }
            Vec3 o;
            o.x = f32(); o.y = f32(); o.z = f32();
            return Variant(Transform3D(Basis(c[0], c[1], c[2]), o));
        }
        case VType::Plane: {
            Vec3 n;
            n.x = f32(); n.y = f32(); n.z = f32();
            return Variant(Plane(n, f32()));
        }
        case VType::AABB: {
            Vec3 lo, hi;
            lo.x = f32(); lo.y = f32(); lo.z = f32();
            hi.x = f32(); hi.y = f32(); hi.z = f32();
            return Variant(AABB(lo, hi));
        }
        case VType::Rect2: {
            float x = f32(), y = f32(), w = f32(), h = f32();
            return Variant(Rect2(x, y, w, h));
        }
        case VType::Projection: {
            Projection p;
            for (int i = 0; i < 16; i++) const_cast<float *>(p.data())[i] = f32();
            return Variant(p);
        }
        case VType::Object: return Variant((Object *)nullptr);
        case VType::Array: {
            const uint32_t n = u32();
            // A length is an allocation request from whoever sent
            // the packet, so it is bounded by what is actually left
            // to read rather than by trust.
            if (!ok_ || n > left_) { ok_ = false; return {}; }
            Array a;
            a.reserve(n);
            for (uint32_t i = 0; i < n && ok_; i++) a.push_back(variant());
            return Variant(a);
        }
        case VType::Dict: {
            const uint32_t n = u32();
            if (!ok_ || n > left_) { ok_ = false; return {}; }
            Dict d;
            for (uint32_t i = 0; i < n && ok_; i++) {
                std::string k = str();
                d[k] = variant();
            }
            return Variant(d);
        }
        default: ok_ = false; return {};
    }
}

}  // namespace wr
