// The part fbxscene.cpp reads an FBX's materials with (fbxlook.cpp).
#pragma once

#include <string>

#include "scene.h"
#include "ufbx.h"

namespace importer {

// The materials, one for each of the file's in its order, and the pictures their colours use:
// one inside the file, or a file beside it of the name the FBX gives (wherever the FBX says it
// was). A picture that can't be read leaves its materials plain. False with `err` for a file
// past the limits.
bool ReadFbxLook(const ufbx_scene *scene, const std::string &folder, Scene &out, std::string &err);

}  // namespace importer
