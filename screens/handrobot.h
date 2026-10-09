// Rendered hands, the robot (HANDS_VIEW_MODEL=robot; ft-handview, handview.cpp): a robot hand
// built in code from the tracked skeleton, no asset. Rigid pieces, each fixed to one segment of
// the hand: a light shell per finger bone (a tapered box with rounded edges and ends), dark ball
// joints at the knuckles and the thumb's base, dark hinges at the other joints, the palm (a dark
// core between a palm plate and a back plate) and a closed cuff at the wrist; small accents glow
// cyan (the fingertips' pads, a line across the knuckles, a dot in the palm, a ring round the
// cuff), and thin dark lines mark the plates.
//
// The mesh is built once, a right hand, in each segment's frame (handskin::TrackedFrames: x the
// axis the bone bends about, y toward the palm, z along the bone), for a reference hand
// (handskin::DemoPose's open one). Posed, each segment's frame is the tracked bone's, scaled by
// the palm's size against the reference's; a left hand's frames are the mirror image of a right
// one's, so the same mesh draws it mirrored. A piece's vertices are fixed to the bone's start or
// its end (Vertex::end): a shell's rounded ends keep their shape, and only its straight middle
// takes up the tracked bone's length, so a fingertip ends at the tracked tip. The palm's width
// follows the tracked knuckles' span. The GPU applies one segment's matrix per vertex
// (handview.cpp's kRobotVs); the CPU only works out the 16 matrices.
//
// No GPU or SteamVR calls here (tests use it).
#pragma once

#include "handcut.h"
#include "handmesh.h"

#include <cstdint>
#include <vector>

namespace handrobot {

// The segments: 0 the palm (and the cuff), 1-3 the thumb's metacarpal, proximal and distal bones,
// then each finger's proximal, middle and distal bones. Segment k runs from joint kFrom[k] to
// joint kTo[k] (MediaPipe's order); the palm's from the wrist to the middle knuckle.
constexpr int kSegs = 16;
constexpr int kFrom[kSegs] = {0, 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19};
constexpr int kTo[kSegs] = {9, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15, 16, 18, 19, 20};

enum Material { kShell = 0, kJoint = 1, kAccent = 2, kLine = 3 };

struct Vertex {
    float p[3];   // in its segment's frame, metres on the reference hand (z from the bone's end if `end`)
    float n[3];   // outward
    float seg;    // its segment
    float end;    // 1: fixed to the bone's end, 0: its start
    float mat;    // Material
};

struct Mesh {
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;   // triangles
};

// The robot hand (a right one), built on first use.
const Mesh &Model();

struct Bones {
    // each segment: its frame (scaled) and where it starts, row-major 3x4 (the segment's frame ->
    // the room), then (the bone's length in the reference hand's units, x scale, y scale, 0)
    float seg[kSegs][16];
    float scale = 1;   // the palm's size against the reference hand's
    bool right = true;
};

// The segments posed as h. False if the hand is degenerate.
bool Pose(const handcut::Hand &h, Bones &out);

// The model's vertex v posed (the GPU's sum, on the CPU: bounds, tests). Room, metres.
void Place(const Bones &b, const Vertex &v, float out[3]);

// Every `step`th vertex posed, as a mesh's points (handmesh::Place's input). Normals are left 0.
void Points(const Bones &b, int step, handmesh::Mesh &out);

// The reference hand's length of segment k (metres), what the mesh was built for.
float ReferenceLength(int k);

}  // namespace handrobot
