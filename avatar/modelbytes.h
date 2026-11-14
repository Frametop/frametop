// Bytes in and out of a Frametop model file (modelfile.h), little-endian: what its chunks'
// readers and writers share. A reader never reads past its span's end.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace modelbytes {

inline uint32_t Tag(const char *s) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

// Reads forward through a span: a read that would pass its end is refused, and `ok` stays false
// from then on.
struct In {
    const uint8_t *p;
    size_t left;
    bool ok = true;
    bool Take(void *out, size_t n) {
        if (!ok || n > left) return ok = false;
        if (n) std::memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    uint8_t U8() { uint8_t v = 0; Take(&v, 1); return v; }
    uint16_t U16() { uint8_t b[2] = {}; Take(b, 2); return uint16_t(b[0] | b[1] << 8); }
    uint32_t U32() { uint8_t b[4] = {}; Take(b, 4); return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; }
    float F32() { const uint32_t u = U32(); float f; std::memcpy(&f, &u, 4); return f; }
    // `count` items of one, two (LE) or four (LE; floats too) bytes, then zeros to a multiple of four.
    template <typename T> bool Array(std::vector<T> &out, size_t count) {
        if (count > left / sizeof(T)) return ok = false;
        const size_t bytes = count * sizeof(T), padded = (bytes + 3) & ~size_t(3);
        if (!ok || padded > left) return ok = false;
        out.resize(count);
        if (sizeof(T) == 1 && count) std::memcpy(out.data(), p, count);
        for (size_t i = 0; sizeof(T) > 1 && i < count; ++i) {
            if (sizeof(T) == 2) out[i] = T(uint16_t(p[2 * i] | p[2 * i + 1] << 8));
            else {
                const uint32_t u = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
                std::memcpy(&out[i], &u, 4);
            }
        }
        p += padded;
        left -= padded;
        return true;
    }
};

struct Out {
    std::vector<uint8_t> b;
    void U8(uint8_t v) { b.push_back(v); }
    void U16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    void U32(uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back(uint8_t(v >> (8 * k))); }
    void F32(float f) { uint32_t u; std::memcpy(&u, &f, 4); U32(u); }
    void Pad() { while (b.size() % 4) b.push_back(0); }
    template <typename T> void Array(const std::vector<T> &v) {
        if (sizeof(T) == 1) b.insert(b.end(), reinterpret_cast<const uint8_t *>(v.data()), reinterpret_cast<const uint8_t *>(v.data()) + v.size());
        for (const T x : v) {
            if (sizeof(T) == 1) break;
            if (sizeof(T) == 2) U16(uint16_t(x));
            else { uint32_t u; std::memcpy(&u, &x, 4); U32(u); }
        }
        Pad();
    }
    void Bytes(const std::string &s) { b.insert(b.end(), s.begin(), s.end()); }
};

}  // namespace modelbytes
