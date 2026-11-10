// See modelfile.h.
#include "modelfile.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace modelfile {
namespace {

constexpr uint32_t kVersion = 1;
constexpr int kMaxChunks = 64;
constexpr float kMaxBind = 0.32f;   // a joint's furthest from the origin, metres: what positions reach

uint32_t Tag(const char *s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; }

// Reads forward through a span, never past its end: a read that would is refused and `ok`
// stays false from then on.
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
    template <typename T> bool Array(std::vector<T> &out, size_t count) {   // of one-byte or LE two-byte items
        const size_t bytes = count * sizeof(T), padded = (bytes + 3) & ~size_t(3);
        if (!ok || padded > left) return ok = false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (sizeof(T) == 1) out[i] = T(p[i]);
            else out[i] = T(uint16_t(p[2 * i] | p[2 * i + 1] << 8));
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
        for (const T x : v) {
            if (sizeof(T) == 1) U8(uint8_t(x));
            else U16(uint16_t(x));
        }
        Pad();
    }
    void Bytes(const std::string &s) { b.insert(b.end(), s.begin(), s.end()); }
};

// Text a file may carry: UTF-8 is not checked, but nothing a terminal or a label would act on.
bool PlainText(const std::string &s) {
    for (const unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

}  // namespace

std::string Plain(const std::string &text) {
    std::string out;
    for (const unsigned char c : text)
        if (c >= 0x20 && c != 0x7f) out += char(c);
    if (out.size() > kMaxText) {
        size_t cut = kMaxText;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;   // not inside a character
        out.resize(cut);
    }
    return out;
}

namespace {

bool ReadInfo(In in, Info &info, std::string &err) {
    while (in.left > 0) {
        const uint8_t key = in.U8();
        const uint16_t len = in.U16();
        std::string text(len, '\0');
        if (!in.Take(text.data(), len)) return err = "INFO runs past its end", false;
        if (key == 5) {
            if (len != 1 || uint8_t(text[0]) > 1) return err = "INFO's share flag isn't 0 or 1", false;
            info.mayShare = text[0] == 1;
            continue;
        }
        if (len > kMaxText || !PlainText(text)) return err = "INFO has text that is too long or not plain", false;
        if (key == 1) info.name = text;
        else if (key == 2) info.licence = text;
        else if (key == 3) info.author = text;
        else if (key == 4) info.format = text;
    }
    return true;
}

bool ReadHand(In in, Model &m, std::string &err) {
    const uint8_t side = in.U8();
    uint8_t zero[3] = {1, 1, 1};
    in.Take(zero, 3);
    const uint32_t verts = in.U32(), tris = in.U32();
    if (!in.ok || side > 1 || zero[0] || zero[1] || zero[2]) return err = "HAND's header is wrong", false;
    if (m.hasHand[side]) return err = "two hands of one side", false;
    if (verts < 3 || verts > uint32_t(kMaxHandVerts) || tris < 1 || tris > uint32_t(kMaxHandTris))
        return err = "a hand has too many or too few vertices or triangles", false;
    Hand &h = m.hands[side];
    for (auto &joint : h.bind)
        for (float &c : joint) c = in.F32();
    if (!in.Array(h.pos, size_t(verts) * 3) || !in.Array(h.nrm, size_t(verts) * 3) || !in.Array(h.joints, size_t(verts) * 4) ||
        !in.Array(h.weights, size_t(verts) * 4) || !in.Array(h.idx, size_t(tris) * 3))
        return err = "HAND runs past its end", false;
    if (in.left != 0) return err = "HAND has bytes after its arrays", false;
    if (!Valid(h, err)) return false;
    m.hasHand[side] = true;
    return true;
}

}  // namespace

bool Valid(const Hand &h, std::string &why) {
    const size_t verts = h.pos.size() / 3;
    if (h.pos.size() % 3 || h.idx.size() % 3 || verts < 3 || verts > size_t(kMaxHandVerts) || h.idx.size() < 3 ||
        h.idx.size() / 3 > size_t(kMaxHandTris))
        return why = "a hand has too many or too few vertices or triangles", false;
    if (h.nrm.size() != verts * 3 || h.joints.size() != verts * 4 || h.weights.size() != verts * 4)
        return why = "a hand's arrays don't match its vertices", false;
    for (const auto &joint : h.bind)
        for (const float c : joint)
            if (!std::isfinite(c) || std::fabs(c) > kMaxBind) return why = "a hand's joint is out of range", false;
    for (size_t v = 0; v < verts; ++v) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            if (h.joints[v * 4 + k] >= kHandJoints) return why = "a vertex names a joint a hand doesn't have", false;
            sum += h.weights[v * 4 + k];
        }
        if (sum != 255) return why = "a vertex's weights don't add up", false;
    }
    for (const uint16_t i : h.idx)
        if (i >= verts) return why = "a triangle names a vertex the hand doesn't have", false;
    return true;
}

bool NameOk(const std::string &name) {
    if (name.empty() || name.size() > size_t(kMaxName)) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

std::string NameFrom(const std::string &text) {
    std::string name;
    for (const char c : text) {
        if (name.size() >= size_t(kMaxName)) break;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') name += c;
        else if (c >= 'A' && c <= 'Z') name += char(c - 'A' + 'a');
        else if (!name.empty() && name.back() != '-') name += '-';
    }
    while (!name.empty() && name.back() == '-') name.pop_back();
    return name.empty() ? "model" : name;
}

std::string Folder() {
    if (const char *dir = std::getenv("FT_AVATARS_DIR"); dir && *dir) return dir;
    const char *data = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
    const std::string base = data && *data ? data : std::string(home ? home : "") + "/.local/share";
    return base + "/frametop/avatars";
}

bool Read(const uint8_t *data, size_t size, Model &out, std::string &err) {
    out = Model();
    if (size > kMaxFileBytes) return err = "the file is too large", false;
    In in = {data, size};
    char magic[4] = {};
    in.Take(magic, 4);
    const uint32_t version = in.U32();
    if (!in.ok || std::memcmp(magic, "FTMD", 4) != 0) return err = "not a Frametop model", false;
    if (version != kVersion) return err = "a Frametop model of a version this Frametop doesn't read", false;
    bool info = false;
    for (int chunks = 0; in.left > 0; ++chunks) {
        if (chunks >= kMaxChunks) return err = "too many chunks", false;
        const uint32_t tag = in.U32(), len = in.U32();
        const size_t padded = (size_t(len) + 3) & ~size_t(3);
        if (!in.ok || padded > in.left) return err = "a chunk runs past the file's end", false;
        const In body = {in.p, len};
        in.p += padded;
        in.left -= padded;
        if (tag == Tag("INFO")) {
            if (info) return err = "two INFO chunks", false;
            info = true;
            if (!ReadInfo(body, out.info, err)) return false;
        } else if (tag == Tag("HAND")) {
            if (!ReadHand(body, out, err)) return false;
        }
    }
    if (!out.hasHand[0] && !out.hasHand[1]) return err = "the model has nothing this Frametop draws", false;
    return true;
}

bool ReadFile(const std::string &path, Model &out, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return err = path + ": " + std::strerror(errno), false;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() <= kMaxFileBytes) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return Read(data.data(), data.size(), out, err);
}

std::vector<uint8_t> Write(const Model &m) {
    Out o;
    o.Bytes("FTMD");
    o.U32(kVersion);
    auto chunk = [&o](const char *tag, const Out &body) {
        o.Bytes(std::string(tag, 4));
        o.U32(uint32_t(body.b.size()));
        o.b.insert(o.b.end(), body.b.begin(), body.b.end());
        o.Pad();
    };
    Out info;
    auto text = [&info](uint8_t key, const std::string &s) {
        const std::string cut = Plain(s);
        if (cut.empty()) return;
        info.U8(key);
        info.U16(uint16_t(cut.size()));
        info.Bytes(cut);
    };
    text(1, m.info.name);
    text(2, m.info.licence);
    text(3, m.info.author);
    text(4, m.info.format);
    info.U8(5);
    info.U16(1);
    info.U8(m.info.mayShare ? 1 : 0);
    chunk("INFO", info);
    for (int side = 0; side < 2; ++side) {
        if (!m.hasHand[side]) continue;
        const Hand &h = m.hands[side];
        Out b;
        b.U8(uint8_t(side));
        b.U8(0), b.U8(0), b.U8(0);
        b.U32(uint32_t(h.verts()));
        b.U32(uint32_t(h.tris()));
        for (const auto &joint : h.bind)
            for (const float c : joint) b.F32(c);
        b.Array(h.pos);
        b.Array(h.nrm);
        b.Array(h.joints);
        b.Array(h.weights);
        b.Array(h.idx);
        chunk("HAND", b);
    }
    return o.b;
}

bool WriteFile(const std::string &path, const Model &m, std::string &err) {
    const std::vector<uint8_t> data = Write(m);
    const std::string tmp = path + ".part";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return err = tmp + ": " + std::strerror(errno), false;
    const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !wrote || std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = path + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace modelfile
