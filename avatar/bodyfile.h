// An avatar's body in Frametop's model file (modelfile.h): one skinned, textured mesh on a
// humanoid skeleton, its textures, and the few face shapes Frametop moves (mouth and blinks).
// Like the hands, it is read strictly: a body that breaks any rule is refused whole, since a
// peer may have sent it.
//
// The body stands in its rest pose in metres: y up, its feet on y = 0, its hips over the origin,
// facing -z, its right hand toward +x. A pose turns each bone
// about its rest place (a world-space turn, as the hands' joints are posed), so a bone has a
// place and no rest turn. The skeleton is VRM's humanoid (kRoleNames): a bone the model lacks
// is absent, and a bone's parent is its nearest present one up the list (Parent). Whatever else
// the model had (hair, cloth, ears) moves with the humanoid bone it hung from.
//
// The chunks, after modelfile.h's:
//   BODY  zero (u32), metres per position unit (f32), the eyes' place (3 f32), the number of
//         roles (u32, kRoles), vertices, triangles, parts and materials (u32 each); then the
//         roles' places (kRoles x 3 f32) and whether each is present (kRoles u8); the vertices'
//         positions (i16 x 3), normals (i8 x 3, out of 127), texture places (f32 x 2), bones
//         (u8 x 4, roles) and weights (u8 x 4, adding up to 255); the triangles (u16 x 3); the
//         parts (u32 x 3: first triangle, triangles, material), one after another over all the
//         triangles; the materials (f32 x 4 colour, i32 texture or -1, u32 alpha mode, f32
//         cutoff, u32 flags). Each array padded with zeros to a multiple of four.
//   TEXR  width, height (u16 each), format (u32: 0, RGBA 8 bit per channel, rows top first), the
//         pixels. One chunk per texture, in the order materials number them.
//   SHAP  which shape (u32, Shape::Id), vertices moved (u32), their numbers (u16, rising), how
//         far each moves (i16 x 3, position units). One chunk per shape.
//
// No GPU, SteamVR or network calls here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "modelfile.h"

namespace bodyfile {

constexpr int kRoles = 55;
// VRM 1.0's names for them, in the file's order: the trunk and head, the arms, the legs, then
// the left hand's fingers and the right's.
extern const char *const kRoleNames[kRoles];
enum Role : int {
    kHips = 0, kSpine, kChest, kUpperChest, kNeck, kHead, kLeftEye, kRightEye, kJaw,
    kLeftShoulder, kLeftUpperArm, kLeftLowerArm, kLeftHand,
    kRightShoulder, kRightUpperArm, kRightLowerArm, kRightHand,
    kLeftUpperLeg, kLeftLowerLeg, kLeftFoot, kLeftToes,
    kRightUpperLeg, kRightLowerLeg, kRightFoot, kRightToes,
    kLeftFingers = 25,    // 15 a hand: thumb (metacarpal, proximal, distal), then index, middle,
    kRightFingers = 40,   // ring and little (proximal, intermediate, distal)
};
// The role a role hangs from in a full skeleton, -1 for the hips.
int RoleParent(int role);
// Whether a body without the role is refused: the trunk, head, arms and legs.
bool RoleNeeded(int role);

constexpr int kMaxVerts = 65535;
constexpr int kMaxTris = 60000;
constexpr int kMaxParts = 32;         // and materials
constexpr int kMaxTextures = 24;
constexpr int kMaxTextureSide = 2048;
constexpr size_t kMaxTextureBytes = 32u << 20;   // all of a body's pixels
constexpr int kMaxShapes = 16;

struct Material {
    float color[4] = {1, 1, 1, 1};   // times the texture's
    int texture = -1;                // -1: none
    enum Alpha : uint32_t { kOpaque = 0, kMask = 1, kBlend = 2 };
    uint32_t alpha = kOpaque;
    float cutoff = 0.5f;             // kMask: alpha under it isn't drawn
    // kUnlit: drawn in its own colours whatever the light; kToon: shaded in two tones
    enum : uint32_t { kDoubleSided = 1, kUnlit = 2, kToon = 4, kKnownFlags = 7 };
    uint32_t flags = 0;
};

struct Part {
    uint32_t first = 0, count = 0;   // triangles
    uint32_t material = 0;
};

struct Texture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;       // width x height x 4, rows top first
};

struct Shape {
    enum Id : uint32_t { kAa = 1, kIh, kOu, kEe, kOh, kBlink, kBlinkLeft, kBlinkRight, kLastId = kBlinkRight };
    uint32_t id = 0;
    std::vector<uint16_t> verts;     // rising
    std::vector<int16_t> delta;      // verts x 3, position units
};

struct Body {
    float unit = 1e-4f;              // metres per position unit
    float eyes[3] = {};              // where the wearer looks from, between the eyes
    float at[kRoles][3] = {};        // the bones' rest places
    bool present[kRoles] = {};
    std::vector<int16_t> pos;        // vertices x 3
    std::vector<int8_t> nrm;         // vertices x 3
    std::vector<float> uv;           // vertices x 2
    std::vector<uint8_t> bones;      // vertices x 4
    std::vector<uint8_t> weights;    // vertices x 4
    std::vector<uint16_t> idx;       // triangles x 3
    std::vector<Part> parts;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Shape> shapes;
    int verts() const { return int(pos.size() / 3); }
    int tris() const { return int(idx.size() / 3); }
    // The nearest present role up from `role`, -1 at the top.
    int Parent(int role) const;
};

// Whether a body keeps every rule of the format (what Read checks of a file's): `why` says
// which one it broke.
bool Valid(const Body &b, std::string &why);

// The body in a file's chunks (modelfile::Chunks). `has` false and true returned when the file
// has none: a hand model. False with `err` when it has one this reader doesn't take.
bool Read(const std::vector<modelfile::Chunk> &chunks, Body &out, bool &has, std::string &err);
// A whole file: its hands and facts, and its body when it has one.
bool ReadFile(const std::string &path, modelfile::Model &model, Body &body, bool &hasBody, std::string &err);

// The body's chunks, for modelfile::Write. The body must be valid.
std::vector<modelfile::Extra> Write(const Body &b);

}  // namespace bodyfile
