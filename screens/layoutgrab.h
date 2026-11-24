// The layout carry's rules (Move All Windows; vrlayoutgrab.cpp; user, 2026-11-14: "when you grab it, it
// grabs your entire Frametop layout and moves it"; 2026-11-24: "drag and move your whole set of tiled or
// free floating windows at the same time"). One panel is held, and everything the carry moves, that panel
// too, keeps its place in a frame at the held panel's spot that faces the head and stays level (Frame).
// So the set turns about you as that spot goes round you, rises and sinks with it, and comes nearer or
// goes away with it, and nothing in it tilts or rolls, however the hand turns. The 3D mouse's carry-all
// (vr.cpp: CarryFollow) moves the others with the head instead.
//   M is any type with a float m[3][4] (vr::HmdMatrix34_t in vrlayoutgrab.cpp). Header only, no OpenVR, so
// its test runs anywhere (screens/tests/layoutgrab-test.cpp).
#pragma once

#include <cmath>
#include <map>

#include "posemath.h"

namespace layoutgrab {

// The frame the set rides in: at the carried point `at` (a pose: only its place counts), its front (z)
// level and toward the head, its top (y) straight up. False with the point within about 25 degrees of
// straight above or below the head (as posemath's FaceAbout): which way is "toward the head" swings
// round there with a few centimetres of movement, and the set would spin. There it slides (Grab::Slide).
template <class M>
bool Frame(const M &at, const M &head, M *out) {
    const double d[3] = {head.m[0][3] - at.m[0][3], head.m[1][3] - at.m[1][3], head.m[2][3] - at.m[2][3]};
    const double flat = std::hypot(d[0], d[2]);
    if (flat < 1e-6 || std::fabs(d[1]) > 0.9 * std::sqrt(posemath::Dot3(d, d))) return false;
    const double z[3] = {d[0] / flat, 0, d[2] / flat}, x[3] = {z[2], 0, -z[0]};   // x: up x z
    M m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = i == 1, m.m[i][2] = float(z[i]), m.m[i][3] = at.m[i][3];
    *out = m;
    return true;
}

// One carry: the held panel, the frame as of the last tick, and what it moves, each by its pose in the
// frame. One something else holds meanwhile is taken again where that has it (Take), so it goes on from
// there.
template <class M>
struct Grab {
    int panel = -1;         // the held one (-1: no carry)
    M frame{};
    std::map<int, M> rel;   // what it moves, the held one too -> its pose in the frame
    bool slid = false;      // the last tick only slid it (Slide)

    bool on() const { return panel >= 0; }
    bool Moves(int i) const { return on() && rel.count(i); }
    // It starts, held at the point `at`, facing the head (Frame) or, straight above or below it, any way.
    void Begin(int held, const M &at, const M &head) {
        panel = held, rel.clear();
        slid = !Frame(at, head, &frame);
        if (slid) frame = posemath::Translation<M>(at.m[0][3], at.m[1][3], at.m[2][3]);
    }
    void End() { panel = -1, rel.clear(), slid = false; }
    void Take(int i, const M &pose) { rel[i] = posemath::Mul(posemath::Inverse(frame), pose); }
    M Pose(int i) const { return posemath::Mul(frame, rel.at(i)); }
    // The carried point is at `at` now: the set turned with it about the head, or only slid. Returns how
    // the set moved since the last tick, for what goes with it and has no pose here (a panel's way
    // somewhere, the front slot's home).
    M Move(const M &at, const M &head) {
        M f{};
        if (!Frame(at, head, &f)) return slid = true, Slide(at);
        if (!slid) return Turn(f);
        // out from straight above or below: it turned nothing there, so it goes on from how it stands
        const M step = Slide(at);
        for (auto &[i, r] : rel) r = posemath::Mul(posemath::Inverse(f), posemath::Mul(frame, r));
        frame = f, slid = false;
        return step;
    }

private:
    M Turn(const M &f) {
        const M step = posemath::Mul(f, posemath::Inverse(frame));
        frame = f;
        return step;
    }
    M Slide(const M &at) {
        M step = posemath::Identity<M>();
        for (int k = 0; k < 3; ++k) step.m[k][3] = at.m[k][3] - frame.m[k][3], frame.m[k][3] = at.m[k][3];
        return step;
    }
};

}  // namespace layoutgrab
