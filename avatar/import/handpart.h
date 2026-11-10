// A hand cut out of a model, as a model file's hand (modelfile.h): the triangles its hand
// bones move, with a short cuff of the forearm's (ft-handview fades the hand out at the wrist,
// and the cut needs something to end in), placed with the wrist at the origin.
//
// The model's joints become WebXR's 25 by the rules the bundled hands were made with
// (screens/tools/hand-asset.py): a metacarpal the rig lacks is put a third of the way from the
// wrist to its knuckle, with no weights; a bone WebXR lacks (a palm bone, a ring's) gives its
// weights to the joint it hangs from, the forearm's go to the wrist; a vertex keeps its four
// heaviest joints. A tip the rig lacks is put where the finger's mesh ends.
//
// A hand of another size (a small character's, a file in centimetres) is scaled to a person's,
// since the drawn hand is fitted to the tracked one. One with more triangles than a hand needs
// is reduced (reduce.h).
#pragma once

#include <string>

#include "handbones.h"

namespace importer {

constexpr int kHandTargetTris = 6000;   // the bundled hands have 1800 to 3000

struct HandReport {
    int sourceTris = 0;   // the hand's triangles in the model
    int tris = 0;         // ...and as written
    double scale = 1;     // what it was scaled by
};

// False with `why`: the bones aren't a hand's, or no mesh moves with them.
bool CutHand(const Scene &s, const HandBones &bones, modelfile::Hand &out, HandReport &report, std::string &why);

// The other side's hand: mirrored across x, its triangles turned to face out again.
modelfile::Hand Mirrored(const modelfile::Hand &h);

}  // namespace importer
