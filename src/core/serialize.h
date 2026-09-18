// Warren -- Variants to bytes and back.
//
// One writer for the network, the scene file and the undo stack,
// because they want the same thing: a value whose type is known only
// at run time, written compactly and read back exactly.
//
// LITTLE ENDIAN, TAGGED, AND NOT SELF-DESCRIBING BEYOND THE TAG. The
// reader must be told what it is reading into; the tag is there to
// catch a mismatch, not to make the stream browsable. A format that
// can be explored is a format that has to stay stable, and this one
// changes whenever the engine's types do.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/variant.h"

namespace wr {

class ByteWriter {
public:
    std::vector<uint8_t> bytes;

    void raw(const void *p, size_t n);
    void u8(uint8_t v) { raw(&v, 1); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i32(int32_t v) { raw(&v, 4); }
    void i64(int64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string &s);
    // Tag and payload. Object references are written as a zero,
    // because a pointer means nothing to the other end -- whatever
    // needs to send one has to send an id it agreed on instead.
    void variant(const Variant &v);
    size_t size() const { return bytes.size(); }
};

class ByteReader {
public:
    ByteReader(const uint8_t *data, size_t size) : p_(data), left_(size) {}
    explicit ByteReader(const std::vector<uint8_t> &v)
        : p_(v.data()), left_(v.size()) {}

    bool raw(void *out, size_t n);
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    int64_t i64() { int64_t v = 0; raw(&v, 8); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::string str();
    Variant variant();

    // False once anything has run off the end. A truncated or
    // malicious packet must not be able to make the reader invent
    // data, and checking once at the end is cheaper and harder to
    // forget than checking every field.
    bool ok() const { return ok_; }
    size_t left() const { return left_; }

private:
    const uint8_t *p_;
    size_t left_;
    bool ok_ = true;
};

}  // namespace wr
