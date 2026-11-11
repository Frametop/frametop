// One of the room's people as the body they are drawn in: posed from what the room told of
// them (wire.h), on the floor of whoever draws them, and put on a sheet for that one's eyes
// (billboard.h). The body is whichever they are given: the abstract one (../abstract) until
// their own is sent.
//
// No GPU, SteamVR or network calls here (the tests use it). Header only.
#pragma once

#include "../pose/bodypose.h"
#include "../view/handfit.h"
#include "billboard.h"
#include "wire.h"

namespace figure {

using avatar::Vec;

// Where someone is drawn for a pair of eyes: their sheet, and each eye's view through it.
struct View {
    billboard::Sheet sheet;
    viewmath::Mat4 clip[2];
    bool sees[2] = {};   // the eye has a view: it isn't at the sheet's plane or behind it
};

class Figure {
public:
    explicit Figure(const bodyfile::Body &body) : fit_(body), solver_(body), rest_(billboard::Balls(body)) {}

    // They are someone new, or weren't seen for a while: the next pose stands alone.
    void Reset() {
        solver_.Reset();
        posed_ = false;
    }
    // Posed as the room told (`pose`, in its directions), on the floor `floor` of whoever draws
    // them, `dt` seconds after the pose before. False, and they are as they were, when it can't
    // be solved.
    bool Pose(const proto::Pose &pose, const roomfloor::Floor &floor, double dt) {
        bodypose::Pose solved;
        if (!solver_.Solve(wearer::Pose(roomwire::FromRoom(pose, floor), fit_, posed_ ? dt : 0), solved)) return false;
        solver_.Matrices(solved, bones_);
        return posed_ = true;
    }
    bool posed() const { return posed_; }
    const float (*bones() const)[12] { return bones_; }   // the skinning matrices, once posed

    // Where they are drawn for eyes at `eyes` (left, right), on a sheet `aspect` times as wide
    // as tall. False when there is nothing to draw: they aren't posed, or neither eye sees them.
    bool Seen(const Vec eyes[2], double aspect, View &out) const {
        if (!posed_ || !billboard::Around(billboard::Posed(rest_, bones_), eyes, aspect, out.sheet)) return false;
        for (int e = 0; e < 2; ++e) out.sees[e] = billboard::EyeView(out.sheet, eyes[e], out.clip[e]);
        return out.sees[0] || out.sees[1];
    }

private:
    handfit::Fit fit_;
    bodypose::Solver solver_;
    std::vector<billboard::Ball> rest_;
    float bones_[bodyfile::kRoles][12] = {};
    bool posed_ = false;
};

}  // namespace figure
