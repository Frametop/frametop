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

class Tracked {
public:
    explicit Tracked(const bodyfile::Body &body) : fit_(body) {}

    // `head`: the headset's pose; `eyes`: each eye's place; `hands`: the tracker's hands now
    // (a side with none, or one too crumpled to tell, hangs); `dt`: seconds since the call
    // before (0 for the first).
    bodypose::Input See(const handcut::Mat &head, const double eyes[2][3], const std::vector<handcut::Hand> &hands, double dt) {
        bodypose::Input in;
        in.eyes = {(eyes[0][0] + eyes[1][0]) / 2, (eyes[0][1] + eyes[1][1]) / 2, (eyes[0][2] + eyes[1][2]) / 2};
        const auto axis = [&head](int c) { return avatar::Vec{head.m[0][c], head.m[1][c], head.m[2][c]}; };
        in.head = bodypose::Turn::FromAxes(axis(0), axis(1), axis(2));
        for (const handcut::Hand &h : hands) fit_.Of(h, in.hand[h.right]);
        stature_.See(in.eyes.y, dt);
        in.eyeHeight = stature_.eyeHeight();
        in.dt = dt;
        return in;
    }
    // The way the head looks.
    static avatar::Vec Ahead(const handcut::Mat &head) { return {-head.m[0][2], -head.m[1][2], -head.m[2][2]}; }

private:
    handfit::Fit fit_;
    bodypose::Stature stature_;
};

}  // namespace wearer
