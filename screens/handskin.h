// Rendered hands from a 3D model, the CPU side (ft-handview, handview.cpp): the skinning matrices
// that pose the model's hand (handasset.h: WebXR Input Profiles' generic-hand, 25 joints) like a
// tracked hand (handcut::Hand: 21 joints in MediaPipe's order), and demo hands to try it on.
//
// Each of the model's joints gets a frame, the same way in its bind pose and on the tracked hand:
// its bone's direction (toward the next joint along the finger; a tip takes its last bone's), and
// a roll from the palm: across it (index knuckle to little finger's) for the fingers' first bones
// and the metacarpals, the palm's normal for the thumb's first bone, then each bone after takes its
// parent's sideways axis, as a finger bends about it. The model's metacarpal joints (MediaPipe has
// none) follow the palm: wrist, index and little knuckles. The wrist's frame is the palm's. Same
// rule on both sides: a left hand is drawn with the model's left hand, whose bind pose is a left
// hand's, so nothing needs mirroring.
//
// The model is scaled to the tracked hand: by the palm's size overall, and each bone along its
// length to the tracked bone's (within 0.6-1.6 times). A joint's matrix takes a model point to the
// room: p_tracked + R_tracked S R_bind^T (v - p_bind). The GPU blends up to four per vertex.
//
// No GPU or SteamVR calls here (tests use it).
#pragma once

#include "handcut.h"
#include "handmesh.h"

namespace handskin {

constexpr int kJoints = 25;   // WebXR's: wrist, thumb (4), then 5 for each finger (metacarpal to tip)

// The model's joint for each of MediaPipe's 21 (the metacarpals, 5 10 15 20, have none).
constexpr int kFromMediaPipe[21] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 11, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 24};

struct Bones {
    float m[kJoints][12];   // row-major 3x4: model (bind pose) -> room
    float wrist[3];         // the tracked wrist, and the direction from it along the hand
    float fwd[3];
    float scale = 1;        // the palm's size against the model's
};

// The skinning matrices for the model of h's side posed as h. False if the hand is degenerate.
bool Pose(const handcut::Hand &h, Bones &out);

// Vertex i of the model of that side, skinned on the CPU (the bounds, tests). Room, metres.
void Skin(bool right, const Bones &b, int i, float out[3]);
// The model's vertex i in its bind pose, metres.
void BindVertex(bool right, int i, float out[3]);
int Vertices(bool right);

// Every `step`th vertex skinned, as a mesh's points (handmesh::Place's input: where to put the
// hand's square). Normals are left 0.
void Points(bool right, const Bones &b, int step, handmesh::Mesh &out);

// The model's bind pose as a tracked hand (the 21 joints at the model's), for tests.
handcut::Hand BindHand(bool right);

// Demo hands (ft-handview --demo and --snapshot): a pose by name, the hand's wrist at `at`, the
// palm facing `palm` and the fingers along `up` (both room directions; up is made square to palm).
// t animates "wave" (a slow open-close and turn). Names: open, fist, pinch, point, spread, wave.
bool DemoPose(const char *name, bool right, const double at[3], const double palm[3], const double up[3], double t,
              handcut::Hand &out);

}  // namespace handskin
