// A tracked hand (handcut::Hand: 21 joints, as the hand tracker gives it) as the pose solver
// takes it (bodypose::Hand): the wrist, and the turn of the body's hand bone and of each finger
// bone from its rest.
//
// A turn is told by giving the body's hand at rest and the tracked hand a frame for each bone
// by one rule, handskin's (TrackedFrames: the rule the hand models are posed by), so a body
// whose arms rest out to the sides and one whose arms rest lower both come out right. A body
// with no finger bones, or part of a finger's, gets that finger made up along its hand.
//
// The tracked hand's frames alone (Frames) have no body in them: they are what one headset
// tells another of a hand (avatar/room/wire.h), and each end fits them to the body it draws.
#pragma once

#include "../bodyfile.h"
#include "../pose/bodypose.h"
#include "handcut.h"

namespace handfit {

// A tracked hand with no body in it: where its wrist is, and the frame (a turn from the room's
// axes) of its palm and of each of the 15 finger bones a body has, in bodyfile's order.
struct Frames {
    bool right = false;
    avatar::Vec wrist;
    bodypose::Turn palm, bone[bodypose::kFingerBones];
    bool fingers = true;   // bone[] is known
    bool hasElbow = false;
    avatar::Vec elbow;
};

// False for a hand too crumpled to tell its palm.
bool Of(const handcut::Hand &h, Frames &out);

class Fit {
public:
    explicit Fit(const bodyfile::Body &b);
    // False (and `out` not tracked) for a hand too crumpled to tell its palm.
    bool Of(const handcut::Hand &h, bodypose::Hand &out) const;
    // The same of a hand's frames. False (and `out` not tracked) for a body whose own hand of
    // that side is too crumpled to tell.
    bool Of(const Frames &f, bodypose::Hand &out) const;
    // The body's own hand at rest as a tracked hand (the tests pose a body with it).
    const handcut::Hand &rest(bool right) const { return side_[right].rest; }

private:
    struct Side {
        handcut::Hand rest;
        bool ok = false;
        bodypose::Turn palm, bone[bodypose::kFingerBones];   // the rest frames, undone
    };
    Side side_[2];
};

}  // namespace handfit
