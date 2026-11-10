#include "bodyfile.h"

#include <cmath>
#include <cstring>

#include "modelbytes.h"

namespace bodyfile {
namespace {

using modelbytes::In;
using modelbytes::Out;
using modelbytes::Tag;

bool Finite(const float *v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100) return false;
    return true;
}

bool ValidMesh(const Body &b, std::string &why) {
    const size_t verts = b.pos.size() / 3, tris = b.idx.size() / 3;
    if (verts < 3 || verts > size_t(kMaxVerts) || b.pos.size() % 3) return why = "the body's vertex count is out of range", false;
    if (tris < 1 || tris > size_t(kMaxTris) || b.idx.size() % 3) return why = "the body's triangle count is out of range", false;
    if (b.nrm.size() != verts * 3 || b.uv.size() != verts * 2 || b.bones.size() != verts * 4 || b.weights.size() != verts * 4)
        return why = "the body's vertex arrays differ in length", false;
    for (const uint16_t i : b.idx)
        if (i >= verts) return why = "a body triangle names a vertex that isn't there", false;
    for (const float t : b.uv)
        if (!std::isfinite(t) || std::fabs(t) > 64) return why = "a texture place isn't a number a body may have", false;
    for (size_t v = 0; v < verts; ++v) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            const int bone = b.bones[4 * v + k], w = b.weights[4 * v + k];
            if (bone >= kRoles || (w && !b.present[bone])) return why = "a body vertex hangs from a bone that isn't there", false;
            sum += w;
        }
        if (sum != 255) return why = "a body vertex's weights don't add up", false;
    }
    return true;
}

bool ValidLook(const Body &b, std::string &why) {
    if (b.parts.empty() || b.parts.size() > size_t(kMaxParts) || b.materials.empty() || b.materials.size() > size_t(kMaxParts))
        return why = "the body's parts or materials are out of range", false;
    uint32_t next = 0;
    for (const Part &p : b.parts) {
        if (p.first != next || !p.count || p.count > uint32_t(b.tris()) - next) return why = "the body's parts don't follow one another", false;
        if (p.material >= b.materials.size()) return why = "a part names a material that isn't there", false;
        next += p.count;
    }
    if (next != uint32_t(b.tris())) return why = "the body's parts don't cover its triangles", false;
    for (const Material &m : b.materials) {
        if (!std::isfinite(m.cutoff) || m.cutoff < 0 || m.cutoff > 1) return why = "a material's cutoff is out of range", false;
        for (const float c : m.color)
            if (!std::isfinite(c) || c < 0 || c > 1) return why = "a material's colour is out of range", false;
        if (m.texture < -1 || m.texture >= int(b.textures.size())) return why = "a material names a texture that isn't there", false;
        if (m.alpha > Material::kBlend || (m.flags & ~uint32_t(Material::kKnownFlags))) return why = "a material is of a kind this Frametop doesn't draw", false;
    }
    if (b.textures.size() > size_t(kMaxTextures)) return why = "too many textures", false;
    size_t bytes = 0;
    for (const Texture &t : b.textures) {
        if (t.width < 1 || t.height < 1 || t.width > kMaxTextureSide || t.height > kMaxTextureSide) return why = "a texture's size is out of range", false;
        if (t.rgba.size() != size_t(t.width) * size_t(t.height) * 4) return why = "a texture's pixels don't match its size", false;
        bytes += t.rgba.size();
    }
    if (bytes > kMaxTextureBytes) return why = "the textures are too large together", false;
    return true;
}

bool ValidShapes(const Body &b, std::string &why) {
    if (b.shapes.size() > size_t(kMaxShapes)) return why = "too many shapes", false;
    uint32_t seen = 0;
    for (const Shape &s : b.shapes) {
        if (s.id < 1 || s.id > Shape::kLastId) return why = "a shape this Frametop doesn't know", false;
        if (seen & (1u << s.id)) return why = "two shapes of one kind", false;
        seen |= 1u << s.id;
        if (s.verts.empty() || s.verts.size() > size_t(b.verts()) || s.delta.size() != s.verts.size() * 3) return why = "a shape's arrays are out of range", false;
        int last = -1;
        for (const uint16_t v : s.verts) {
            if (int(v) <= last || int(v) >= b.verts()) return why = "a shape moves a vertex that isn't there", false;
            last = v;
        }
    }
    return true;
}

bool ReadBody(In in, Body &b, std::string &err) {
    const uint32_t zero = in.U32();
    b.unit = in.F32();
    for (float &e : b.eyes) e = in.F32();
    const uint32_t roles = in.U32(), verts = in.U32(), tris = in.U32(), parts = in.U32(), materials = in.U32();
    if (!in.ok || zero != 0 || roles != uint32_t(kRoles)) return err = "a body of a kind this Frametop doesn't read", false;
    if (verts > uint32_t(kMaxVerts) || tris > uint32_t(kMaxTris) || parts > uint32_t(kMaxParts) || materials > uint32_t(kMaxParts))
        return err = "the body's counts are out of range", false;
    std::vector<float> at;
    std::vector<uint8_t> present;
    std::vector<uint32_t> part;
    in.Array(at, size_t(kRoles) * 3);
    in.Array(present, size_t(kRoles));
    in.Array(b.pos, size_t(verts) * 3);
    in.Array(b.nrm, size_t(verts) * 3);
    in.Array(b.uv, size_t(verts) * 2);
    in.Array(b.bones, size_t(verts) * 4);
    in.Array(b.weights, size_t(verts) * 4);
    in.Array(b.idx, size_t(tris) * 3);
    in.Array(part, size_t(parts) * 3);
    if (!in.ok) return err = "the body runs past its end", false;
    for (int r = 0; r < kRoles; ++r) {
        if (present[r] > 1) return err = "the body's skeleton is damaged", false;
        b.present[r] = present[r] == 1;
        std::memcpy(b.at[r], &at[size_t(r) * 3], sizeof b.at[r]);
    }
    for (uint32_t i = 0; i < parts; ++i) b.parts.push_back({part[3 * i], part[3 * i + 1], part[3 * i + 2]});
    for (uint32_t i = 0; i < materials; ++i) {
        Material m;
        for (float &c : m.color) c = in.F32();
        m.texture = int(int32_t(in.U32()));
        m.alpha = in.U32();
        m.cutoff = in.F32();
        m.flags = in.U32();
        b.materials.push_back(m);
    }
    if (!in.ok || in.left != 0) return err = "the body's size doesn't match its counts", false;
    return true;
}

bool ReadTexture(In in, Body &b, std::string &err) {
    if (b.textures.size() >= size_t(kMaxTextures)) return err = "too many textures", false;
    Texture t;
    t.width = in.U16(), t.height = in.U16();
    const uint32_t format = in.U32();
    if (!in.ok || format != 0) return err = "a texture of a kind this Frametop doesn't read", false;
    if (t.width < 1 || t.height < 1 || t.width > kMaxTextureSide || t.height > kMaxTextureSide) return err = "a texture's size is out of range", false;
    if (!in.Array(t.rgba, size_t(t.width) * size_t(t.height) * 4) || in.left != 0) return err = "a texture's pixels don't match its size", false;
    b.textures.push_back(std::move(t));
    return true;
}

bool ReadShape(In in, Body &b, std::string &err) {
    if (b.shapes.size() >= size_t(kMaxShapes)) return err = "too many shapes", false;
    Shape s;
    s.id = in.U32();
    const uint32_t count = in.U32();
    if (!in.ok || count > uint32_t(kMaxVerts)) return err = "a shape's arrays are out of range", false;
    in.Array(s.verts, count);
    in.Array(s.delta, size_t(count) * 3);
    if (!in.ok || in.left != 0) return err = "a shape's size doesn't match its count", false;
    b.shapes.push_back(std::move(s));
    return true;
}

}  // namespace

const char *const kRoleNames[kRoles] = {
    "hips", "spine", "chest", "upperChest", "neck", "head", "leftEye", "rightEye", "jaw",
    "leftShoulder", "leftUpperArm", "leftLowerArm", "leftHand",
    "rightShoulder", "rightUpperArm", "rightLowerArm", "rightHand",
    "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes",
    "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes",
    "leftThumbMetacarpal", "leftThumbProximal", "leftThumbDistal",
    "leftIndexProximal", "leftIndexIntermediate", "leftIndexDistal",
    "leftMiddleProximal", "leftMiddleIntermediate", "leftMiddleDistal",
    "leftRingProximal", "leftRingIntermediate", "leftRingDistal",
    "leftLittleProximal", "leftLittleIntermediate", "leftLittleDistal",
    "rightThumbMetacarpal", "rightThumbProximal", "rightThumbDistal",
    "rightIndexProximal", "rightIndexIntermediate", "rightIndexDistal",
    "rightMiddleProximal", "rightMiddleIntermediate", "rightMiddleDistal",
    "rightRingProximal", "rightRingIntermediate", "rightRingDistal",
    "rightLittleProximal", "rightLittleIntermediate", "rightLittleDistal",
};

int RoleParent(int role) {
    static const int8_t trunk[kLeftFingers] = {
        -1, kHips, kSpine, kChest, kUpperChest, kNeck, kHead, kHead, kHead,
        kUpperChest, kLeftShoulder, kLeftUpperArm, kLeftLowerArm,
        kUpperChest, kRightShoulder, kRightUpperArm, kRightLowerArm,
        kHips, kLeftUpperLeg, kLeftLowerLeg, kLeftFoot,
        kHips, kRightUpperLeg, kRightLowerLeg, kRightFoot,
    };
    if (role < 0 || role >= kRoles) return -1;
    if (role < kLeftFingers) return trunk[role];
    const bool right = role >= kRightFingers;
    const int k = role - (right ? kRightFingers : kLeftFingers);   // three bones a finger
    return k % 3 == 0 ? (right ? kRightHand : kLeftHand) : role - 1;
}

bool RoleNeeded(int role) {
    switch (role) {
    case kHips: case kSpine: case kHead:
    case kLeftUpperArm: case kLeftLowerArm: case kLeftHand: case kRightUpperArm: case kRightLowerArm: case kRightHand:
    case kLeftUpperLeg: case kLeftLowerLeg: case kLeftFoot: case kRightUpperLeg: case kRightLowerLeg: case kRightFoot:
        return true;
    default:
        return false;
    }
}

int Body::Parent(int role) const {
    for (int up = RoleParent(role); up >= 0; up = RoleParent(up))
        if (present[up]) return up;
    return -1;
}

bool Valid(const Body &b, std::string &why) {
    if (!std::isfinite(b.unit) || b.unit < 1e-6f || b.unit > 1e-2f) return why = "the body's position unit is out of range", false;
    if (!Finite(b.eyes, 3)) return why = "the body's eyes aren't at a place", false;
    for (int r = 0; r < kRoles; ++r) {
        if (!b.present[r] && RoleNeeded(r)) return why = std::string("the body has no ") + kRoleNames[r], false;
        if (!Finite(b.at[r], 3)) return why = "a bone isn't at a place", false;
    }
    return ValidMesh(b, why) && ValidLook(b, why) && ValidShapes(b, why);
}

bool Read(const std::vector<modelfile::Chunk> &chunks, Body &out, bool &has, std::string &err) {
    out = Body();
    has = false;
    for (const modelfile::Chunk &c : chunks) {
        const In in = {c.data, c.size};
        if (c.tag == Tag("BODY")) {
            if (has) return err = "two bodies", false;
            has = true;
            if (!ReadBody(in, out, err)) return false;
        } else if (c.tag == Tag("TEXR")) {
            if (!ReadTexture(in, out, err)) return false;
        } else if (c.tag == Tag("SHAP")) {
            if (!ReadShape(in, out, err)) return false;
        }
    }
    if (!has) {
        if (!out.textures.empty() || !out.shapes.empty()) return err = "textures or shapes without a body", false;
        return true;
    }
    return Valid(out, err);
}

bool ReadFile(const std::string &path, modelfile::Model &model, Body &body, bool &hasBody, std::string &err) {
    std::vector<uint8_t> data;
    std::vector<modelfile::Chunk> chunks;
    return modelfile::ReadBytes(path, data, err) && modelfile::Read(data.data(), data.size(), model, err) &&
           modelfile::Chunks(data.data(), data.size(), chunks, err) && Read(chunks, body, hasBody, err);
}

std::vector<modelfile::Extra> Write(const Body &b) {
    std::vector<modelfile::Extra> out;
    auto add = [&](const char *tag, Out &o) {
        modelfile::Extra e;
        std::memcpy(e.tag, tag, 4);
        e.bytes = std::move(o.b);
        out.push_back(std::move(e));
    };
    {
        Out o;
        o.U32(0);
        o.F32(b.unit);
        for (const float e : b.eyes) o.F32(e);
        o.U32(uint32_t(kRoles));
        o.U32(uint32_t(b.verts()));
        o.U32(uint32_t(b.tris()));
        o.U32(uint32_t(b.parts.size()));
        o.U32(uint32_t(b.materials.size()));
        for (int r = 0; r < kRoles; ++r)
            for (const float c : b.at[r]) o.F32(c);
        for (int r = 0; r < kRoles; ++r) o.U8(b.present[r] ? 1 : 0);
        o.Pad();
        o.Array(b.pos);
        o.Array(b.nrm);
        o.Array(b.uv);
        o.Array(b.bones);
        o.Array(b.weights);
        o.Array(b.idx);
        for (const Part &p : b.parts) o.U32(p.first), o.U32(p.count), o.U32(p.material);
        for (const Material &m : b.materials) {
            for (const float c : m.color) o.F32(c);
            o.U32(uint32_t(int32_t(m.texture)));
            o.U32(m.alpha);
            o.F32(m.cutoff);
            o.U32(m.flags);
        }
        add("BODY", o);
    }
    for (const Texture &t : b.textures) {
        Out o;
        o.U16(uint16_t(t.width)), o.U16(uint16_t(t.height));
        o.U32(0);
        o.Array(t.rgba);
        add("TEXR", o);
    }
    for (const Shape &s : b.shapes) {
        Out o;
        o.U32(s.id);
        o.U32(uint32_t(s.verts.size()));
        o.Array(s.verts);
        o.Array(s.delta);
        add("SHAP", o);
    }
    return out;
}

}  // namespace bodyfile
