// Bytes on Frametop Together's links, little-endian (docs/together.md, "The protocol").
//
// Everything a peer sends is untrusted. A reader never reads past its span, and what it hands
// out is bounded: a number is finite and within the limits asked for, and text is UTF-8 with no
// control or direction-changing characters and no longer than asked (it ends up on screen, in
// the join prompt among other places). A read that fails leaves `ok` false from then on, so a
// message's reader checks once, at its end.
//
// Header only, and nothing of the Frame's in it: a PC that sends panels speaks the same bytes.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wire {

// Whether `s` is text a peer may show us: well-formed UTF-8 (no overlong forms, surrogates or
// code points past U+10FFFF), without control characters (C0, DEL, C1), line or paragraph
// separators, or the characters that reorder what follows them.
inline bool IsText(const std::string &s) {
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const uint8_t c = uint8_t(s[i]);
        int more = 0;
        uint32_t cp = c, least = 0;
        if (c < 0x80) more = 0;
        else if (c >= 0xc2 && c < 0xe0) more = 1, cp = c & 0x1f, least = 0x80;
        else if (c >= 0xe0 && c < 0xf0) more = 2, cp = c & 0x0f, least = 0x800;
        else if (c >= 0xf0 && c < 0xf5) more = 3, cp = c & 0x07, least = 0x10000;
        else return false;
        if (size_t(more) > n - i - 1) return false;
        for (int k = 1; k <= more; ++k) {
            const uint8_t t = uint8_t(s[i + k]);
            if ((t & 0xc0) != 0x80) return false;
            cp = cp << 6 | (t & 0x3f);
        }
        if (cp < least || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)) return false;
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return false;
        if (cp == 0x2028 || cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200e || cp == 0x200f) return false;
        i += size_t(more) + 1;
    }
    return true;
}

// Our own text made fit to send: what isn't text is dropped (all of it, if it isn't UTF-8), and
// it is cut to `most` bytes at a character's boundary.
inline std::string Clean(const std::string &s, size_t most) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        while (i + len < s.size() && (uint8_t(s[i + len]) & 0xc0) == 0x80) ++len;
        const std::string one = s.substr(i, len);
        if (out.size() + len > most) break;
        if (IsText(one)) out += one;
        i += len;
    }
    return out;
}

struct In {
    const uint8_t *p;
    size_t left;
    bool ok = true;

    In(const uint8_t *data, size_t n) : p(data), left(n) {}

    bool Take(void *out, size_t n) {
        if (!ok || n > left) return ok = false;
        if (n) std::memcpy(out, p, n);
        p += n, left -= n;
        return true;
    }
    uint8_t U8() { uint8_t v = 0; Take(&v, 1); return v; }
    uint16_t U16() { uint8_t b[2] = {}; Take(b, 2); return uint16_t(b[0] | b[1] << 8); }
    uint32_t U32() { uint8_t b[4] = {}; Take(b, 4); return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; }
    uint64_t U64() { const uint64_t lo = U32(); return lo | uint64_t(U32()) << 32; }
    // A number from `lo` to `hi`. Anything else (not a number, too large) fails the read.
    float F32(float lo, float hi) {
        const uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        if (!ok || !std::isfinite(f) || f < lo || f > hi) return ok = false, lo;
        return f;
    }
    // A number from -1 to 1 sent as 16 bits (a turn's part).
    float Unit16() {
        const float f = float(int16_t(U16())) / 32767.0f;
        return f < -1 ? -1 : f;
    }
    // Text of at most `most` bytes (255 at the very most), its length in a byte before it.
    bool Text(std::string &out, size_t most) {
        const size_t n = U8();
        if (!ok || n > most || n > left) return ok = false;
        out.assign(reinterpret_cast<const char *>(p), n);
        p += n, left -= n;
        return ok = IsText(out);
    }
    // The read went well and used the span up.
    bool done() const { return ok && left == 0; }
};

struct Out {
    std::vector<uint8_t> b;

    void U8(uint8_t v) { b.push_back(v); }
    void U16(uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
    void U32(uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back(uint8_t(v >> (8 * k))); }
    void U64(uint64_t v) { U32(uint32_t(v)), U32(uint32_t(v >> 32)); }
    void F32(float f) { uint32_t u; std::memcpy(&u, &f, 4); U32(u); }
    void Unit16(float f) { U16(uint16_t(int16_t(std::lround(std::fmin(1.0f, std::fmax(-1.0f, std::isfinite(f) ? f : 0.0f)) * 32767.0f)))); }
    void Bytes(const void *data, size_t n) { b.insert(b.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + n); }
    void Text(const std::string &s, size_t most) {
        const std::string t = Clean(s, most < 255 ? most : 255);
        U8(uint8_t(t.size()));
        Bytes(t.data(), t.size());
    }
};

}  // namespace wire
