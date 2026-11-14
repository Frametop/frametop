// The layout grab's rules (vr.cpp: LayoutGrabStart; user, 2026-11-14: "when you grab it, it grabs your
// entire Frametop layout and moves it"). One screen is carried, by a laser on its layout button, and
// the room's other panels go with it in one piece: each keeps its place in a frame at the carried
// screen's centre that faces the head and stays level (Frame). So the layout turns about you as the
// carried screen goes round you, rises and sinks with it, and comes nearer or goes away with it, and
// nothing in it tilts or rolls, however the hand turns the carried screen itself. The 3D mouse's
// carry-all (vr.cpp: CarryFollow) moves the others with the head instead.
//   M is any type with a float m[3][4] (vr::HmdMatrix34_t in vr.cpp). Header only, no OpenVR, so its
// test runs anywhere (screens/tests/layoutgrab-test.cpp).
#pragma once

#include <cmath>
#include <map>

#include "posemath.h"

namespace layoutgrab {

// The frame the layout rides in: at the carried screen's centre, its front (z) level and toward the
// head, its top (y) straight up. False with the screen within about 25 degrees of straight above or
// below the head (as posemath's FaceAbout): which way is "toward the head" swings round there with a
// few centimetres of movement, and the layout would spin.
template <class M>
bool Frame(const M &carried, const M &head, M *out) {
    const double d[3] = {head.m[0][3] - carried.m[0][3], head.m[1][3] - carried.m[1][3], head.m[2][3] - carried.m[2][3]};
    const double flat = std::hypot(d[0], d[2]);
    if (flat < 1e-6 || std::fabs(d[1]) > 0.9 * std::sqrt(posemath::Dot3(d, d))) return false;
    const double z[3] = {d[0] / flat, 0, d[2] / flat}, x[3] = {z[2], 0, -z[0]};   // x: up x z
    M m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = i == 1, m.m[i][2] = float(z[i]), m.m[i][3] = carried.m[i][3];
    *out = m;
    return true;
}

// One grab: the carried screen, the frame as of the last tick, and the others, each by its pose in the
// frame. A panel something else holds meanwhile is taken again where that has it (Take), so it goes
// on from there.
template <class M>
struct Grab {
    int panel = -1;         // the carried screen (-1: no grab)
    M frame{};
    std::map<int, M> rel;   // the others: panel -> its pose in the frame
    bool lost = false;      // no frame last tick (Lose)

    bool on() const { return panel >= 0; }
    // The grab moves it: the carried screen, or one of the others.
    bool Moves(int i) const { return on() && (i == panel || rel.count(i)); }
    void Begin(int carried, const M &f) { panel = carried, frame = f, rel.clear(), lost = false; }
    void End() { panel = -1, rel.clear(), lost = false; }
    void Take(int i, const M &pose) { rel[i] = posemath::Mul(posemath::Inverse(frame), pose); }
    M Pose(int i) const { return posemath::Mul(frame, rel.at(i)); }
    // A tick without a frame (Frame false, or no head pose): the others stay where they are...
    void Lose() { lost = true; }
    // The frame now. Returns how the layout moved since the last tick, for what goes with it and has
    // no pose here (a panel's way somewhere, the front slot's home). ...and after ticks without one
    // the layout goes on from where it stayed: this tick moves nothing.
    M Turn(const M &f) {
        if (lost) {
            for (auto &[i, r] : rel) r = posemath::Mul(posemath::Inverse(f), posemath::Mul(frame, r));
            frame = f, lost = false;
            return posemath::Identity<M>();
        }
        const M step = posemath::Mul(f, posemath::Inverse(frame));
        frame = f;
        return step;
    }
};

}  // namespace layoutgrab
