// Bounds-checked little/big endian reader over a byte range.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace xl {

inline uint16_t bswap(uint16_t v) { return _byteswap_ushort(v); }
inline uint32_t bswap(uint32_t v) { return _byteswap_ulong(v); }
inline uint64_t bswap(uint64_t v) { return _byteswap_uint64(v); }

struct Reader {
    const uint8_t* base = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool big = false;

    Reader() = default;
    Reader(const uint8_t* data, size_t length, bool bigEndian = false) : base(data), size(length), big(bigEndian) {}

    void need(size_t n) const {
        if (n > size || pos > size - n) throw std::runtime_error("read past end of data");
    }
    size_t left() const { return pos < size ? size - pos : 0; }
    void seek(size_t p) {
        if (p > size) throw std::runtime_error("seek past end of data");
        pos = p;
    }
    void skip(size_t n) { need(n); pos += n; }
    void align(size_t a) { pos = (pos + a - 1) & ~(a - 1); }

    template <class T> T raw() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, base + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    uint8_t u8() { return raw<uint8_t>(); }
    int8_t i8() { return raw<int8_t>(); }
    bool boolean() { return u8() != 0; }
    uint16_t u16() { auto v = raw<uint16_t>(); return big ? bswap(v) : v; }
    uint32_t u32() { auto v = raw<uint32_t>(); return big ? bswap(v) : v; }
    uint64_t u64() { auto v = raw<uint64_t>(); return big ? bswap(v) : v; }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    int64_t i64() { return (int64_t)u64(); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = u64(); double d; std::memcpy(&d, &u, 8); return d; }

    const uint8_t* bytes(size_t n) {
        need(n);
        const uint8_t* p = base + pos;
        pos += n;
        return p;
    }
    std::string cstr() {
        size_t start = pos;
        while (pos < size && base[pos] != 0) ++pos;
        if (pos >= size) throw std::runtime_error("unterminated string");
        std::string s((const char*)base + start, pos - start);
        ++pos;
        return s;
    }
};

inline float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            mant &= 0x3ff;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000 | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

} // namespace xl
