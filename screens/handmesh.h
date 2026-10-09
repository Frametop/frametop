// Rendered hands, the CPU side (ft-handview, handview.cpp): a hand's surface from the matte's
// pieces (handcut.h BuildMatte, with no margin: tapered capsules for the finger bones, convex
// pieces for the palm, the thumb's web and the forearm's stub), and where it's drawn.
//
// Each hand is drawn on a square overlay of its own, placed in the room at the hand and turned
// to face the head, side by side (one half per eye, VROverlayFlags_SideBySide_Parallel, as the
// cutouts): each half holds what that eye sees of the hand, projected onto the square from the
// eye. The compositor reprojects a placed overlay for head motion, so the hand stays put in the
// room between updates, and the halves give it depth. Only the hand's motion waits for an update.
//
// No GPU or SteamVR calls here (tests use it).
#pragma once

#include "handcut.h"

#include <cstdint>
#include <vector>

namespace handmesh {

struct Vertex {
    float p[3];      // in the room, metres
    float n[3];      // outward
    float back;      // metres behind the wrist, along the hand (the forearm fades by it)
};

struct Mesh {
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;   // triangles
    void clear() { v.clear(), idx.clear(); }
};

// The hand's surface, true size (the matte with no margin).
void Build(const handcut::Hand &h, const double headPos[3], Mesh &out);
// A convex hull's triangles, flat shaded (BuildMatte's convex pieces). Adds to out.
void AddHull(const std::vector<std::array<float, 3>> &pts, Mesh &out);
// A tapered capsule (round ends of radius ra at a, rb at b). Adds to out.
void AddCapsule(const handcut::Capsule &c, Mesh &out);

// The square a hand is drawn on: pose (+x right, +y up, +z toward the head, the standing
// universe) and size in metres; `distance` from its centre to the middle of the eyes.
struct Quad {
    handcut::Mat pose;
    double size = 0, distance = 0;
};

// The square through the hand's centre, facing the middle of the eyes, just big enough for
// what either eye sees of the mesh on it (plus `margin` of the size each side). False when the
// hand is too close to the eyes to draw (within 8 cm) or nothing's left.
bool Place(const Mesh &m, const double eyes[2][3], double margin, Quad &out);

// The eye's projection for drawing onto the square: room -> clip space, the square filling
// the viewport, its top at the top of the buffer (the first row). GL order (column-major).
// Depth runs from `nearM` to `farM` metres along the eye's view of the square.
void EyeMatrix(const Quad &q, const double eye[3], double nearM, double farM, float out[16]);

}  // namespace handmesh
