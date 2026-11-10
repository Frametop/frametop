// An avatar's body out of a model, as a model file's (bodyfile.h): every triangle that moves
// with the skeleton, on the humanoid's bones alone, standing where the format says (its feet on
// the floor, its hips over the origin, facing -z).
//
// A bone the humanoid lacks (hair, a skirt, an ear) gives its weights to the humanoid bone it
// hangs from, so what it moved stays rigid on that; a vertex keeps its four heaviest bones. A
// mesh with more triangles than a body needs is reduced (reduce.h), the vertices a face shape
// moves kept. The parts are put in the order they are drawn in: the solid ones, the cut-out
// ones, then the see-through.
#pragma once

#include <string>

#include "bodybones.h"

namespace importer {

constexpr int kBodyTargetTris = 20000;

struct BodyReport {
    int sourceTris = 0;   // the body's triangles in the model
    int tris = 0;         // ...and as written
    int verts = 0;
    size_t textureBytes = 0;
};

// False with `why`: the bones aren't a standing body's, or no mesh moves with them.
bool CutBody(const Scene &s, const BodyBones &bones, bodyfile::Body &out, BodyReport &report, std::string &why);

}  // namespace importer
