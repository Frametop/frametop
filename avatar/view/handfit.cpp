// A tracked hand for the pose solver (handfit.h).
#include "handfit.h"

#include "handskin.h"

namespace handfit {

using avatar::Vec;
using bodypose::Turn;
using namespace bodyfile;

namespace {

Vec At(const Body &b, int role) { return {b.at[role][0], b.at[role][1], b.at[role][2]}; }
void Put(const Vec &v, float out[3]) { out[0] = float(v.x), out[1] = float(v.y), out[2] = float(v.z); }
Turn Frame(const float f[9]) { return Turn::FromAxes({f[0], f[1], f[2]}, {f[3], f[4], f[5]}, {f[6], f[7], f[8]}); }

// The body's hand of that side at rest, as 21 joints. A finger the body lacks bones for is made
// up: straight along the hand, from where a knuckle would be.
handcut::Hand Rest(const Body &b, bool right) {
    const int hand = right ? kRightHand : kLeftHand, fingers = right ? kRightFingers : kLeftFingers;
    const Vec wrist = At(b, hand);
    Vec along = (wrist - At(b, hand - 1)).unit();   // the hand carries on from the forearm
    if (along.norm() < 0.5) along = {right ? 1.0 : -1.0, 0, 0};
    Vec palm = Vec{0, -1, 0} - along * along.dot(Vec{0, -1, 0});   // a resting palm faces down
    if (palm.norm() < 0.1) palm = Vec{0, 0, 1} - along * along.z;
    const Vec thumbward = along.cross(palm.unit()) * (right ? 1 : -1);

    handcut::Hand h{};
    h.right = right, h.fit = true, h.stereo = true, h.confidence = 1;
    Put(wrist, h.p[0]);
    static const double kKnuckle[5][2] = {{0.025, 0.030}, {0.090, 0.025}, {0.092, 0.003}, {0.086, -0.018}, {0.076, -0.036}};   // along, thumbward
    static const double kLength[3] = {0.040, 0.026, 0.021};
    for (int f = 0; f < 5; ++f) {
        bool has = true;
        for (int k = 0; k < 3; ++k) has = has && b.present[fingers + f * 3 + k];
        Vec joint[4];
        if (has) {
            for (int k = 0; k < 3; ++k) joint[k] = At(b, fingers + f * 3 + k);
            has = (joint[1] - joint[0]).norm() > 1e-4 && (joint[2] - joint[1]).norm() > 1e-4;
        }
        if (has) {
            joint[3] = joint[2] + (joint[2] - joint[1]) * 0.8;   // a body has no bone for the tip
        } else {
            const Vec heading = f ? along : (along * 0.6 + thumbward * 0.8).unit();
            joint[0] = wrist + along * kKnuckle[f][0] + thumbward * kKnuckle[f][1];
            for (int k = 0; k < 3; ++k) joint[k + 1] = joint[k] + heading * kLength[k];
        }
        for (int k = 0; k < 4; ++k) Put(joint[k], h.p[4 * f + 1 + k]);
    }
    return h;
}

}  // namespace

// handskin's bone 4f + 1 + k is finger f's bone k: its first bone, from the wrist, is the palm's.
bool Of(const handcut::Hand &h, Frames &out) {
    out = Frames();
    float p[9], b[20][9];
    if (!handskin::TrackedFrames(h, p, b)) return false;
    out.right = h.right;
    out.wrist = {h.p[0][0], h.p[0][1], h.p[0][2]};
    out.palm = Frame(p);
    for (int i = 0; i < bodypose::kFingerBones; ++i) out.bone[i] = Frame(b[4 * (i / 3) + 1 + i % 3]);
    if (h.hasElbow && h.elbowConf > 0.3f) out.hasElbow = true, out.elbow = {h.elbow[0], h.elbow[1], h.elbow[2]};
    return true;
}

Fit::Fit(const Body &b) {
    for (int right = 0; right < 2; ++right) {
        Side &s = side_[right];
        s.rest = Rest(b, right);
        Frames rest;
        s.ok = handfit::Of(s.rest, rest);
        if (!s.ok) continue;
        s.palm = rest.palm.inverse();
        for (int i = 0; i < bodypose::kFingerBones; ++i) s.bone[i] = rest.bone[i].inverse();
    }
}

bool Fit::Of(const Frames &f, bodypose::Hand &out) const {
    out = bodypose::Hand();
    const Side &s = side_[f.right];
    if (!s.ok) return false;
    out.tracked = true, out.fingers = f.fingers;
    out.wrist = f.wrist;
    out.turn = f.palm * s.palm;
    for (int i = 0; f.fingers && i < bodypose::kFingerBones; ++i) out.finger[i] = f.bone[i] * s.bone[i];
    out.hasElbow = f.hasElbow, out.elbow = f.elbow;
    return true;
}

bool Fit::Of(const handcut::Hand &h, bodypose::Hand &out) const {
    Frames f;
    if (!handfit::Of(h, f)) return out = bodypose::Hand(), false;
    return Of(f, out);
}

}  // namespace handfit
