// A wearer (wearer::Seen: what a headset knows of them, on their own floor) as the pose a room's
// people send each other (together/proto/posewire.h: the same, in the room's directions), and
// back again onto the floor of whoever draws them.
//
// No body crosses: a hand goes as its wrist and the frames of its palm and finger bones
// (handfit::Frames), and each end fits those to the body it draws for that person. The mouth
// isn't the headset's to know: it is left shut here (the voice moves it).
//
// The sums only. Header only.
#pragma once

#include "../../together/proto/posewire.h"
#include "../view/wearer.h"
#include "floor.h"

namespace roomwire {

using avatar::Vec;
using bodypose::Turn;

namespace detail {

inline void Put(const Vec &v, float out[3]) { out[0] = float(v.x), out[1] = float(v.y), out[2] = float(v.z); }
inline void Put(const Turn &t, float out[4]) { out[0] = float(t.w), out[1] = float(t.x), out[2] = float(t.y), out[3] = float(t.z); }
inline Vec At(const float v[3]) { return {v[0], v[1], v[2]}; }
inline Turn Of(const float q[4]) { return Turn{q[0], q[1], q[2], q[3]}.unit(); }

}  // namespace detail

// The wearer as the room is told. `floor`: theirs in the room. The count (`turn`) is left 0:
// ft-together counts what it sends. Where they look isn't said here either: ft-together says
// it, and only while they share it.
inline proto::Pose ToRoom(const wearer::Seen &s, const roomfloor::Floor &floor) {
    const Turn turned = roomfloor::Turned(floor);
    proto::Pose p;
    detail::Put(roomfloor::ToRoom(floor, s.eyes), p.eyes);
    detail::Put(turned * s.head, p.head);
    p.eyeHeight = float(s.eyeHeight);
    for (int side = 0; side < 2; ++side) {
        if (!s.has[side]) continue;
        const handfit::Frames &f = s.hand[side];
        proto::HandPose &h = p.hand[side];
        h.tracked = true, h.fingers = f.fingers, h.hasElbow = f.hasElbow;
        detail::Put(roomfloor::ToRoom(floor, f.wrist), h.wrist);
        detail::Put(turned * f.palm, h.turn);
        for (int k = 0; f.fingers && k < proto::kFingerTurns; ++k) detail::Put(turned * f.bone[k], h.finger[k]);
        if (f.hasElbow) detail::Put(roomfloor::ToRoom(floor, f.elbow), h.elbow);
    }
    return p;
}

// Someone the room told of, on the floor of whoever draws them (`floor`: the drawer's).
inline wearer::Seen FromRoom(const proto::Pose &p, const roomfloor::Floor &floor) {
    const Turn turned = roomfloor::Turned(floor).inverse();
    wearer::Seen s;
    s.eyes = roomfloor::FromRoom(floor, detail::At(p.eyes));
    s.head = turned * detail::Of(p.head);
    s.eyeHeight = p.eyeHeight;
    s.looks = p.looks, s.gaze[0] = p.gaze[0], s.gaze[1] = p.gaze[1];   // from the way the head faces: the same on any floor
    for (int side = 0; side < 2; ++side) {
        const proto::HandPose &h = p.hand[side];
        if (!h.tracked) continue;
        handfit::Frames &f = s.hand[side];
        s.has[side] = true;
        f.right = side == 1, f.fingers = h.fingers, f.hasElbow = h.hasElbow;
        f.wrist = roomfloor::FromRoom(floor, detail::At(h.wrist));
        f.palm = turned * detail::Of(h.turn);
        for (int k = 0; h.fingers && k < proto::kFingerTurns; ++k) f.bone[k] = turned * detail::Of(h.finger[k]);
        if (h.hasElbow) f.elbow = roomfloor::FromRoom(floor, detail::At(h.elbow));
    }
    return s;
}

}  // namespace roomwire
