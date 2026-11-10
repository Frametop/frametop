// An avatar's body in Frametop's model file (avatar/bodyfile.h): what's written reads back the
// same beside the hands, a hand model reads as having no body, and a body that breaks any rule
// of the format is refused whole. A peer may send one, so the second half feeds the reader
// damaged files: each rule broken on its own, every truncation, and every byte changed.
#include "../bodyfile.h"

#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A small body that keeps every rule: a strip of triangles in two parts, every needed bone and
// one finger, two textures and two shapes. Odd counts, so every array needs padding.
static bodyfile::Body body() {
    using namespace bodyfile;
    Body b;
    b.unit = 2e-4f;
    b.eyes[1] = 1.5f, b.eyes[2] = -0.08f;
    for (int r = 0; r < kRoles; ++r) {
        b.present[r] = RoleNeeded(r) || r == kNeck || r == kLeftFingers + 3;
        b.at[r][0] = 0.01f * float(r % 7) - 0.03f, b.at[r][1] = 0.03f * float(r), b.at[r][2] = 0.002f * float(r);
    }
    const int verts = 11;
    for (int v = 0; v < verts; ++v) {
        for (int k = 0; k < 3; ++k) {
            b.pos.push_back(int16_t((v * 371 + k * 113) % 8000 - 4000));
            b.nrm.push_back(int8_t(k == 2 ? -127 : 0));
        }
        b.uv.push_back(float(v) / float(verts)), b.uv.push_back(1.25f - float(v) * 0.1f);
        const uint8_t one = v % 2 ? kHips : kHead, two = v % 3 ? kLeftHand : kLeftFingers + 3;
        b.bones.insert(b.bones.end(), {one, two, uint8_t(kJaw), 0});   // the jaw is absent, at no weight
        b.weights.insert(b.weights.end(), {180, 75, 0, 0});
    }
    for (int v = 0; v + 2 < verts; ++v) b.idx.insert(b.idx.end(), {uint16_t(v), uint16_t(v + 1), uint16_t(v + 2)});
    b.parts = {{0, 4, 1}, {4, 5, 0}};
    b.materials.resize(2);
    b.materials[0].texture = 1, b.materials[0].alpha = Material::kMask, b.materials[0].cutoff = 0.25f;
    b.materials[1].color[0] = 0.5f, b.materials[1].flags = Material::kDoubleSided | Material::kUnlit;
    b.textures.resize(2);
    b.textures[0].width = 3, b.textures[0].height = 2;
    b.textures[1].width = 1, b.textures[1].height = 5;
    for (Texture &t : b.textures)
        for (int i = 0; i < t.width * t.height * 4; ++i) t.rgba.push_back(uint8_t(i * 7 + t.width));
    b.shapes.resize(2);
    b.shapes[0].id = Shape::kAa, b.shapes[0].verts = {1, 4, 9}, b.shapes[0].delta = {1, -2, 3, 4, 5, -6, 7, 8, 9};
    b.shapes[1].id = Shape::kBlink, b.shapes[1].verts = {0}, b.shapes[1].delta = {-30, 0, 30};
    return b;
}

static bool same(const bodyfile::Body &a, const bodyfile::Body &b) {
    bool ok = a.unit == b.unit && !std::memcmp(a.eyes, b.eyes, sizeof a.eyes) && !std::memcmp(a.at, b.at, sizeof a.at) &&
              !std::memcmp(a.present, b.present, sizeof a.present) && a.pos == b.pos && a.nrm == b.nrm && a.uv == b.uv &&
              a.bones == b.bones && a.weights == b.weights && a.idx == b.idx && a.parts.size() == b.parts.size() &&
              a.materials.size() == b.materials.size() && a.textures.size() == b.textures.size() && a.shapes.size() == b.shapes.size();
    for (size_t i = 0; ok && i < a.parts.size(); ++i)
        ok = a.parts[i].first == b.parts[i].first && a.parts[i].count == b.parts[i].count && a.parts[i].material == b.parts[i].material;
    for (size_t i = 0; ok && i < a.materials.size(); ++i) {
        const bodyfile::Material &m = a.materials[i], &n = b.materials[i];
        ok = !std::memcmp(m.color, n.color, sizeof m.color) && m.texture == n.texture && m.alpha == n.alpha && m.cutoff == n.cutoff && m.flags == n.flags;
    }
    for (size_t i = 0; ok && i < a.textures.size(); ++i)
        ok = a.textures[i].width == b.textures[i].width && a.textures[i].height == b.textures[i].height && a.textures[i].rgba == b.textures[i].rgba;
    for (size_t i = 0; ok && i < a.shapes.size(); ++i) ok = a.shapes[i].id == b.shapes[i].id && a.shapes[i].verts == b.shapes[i].verts && a.shapes[i].delta == b.shapes[i].delta;
    return ok;
}

static modelfile::Model facts() {
    modelfile::Model m;
    m.info = {"Test avatar", "CC0-1.0", "someone", "vrm", true};
    return m;
}

static std::vector<uint8_t> file(const bodyfile::Body &b) { return modelfile::Write(facts(), bodyfile::Write(b)); }

// The body in a file's bytes: whether the file reads, and through `has` whether it has a body.
static bool reads(const std::vector<uint8_t> &bytes, bodyfile::Body &out, bool &has, std::string &err) {
    std::vector<modelfile::Chunk> chunks;
    has = false;
    return modelfile::Chunks(bytes.data(), bytes.size(), chunks, err) && bodyfile::Read(chunks, out, has, err);
}

// The file with one of the body's rules broken (the writer doesn't check): refused, and Valid
// says the same of the body itself.
template <typename F> static bool refused(F &&damage, const char *expect) {
    bodyfile::Body b = body(), r;
    damage(b);
    std::string why, direct;
    bool has;
    const bool ok = reads(file(b), r, has, why), valid = bodyfile::Valid(b, direct);
    if (ok || valid || why.find(expect) == std::string::npos) std::printf("      got: %s\n", ok ? "(read)" : why.c_str());
    return !ok && !valid && why.find(expect) != std::string::npos;
}

static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + k] = uint8_t(v >> (8 * k));
}
static size_t chunk(const std::vector<uint8_t> &b, int n) {
    size_t at = 8;
    for (; n > 0; --n) at += 8 + ((size_t(b[at + 4]) | size_t(b[at + 5]) << 8 | size_t(b[at + 6]) << 16 | size_t(b[at + 7]) << 24) + 3) / 4 * 4;
    return at;
}

int main() {
    using namespace bodyfile;
    const Body made = body();
    const std::vector<uint8_t> bytes = file(made);
    {
        std::string why;
        check("the test's body keeps the rules", Valid(made, why));
        Body r;
        bool has = false;
        std::string err;
        check("a body reads back the same", reads(bytes, r, has, err) && has && same(r, made));
        modelfile::Model m;
        check("and the hands' reader takes the file, skipping the body", modelfile::Read(bytes.data(), bytes.size(), m, err) &&
                                                                              m.info.name == "Test avatar" && !m.hasHand[0]);
        const std::vector<uint8_t> hands = modelfile::Write(facts());
        check("a file without a body reads as one without", reads(hands, r, has, err) && !has);
        check("writing is the same each time", file(made) == bytes);
    }
    {
        check("the skeleton has VRM's names", std::string(kRoleNames[kHips]) == "hips" && std::string(kRoleNames[kRightHand]) == "rightHand" &&
                                                  std::string(kRoleNames[kLeftFingers]) == "leftThumbMetacarpal" &&
                                                  std::string(kRoleNames[kRoles - 1]) == "rightLittleDistal");
        bool tree = RoleParent(kHips) == -1;
        for (int r = 1; r < kRoles; ++r) tree = tree && RoleParent(r) >= 0 && RoleParent(r) < r;   // a parent comes before its children
        check("every bone but the hips hangs from an earlier one", tree);
        check("fingers hang from their hand, then from each other", RoleParent(kLeftFingers) == kLeftHand && RoleParent(kLeftFingers + 4) == kLeftFingers + 3 &&
                                                                       RoleParent(kRightFingers + 12) == kRightHand && RoleParent(kRightFingers + 14) == kRightFingers + 13);
        check("a bone's parent is its nearest present one", made.Parent(kHead) == kNeck && made.Parent(kNeck) == kSpine &&
                                                                made.Parent(kLeftUpperArm) == kSpine && made.Parent(kLeftFingers + 3) == kLeftHand &&
                                                                made.Parent(kHips) == -1);
    }
    // each rule broken on its own
    check("a missing leg", refused([](Body &b) { b.present[kLeftLowerLeg] = false; }, "no leftLowerLeg"));
    check("a bone nowhere", refused([](Body &b) { b.at[kChest][1] = 1e9f; }, "bone isn't at a place"));
    check("eyes nowhere", refused([](Body &b) { b.eyes[0] = 1e9f; }, "eyes"));
    check("a position unit of zero", refused([](Body &b) { b.unit = 0; }, "unit"));
    check("a triangle past the vertices", refused([](Body &b) { b.idx[5] = 11; }, "vertex that isn't there"));
    check("a vertex on an absent bone", refused([](Body &b) { b.weights[2] = 5, b.weights[0] -= 5; }, "bone that isn't there"));
    check("a vertex on a bone past the skeleton", refused([](Body &b) { b.bones[3] = kRoles; }, "bone that isn't there"));
    check("weights that don't add up", refused([](Body &b) { b.weights[4] = 100; }, "add up"));
    check("a texture place that isn't a number", refused([](Body &b) { b.uv[3] = 1e9f; }, "texture place"));
    check("arrays of different lengths", refused([](Body &b) { b.nrm.resize(b.nrm.size() - 3); }, ""));
    check("parts that leave a gap", refused([](Body &b) { b.parts[1].first = 5, b.parts[1].count = 4; }, "follow"));
    check("parts that stop early", refused([](Body &b) { b.parts[1].count = 4; }, "cover"));
    check("a part past the triangles", refused([](Body &b) { b.parts[1].count = 0xfffffff0u; }, "follow"));
    check("a part with a material that isn't there", refused([](Body &b) { b.parts[0].material = 2; }, "material that isn't there"));
    check("a material with a texture that isn't there", refused([](Body &b) { b.materials[0].texture = 2; }, "texture that isn't there"));
    check("a material with texture -2", refused([](Body &b) { b.materials[0].texture = -2; }, "texture that isn't there"));
    check("a colour out of range", refused([](Body &b) { b.materials[1].color[3] = 1.5f; }, "colour"));
    check("an alpha mode this doesn't draw", refused([](Body &b) { b.materials[1].alpha = 3; }, "kind"));
    check("a flag this doesn't know", refused([](Body &b) { b.materials[1].flags = 8; }, "kind"));
    check("a texture with too few pixels", refused([](Body &b) { b.textures[0].rgba.resize(20); }, "pixels"));
    check("a texture of no width", refused([](Body &b) { b.textures[0].width = 0, b.textures[0].rgba.clear(); }, "size is out of range"));
    check("a texture too wide", refused([](Body &b) { b.textures[1].width = 2049, b.textures[1].height = 1, b.textures[1].rgba.assign(2049 * 4, 9); }, "size is out of range"));
    check("a shape this doesn't know", refused([](Body &b) { b.shapes[0].id = 9; }, "doesn't know"));
    check("two shapes of one kind", refused([](Body &b) { b.shapes[1].id = Shape::kAa; }, "two shapes"));
    check("a shape on a vertex that isn't there", refused([](Body &b) { b.shapes[0].verts[2] = 11; }, "isn't there"));
    check("a shape whose vertices don't rise", refused([](Body &b) { b.shapes[0].verts[1] = 1; }, "isn't there"));
    check("a shape that moves nothing", refused([](Body &b) { b.shapes[1].verts.clear(), b.shapes[1].delta.clear(); }, "out of range"));
    {
        Body big = body(), r;
        big.textures.resize(kMaxTextures);
        for (Texture &t : big.textures) t.width = 2048, t.height = 2048, t.rgba.assign(size_t(2048) * 2048 * 4, 1);
        std::string why;
        check("textures too large together", !Valid(big, why) && why.find("together") != std::string::npos);
    }
    // the file itself
    {
        std::string why;
        Body r;
        bool has;
        const size_t at = chunk(bytes, 1), texture = chunk(bytes, 2), shape = chunk(bytes, 4);
        std::vector<uint8_t> b = bytes;
        b.insert(b.end(), bytes.begin() + long(at), bytes.begin() + long(texture));
        check("two bodies", !reads(b, r, has, why) && why.find("two bodies") != std::string::npos);
        b = bytes;
        put32(b, at + 8, 1);
        check("a body of another kind", !reads(b, r, has, why) && why.find("kind") != std::string::npos);
        b = bytes;
        put32(b, at + 8 + 20, 54);
        check("a skeleton of another size", !reads(b, r, has, why) && why.find("kind") != std::string::npos);
        b = bytes;
        put32(b, at + 8 + 24, 0x40000000u);   // a billion vertices: refused before anything is sized for them
        check("a count far past the limit", !reads(b, r, has, why) && why.find("out of range") != std::string::npos);
        b = bytes;
        put32(b, at + 8 + 24, 12);
        check("a count past the chunk's end", !reads(b, r, has, why));
        b = bytes;
        put32(b, at + 8 + 24, 10);
        check("a count short of the chunk's end", !reads(b, r, has, why));
        b = bytes;
        put32(b, texture + 8 + 4, 1);
        check("a texture of another format", !reads(b, r, has, why) && why.find("kind") != std::string::npos);
        b = bytes;
        put32(b, shape + 8 + 4, 0x7fffffffu);
        check("a shape with a count far past the limit", !reads(b, r, has, why) && why.find("out of range") != std::string::npos);
        b.assign(bytes.begin(), bytes.begin() + long(at));
        b.insert(b.end(), bytes.begin() + long(texture), bytes.end());
        check("textures and shapes without a body", !reads(b, r, has, why) && why.find("without a body") != std::string::npos);
        b.assign(bytes.begin(), bytes.begin() + long(shape));
        check("a body without its shapes reads", reads(b, r, has, why) && has && r.shapes.empty() && r.textures.size() == 2);
        b.assign(bytes.begin(), bytes.begin() + long(chunk(bytes, 3)));
        check("a body that lost a texture a material names is refused", !reads(b, r, has, why) && why.find("texture that isn't there") != std::string::npos);
    }
    // every truncation: refused, or a whole file with fewer chunks; none reads past its end (the address sanitizer watches)
    {
        int odd = 0;
        for (size_t n = 0; n < bytes.size(); ++n) {
            const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + long(n));
            Body r;
            bool has, whole = false;
            std::string err;
            for (int c = 0; c <= 6; ++c) whole = whole || n == chunk(bytes, c);
            if (reads(cut, r, has, err) && !whole) ++odd;
        }
        check("every cut-off file is refused (but the ones that end after a whole chunk)", odd == 0);
    }
    // every byte changed: read or refused, and whatever reads keeps the rules
    {
        int broke = 0, took = 0;
        for (size_t at = 0; at < bytes.size(); ++at) {
            for (const uint8_t flip : {uint8_t(0x01), uint8_t(0x80), uint8_t(0xff)}) {
                std::vector<uint8_t> b = bytes;
                b[at] ^= flip;
                Body r;
                bool has;
                std::string err, why;
                if (!reads(b, r, has, err)) continue;
                ++took;
                if (has && !Valid(r, why)) ++broke;
            }
        }
        std::printf("      %d changed files read (a position, a colour, a pixel or a letter changed)\n", took);
        check("a changed file that reads still keeps the rules", broke == 0 && took > 0);
    }
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
