// The parts gltfscene.cpp reads a glTF with, each in its own file: VRM's extension
// (gltfvrm.cpp) and the materials and their pictures (gltflook.cpp).
#pragma once

#include <string>
#include <vector>

#include "cgltf.h"
#include "scene.h"

namespace importer {

// One of a mesh's own shapes (a morph target) as a share of one of Frametop's.
struct MorphBind {
    uint32_t shape;   // bodyfile.h's Shape::Id
    size_t mesh, target;
    double weight;
};

// A name as the file means it: a glTF's text is JSON, whose escapes ("\u3042") cgltf leaves in.
std::string GltfText(const char *raw);

// The materials, one for each of the file's in its order, and the pictures their colours use.
// A picture that can't be read leaves its materials plain (Scene::unreadImages). `folder`:
// where the file is, for pictures beside it. False with `err` for a file past the limits.
bool ReadLook(const cgltf_data *d, const std::string &folder, Scene &out, std::string &err);

// VRM's humanoid, licence, toon materials and expressions, from whichever of its two
// extensions the file has. False when the file isn't a VRM. After ReadLook.
bool ReadVrm(const cgltf_data *d, Scene &out, std::vector<MorphBind> &binds);

// For a file that isn't a VRM: its meshes' shapes that are named as Frametop's (shapenames.h).
void NamedMorphs(const cgltf_data *d, std::vector<MorphBind> &binds);

}  // namespace importer
