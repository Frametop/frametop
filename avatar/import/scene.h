// What ft-model-import takes from a model's file, whatever its format: the skeleton's nodes and
// the skinned triangles, in the rest pose, in one space, in metres. The readers (gltfscene.cpp
// for glTF, GLB and VRM; fbxscene.cpp for FBX) fill it; everything after works on it alone.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../modelfile.h"

namespace importer {

struct Vec {
    double x = 0, y = 0, z = 0;
    Vec operator+(const Vec &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec operator-(const Vec &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const Vec &o) const { return x * o.x + y * o.y + z * o.z; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vec unit() const {
        const double n = norm();
        return n > 1e-12 ? *this * (1 / n) : Vec{};
    }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Node {
    std::string name;
    int parent = -1;   // another node (in no promised order: a walk up is capped), or -1
    Vec at;            // in the rest pose
};

constexpr int kInfluences = 8;   // a vertex's joints, at most: the heaviest are kept

struct Vertex {
    Vec p, n;                    // in the rest pose, as the skin puts it; n may be zero (the file had none)
    int node[kInfluences];       // the nodes that move it, -1 for none
    float weight[kInfluences];   // adding up to 1
};

// The limits a reader keeps: a file past any of them is refused, not cut short.
constexpr size_t kMaxSourceBytes = 512u << 20;
constexpr size_t kMaxNodes = 20000;
constexpr size_t kMaxVerts = 4000000;
constexpr size_t kMaxTris = 4000000;

struct Scene {
    std::vector<Node> nodes;
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;      // three vertices each
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
