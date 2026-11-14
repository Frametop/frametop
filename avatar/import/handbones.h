// Which of a model's nodes are a hand's joints: WebXR's 25 (the wrist, the thumb's four, then
// five for each finger, metacarpal to tip), as ft-handview's models have them.
//
// A VRM says (its humanoid). For anything else the nodes' names do, the way the common rigs
// name them (Unity's humanoid, Mixamo, VRoid, Blender's, WebXR's and OpenXR's own): a hand or
// wrist bone of that side, and for each finger the nodes named for it: under the hand, each the
// next one's parent, or in a flat rig (WebXR's hands: every joint a child of one node) beside
// it, in the order of their distance from the wrist. How far along the finger each node is
// isn't taken from its name but from the chain: three nodes are the finger's three bones, five
// its metacarpal to its tip, and four are told apart by where the first one is (a metacarpal
// starts near the wrist). A rig that can't be read that way is refused, with what is missing:
// nothing is guessed.
#pragma once

#include <string>

#include "rig.h"

namespace importer {

constexpr int kHandJoints = modelfile::kHandJoints;

struct HandBones {
    int joint[kHandJoints];   // each joint's node; -1 for a metacarpal or a tip the rig doesn't have
};

enum class Side { Left, Right, Unsaid };

// The hand of that side. For a file of one hand whose names don't say which (WebXR's), pass
// Side::Unsaid: the nodes then need no side in their names. False with `why`.
bool FindHand(const Scene &s, Side side, HandBones &out, std::string &why);

}  // namespace importer
