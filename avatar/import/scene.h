// What ft-model-import takes from a model's file, whatever its format: the skeleton's nodes and
// the triangles that move with them, in the rest pose, in one space, in metres, with their
// materials, the pictures those use and the face shapes Frametop moves. The readers
// (gltfscene.cpp for glTF, GLB and VRM; fbxscene.cpp for FBX) fill it; everything after works
// on it alone.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../modelfile.h"
#include "../vec.h"

namespace importer {

using avatar::Vec;

struct Node {
    std::string name;
    int parent = -1;   // another node (in no promised order: a walk up is capped), or -1
    Vec at;            // in the rest pose
    // A bone it turns with though it doesn't hang from it (a VRM's constraint: a sleeve's bone
    // that aims along the arm from beside it), or -1.
    int follows = -1;
};

constexpr int kInfluences = 8;   // a vertex's joints, at most: the heaviest are kept

struct Vertex {
    Vec p, n;                    // in the rest pose, as the skin puts it; n may be zero (the file had none)
    float u = 0, v = 0;          // its place in its material's picture (v down, as glTF has it)
    int material = -1;           // Scene::materials, or -1
    int node[kInfluences];       // the nodes that move it, -1 for none
    float weight[kInfluences];   // adding up to 1
};

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width x height x 4, rows top first; empty: it couldn't be read
};

struct SceneMaterial {
    float color[4] = {1, 1, 1, 1};
    int image = -1;              // Scene::images, or -1
    uint32_t alpha = 0;          // bodyfile.h's Material::Alpha
    float cutoff = 0.5f;
    bool doubleSided = false, unlit = false, toon = false;
};

// A face shape: how far it moves each vertex it moves, in the rest pose's space.
struct SceneShape {
    uint32_t id = 0;             // bodyfile.h's Shape::Id
    std::map<uint32_t, Vec> move;
};

// The limits a reader keeps: a file past any of them is refused, not cut short.
constexpr size_t kMaxSourceBytes = 512u << 20;
constexpr size_t kMaxNodes = 20000;
constexpr size_t kMaxVerts = 4000000;
constexpr size_t kMaxTris = 4000000;
constexpr size_t kMaxMaterials = 256;
constexpr size_t kMaxImages = 64;         // read; more are left unread (their materials plain)
constexpr int kMaxImageSide = 2048;       // an image is halved until it is no larger
constexpr int kSmallImageSide = 512;      // ...or than this, once the images read take kManyImageBytes
constexpr size_t kManyImageBytes = 96u << 20;

struct Scene {
    std::vector<Node> nodes;
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;      // three vertices each
    std::vector<SceneMaterial> materials;
    std::vector<Image> images;
    std::vector<SceneShape> shapes;      // one of an id at most
    int unreadImages = 0;            // pictures that couldn't be read
    // How large a side the next image read may have: a file of many large ones stays within memory.
    int ImageSide() const {
        size_t bytes = 0;
        for (const Image &im : images) bytes += im.rgba.size();
        return bytes < kManyImageBytes ? kMaxImageSide : kSmallImageSide;
    }
    // A VRM's humanoid: its bones' nodes by VRM 1.0's names (leftHand, leftIndexProximal...).
    // Empty for a file that doesn't say.
    std::map<std::string, int> humanoid;
    // What the file says of itself. mayShare: its licence lets it be passed on (a VRM's fields).
    modelfile::Info info;
    bool licenceSaysNo = false;   // a VRM whose licence forbids passing it on: asking can't override it
};

// False with `err` when the file can't be read or isn't a model these take.
bool LoadGltf(const std::string &path, Scene &out, std::string &err);
bool LoadFbx(const std::string &path, Scene &out, std::string &err);

}  // namespace importer
