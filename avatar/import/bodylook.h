// A body's materials and textures out of a Scene's (bodypart.cpp cuts the mesh): the materials
// that are the same become one, only the pictures still used are kept, and they are halved,
// largest first, until together they fit what a body may carry.
#pragma once

#include <string>
#include <vector>

#include "../bodyfile.h"
#include "scene.h"

namespace importer {

constexpr size_t kTextureBudget = 16u << 20;   // a body's pixels, all its textures together

// Fills out.materials and out.textures. `materialOf`: for each of the scene's materials, the
// body's (the last entry: a vertex without one). False with `why`.
bool CutLook(const Scene &s, bodyfile::Body &out, std::vector<uint32_t> &materialOf, std::string &why);

}  // namespace importer
