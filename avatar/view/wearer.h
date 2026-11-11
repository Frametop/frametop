// A body's wearer as the headset tracks them, for the pose solver (bodypose.h): the head from
// its pose and the eyes' places, each hand the hand tracker shows (handfit.h), and how tall they
// stand, told from where their head has been (bodypose::Stature).
//
// The room is SteamVR's standing universe: its floor is at height 0. No GPU or SteamVR calls
// here (the tests use it).
#pragma once

#include "../pose/bodypose.h"
#include "handcut.h"
#include "handfit.h"

#include <vector>

namespace wearer {

// What a headset knows of its wearer, with no body in it: what one headset tells another
// (avatar/room/wire.h), and what a body of any shape is posed from.
struct Seen {
    avatar::Vec eyes;          // between their eyes
    bodypose::Turn head;       // none when the head is upright and faces -z
    double eyeHeight = 1.6;    // their eyes above the floor when they stand
    bool has[2] = {};          // left, right: the hand is tracked
    handfit::Frames hand[2];
};

// The wearer for the pose solver, their hands fitted to a body's (`fit`: that body's). `dt`:
// seconds since the pose before (0: this one stands alone).
inline bodypose::Input Pose(const Seen &s, const handfit::Fit &fit, double dt) {
    bodypose::Input in;
    in.eyes = s.eyes, in.head = s.head, in.eyeHeight = s.eyeHeight, in.dt = dt;
    for (int side = 0; side < 2; ++side)
        if (s.has[side]) fit.Of(s.hand[side], in.hand[side]);
    return in;
}

// The wearer, watched: each look at the headset's tracking, and how tall they have stood.
class Watch {
public:
    // `head`: the headset's pose; `eyes`: each eye's place; `hands`: the tracker's hands now
    // (a side with none, or one too crumpled to tell, hangs); `dt`: seconds since the call
    // before (0 for the first).
    Seen See(const handcut::Mat &head, const double eyes[2][3], const std::vector<handcut::Hand> &hands, double dt) {
        Seen s;
        s.eyes = {(eyes[0][0] + eyes[1][0]) / 2, (eyes[0][1] + eyes[1][1]) / 2, (eyes[0][2] + eyes[1][2]) / 2};
        const auto axis = [&head](int c) { return avatar::Vec{head.m[0][c], head.m[1][c], head.m[2][c]}; };
        s.head = bodypose::Turn::FromAxes(axis(0), axis(1), axis(2));
        for (const handcut::Hand &h : hands) s.has[h.right] = handfit::Of(h, s.hand[h.right]);
        stature_.See(s.eyes.y, dt);
        s.eyeHeight = stature_.eyeHeight();
        return s;
    }
    // The way the head looks.
    static avatar::Vec Ahead(const handcut::Mat &head) { return {-head.m[0][2], -head.m[1][2], -head.m[2][2]}; }

private:
    bodypose::Stature stature_;
};

// The wearer of one body, for the pose solver.
class Tracked {
public:
    explicit Tracked(const bodyfile::Body &body) : fit_(body) {}

    bodypose::Input See(const handcut::Mat &head, const double eyes[2][3], const std::vector<handcut::Hand> &hands, double dt) {
        return Pose(watch_.See(head, eyes, hands, dt), fit_, dt);
    }
    static avatar::Vec Ahead(const handcut::Mat &head) { return Watch::Ahead(head); }

private:
    handfit::Fit fit_;
    Watch watch_;
};

}  // namespace wearer
