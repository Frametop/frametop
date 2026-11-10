// Which of a model's nodes are the body's bones: VRM's humanoid (bodyfile.h's roles).
//
// A VRM says. For anything else the nodes' names do, the way the common rigs name them (Unity's
// humanoid, Mixamo, VRoid, VRChat's, Blender's), together with how they hang: the hips and the
// head by name, the spine's bones the ones between them, each hand and foot by name and its
// arm's or leg's bones the ones above it. The fingers are the ones the hands were found with
// (handbones.h). A skeleton that can't be read that way is refused, with the bones that are
// missing: nothing is guessed.
#pragma once

#include <string>

#include "../bodyfile.h"
#include "handbones.h"

namespace importer {

struct BodyBones {
    int node[bodyfile::kRoles];   // each role's node, -1 for one the model lacks
};

// `hands`: the left and right hands' bones where they were found (FindHand), or null: their
// fingers become the body's. False with `why`.
bool FindBody(const Scene &s, const HandBones *const hands[2], BodyBones &out, std::string &why);

}  // namespace importer
