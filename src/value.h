// A parsed Unity object: a tree of values read through the file's type tree.
// Arrays of primitives stay as spans into the (decompressed) file data.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xl {

enum class Kind : uint8_t { Null, Bool, Int, UInt, Float, String, Bytes, Array, Object };
enum class Prim : uint8_t { None, U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Bool };

inline size_t prim_size(Prim p) {
    switch (p) {
    case Prim::U8: case Prim::I8: case Prim::Bool: return 1;
    case Prim::U16: case Prim::I16: return 2;
    case Prim::U32: case Prim::I32: case Prim::F32: return 4;
    case Prim::U64: case Prim::I64: case Prim::F64: return 8;
    default: return 0;
    }
}

struct Value {
    Kind kind = Kind::Null;
    Prim prim = Prim::None;  // element type of Bytes
    bool big = false;        // Bytes endianness
    union {
        int64_t i;
        uint64_t u;
        double f;
    };
    std::string str;
    const uint8_t* data = nullptr;
    size_t size = 0;  // Bytes: byte length
    std::vector<Value> items;
    std::vector<std::pair<std::string_view, Value>> fields;

    Value() : i(0) {}

    static const Value& null() {
        static const Value v;
        return v;
    }

    bool is_null() const { return kind == Kind::Null; }
    bool is_object() const { return kind == Kind::Object; }

    const Value& operator[](std::string_view name) const {
        if (kind == Kind::Object)
            for (auto& f : fields)
                if (f.first == name) return f.second;
        return null();
    }
    const Value& operator[](size_t index) const {
        if (kind == Kind::Array && index < items.size()) return items[index];
        return null();
    }
    bool has(std::string_view name) const { return !(*this)[name].is_null(); }

    double num(double fallback = 0.0) const {
        switch (kind) {
        case Kind::Bool: case Kind::Int: return (double)i;
        case Kind::UInt: return (double)u;
        case Kind::Float: return f;
        default: return fallback;
        }
    }
    int64_t i64(int64_t fallback = 0) const {
        switch (kind) {
        case Kind::Bool: case Kind::Int: return i;
        case Kind::UInt: return (int64_t)u;
        case Kind::Float: return (int64_t)f;
        default: return fallback;
        }
    }
    bool truthy() const { return i64(0) != 0; }
    const std::string& s() const { return str; }

    // Element count of an Array or a primitive Bytes array.
    size_t count() const {
        if (kind == Kind::Array) return items.size();
        if (kind == Kind::Bytes) return prim_size(prim) ? size / prim_size(prim) : size;
        return 0;
    }
    // Element i of a primitive array, as double.
    double elem(size_t index) const {
        if (kind == Kind::Array) return (*this)[index].num();
        if (kind != Kind::Bytes) return 0.0;
        size_t es = prim_size(prim);
        if (es == 0 || (index + 1) * es > size) return 0.0;
        const uint8_t* p = data + index * es;
        uint64_t raw = 0;
        std::memcpy(&raw, p, es);
        if (big) {
            if (es == 2) raw = _byteswap_ushort((uint16_t)raw);
            else if (es == 4) raw = _byteswap_ulong((uint32_t)raw);
            else if (es == 8) raw = _byteswap_uint64(raw);
        }
        switch (prim) {
        case Prim::U8: case Prim::Bool: return (double)(uint8_t)raw;
        case Prim::I8: return (double)(int8_t)raw;
        case Prim::U16: return (double)(uint16_t)raw;
        case Prim::I16: return (double)(int16_t)raw;
        case Prim::U32: return (double)(uint32_t)raw;
        case Prim::I32: return (double)(int32_t)raw;
        case Prim::U64: return (double)raw;
        case Prim::I64: return (double)(int64_t)raw;
        case Prim::F32: { uint32_t b = (uint32_t)raw; float fl; std::memcpy(&fl, &b, 4); return fl; }
        case Prim::F64: { double d; std::memcpy(&d, &raw, 8); return d; }
        default: return 0.0;
        }
    }
};

// Reads common small structs.
struct Vec2 { double x = 0, y = 0; };
struct Vec3v { double x = 0, y = 0, z = 0; };
struct Vec4v { double x = 0, y = 0, z = 0, w = 0; };
struct Color { double r = 1, g = 1, b = 1, a = 1; };

inline Vec2 as_vec2(const Value& v) { return {v["x"].num(), v["y"].num()}; }
inline Vec3v as_vec3(const Value& v) { return {v["x"].num(), v["y"].num(), v["z"].num()}; }
inline Vec4v as_vec4(const Value& v) { return {v["x"].num(), v["y"].num(), v["z"].num(), v["w"].num()}; }
inline Color as_color(const Value& v) {
    if (v.is_null()) return {};
    return {v["r"].num(1), v["g"].num(1), v["b"].num(1), v["a"].num(1)};
}

} // namespace xl
