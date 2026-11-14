// A person's pose as a datagram on a Frametop Together link (docs/together.md, "The protocol"):
// what a headset knows of its wearer, in the room's own directions. The head, each tracked hand
// (the wrist, the hand's turn, its fingers', the elbow when the tracker has one), how tall they
// stand, and their mouth's five shapes. The receiver poses a body from it with the solver the
// sender poses its own with (avatar/pose/bodypose.h), so no bones cross the link.
//
// Where they look is in it only while they share that (docs/together.md, "Gaze"): one
// direction for both eyes, from the way the head faces, in half degrees.
//
// A hand's turns belong to no body: `turn` is the frame of the palm and finger[k] the frame of
// finger bone k, each in the room's directions (which way the bone points, which way its back
// faces). Each end fits them to the body it draws for that person (avatar/view/handfit.h).
//
// About thirty a second, at most 217 bytes, and a late one is worth nothing: `turn` counts
// them, and the receiver keeps the newest (Newer). A turn is a unit quaternion (w, x, y, z).
//
// Header only; the bytes only.
#pragma once

#include "bytes.h"
#include "messages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace proto {

constexpr uint8_t kPoseDatagram = 1;   // a datagram's first byte: what it is
constexpr int kFingerTurns = 15;       // a hand's finger bones, in the model file's order
constexpr int kMouthShapes = 5;        // aa, ih, ou, ee, oh

struct HandPose {
    bool tracked = false, fingers = false, hasElbow = false;
    float wrist[3] = {}, turn[4] = {1, 0, 0, 0};
    float finger[kFingerTurns][4] = {};
    float elbow[3] = {};
};

struct Pose {
    uint16_t turn = 0;
    float eyes[3] = {}, head[4] = {1, 0, 0, 0};
    float eyeHeight = 1.6f;   // their eyes above the floor when they stand, metres
    HandPose hand[2];         // left, right
    uint8_t mouth[kMouthShapes] = {};   // each shape's weight, 255 whole
    bool looks = false;       // gaze[] is told: they share where they look
    float gaze[2] = {};       // to their left, and up, in degrees from the way the head faces
};

constexpr uint8_t kPoseLooks = 0x08;   // in a pose's flags, beside the hands' two nibbles
constexpr float kMostGaze = 63.5f;     // degrees either way: what a byte of half degrees holds

// Whether a pose counted `turn` came after one counted `than`: the count wraps.
inline bool Newer(uint16_t turn, uint16_t than) { return int16_t(uint16_t(turn - than)) > 0; }

namespace posewire {

inline void PutTurn16(wire::Out &out, const float q[4]) {
    for (int i = 0; i < 4; ++i) out.Unit16(q[i]);
}

// A turn read and made a unit one. One that is nowhere near a unit fails the read.
inline void TakeTurn(wire::In &in, float q[4], bool sixteen) {
    double norm = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = sixteen ? in.Unit16() : float(int8_t(in.U8())) / 127.0f;
        norm += double(q[i]) * q[i];
    }
    if (!in.ok || norm < 0.5 || norm > 1.5) {
        in.ok = false;
        return;
    }
    const float by = float(1 / std::sqrt(norm));
    for (int i = 0; i < 4; ++i) q[i] *= by;
}

}  // namespace posewire

inline std::vector<uint8_t> Pack(const Pose &m) {
    wire::Out out;
    out.U8(kPoseDatagram);
    out.U16(m.turn);
    for (float v : m.eyes) out.F32(v);
    posewire::PutTurn16(out, m.head);
    out.U16(uint16_t(std::lround(std::fmin(3.0f, std::fmax(0.0f, std::isfinite(m.eyeHeight) ? m.eyeHeight : 1.6f)) * 1000)));
    uint8_t flags = 0;
    for (int side = 0; side < 2; ++side) {
        const HandPose &h = m.hand[side];
        if (h.tracked) flags |= uint8_t((1 | (h.fingers ? 2 : 0) | (h.hasElbow ? 4 : 0)) << (4 * side));
    }
    out.U8(m.looks ? flags | kPoseLooks : flags);
    out.Bytes(m.mouth, kMouthShapes);
    for (int i = 0; m.looks && i < 2; ++i)
        out.U8(uint8_t(int8_t(std::lround(std::fmin(kMostGaze, std::fmax(-kMostGaze, std::isfinite(m.gaze[i]) ? m.gaze[i] : 0.0f)) * 2))));
    for (const HandPose &h : m.hand) {
        if (!h.tracked) continue;
        for (float v : h.wrist) out.F32(v);
        posewire::PutTurn16(out, h.turn);
        for (int f = 0; h.fingers && f < kFingerTurns; ++f)
            for (int i = 0; i < 4; ++i) out.U8(uint8_t(int8_t(std::lround(std::fmin(1.0f, std::fmax(-1.0f, std::isfinite(h.finger[f][i]) ? h.finger[f][i] : 0.0f)) * 127))));
        for (int i = 0; h.hasElbow && i < 3; ++i) out.F32(h.elbow[i]);
    }
    return out.b;
}

// A pose datagram read. False when it isn't one, or says something no person could: a place
// further than the room goes, a turn that isn't one, a height nobody has.
inline bool Read(const uint8_t *data, size_t n, Pose &m) {
    wire::In in(data, n);
    m = Pose{};
    if (in.U8() != kPoseDatagram) return false;
    m.turn = in.U16();
    for (float &v : m.eyes) v = in.F32(-kFar, kFar);
    posewire::TakeTurn(in, m.head, true);
    m.eyeHeight = float(in.U16()) / 1000;
    const uint8_t flags = in.U8();
    in.Take(m.mouth, kMouthShapes);
    if (!in.ok || m.eyeHeight < 0.5f || m.eyeHeight > 2.5f || (flags & 0x80)) return false;
    m.looks = flags & kPoseLooks;
    for (int i = 0; m.looks && i < 2; ++i) m.gaze[i] = float(std::max(-127, int(int8_t(in.U8())))) / 2;
    for (int side = 0; side < 2; ++side) {
        HandPose &h = m.hand[side];
        const int bits = flags >> (4 * side) & 7;
        if (!(bits & 1)) {
            if (bits) return false;   // fingers or an elbow of a hand that isn't there
            continue;
        }
        h.tracked = true, h.fingers = bits & 2, h.hasElbow = bits & 4;
        for (float &v : h.wrist) v = in.F32(-kFar, kFar);
        posewire::TakeTurn(in, h.turn, true);
        for (int f = 0; h.fingers && f < kFingerTurns; ++f) posewire::TakeTurn(in, h.finger[f], false);
        for (int i = 0; h.hasElbow && i < 3; ++i) h.elbow[i] = in.F32(-kFar, kFar);
    }
    return in.done();
}

}  // namespace proto
