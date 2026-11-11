// The abstract body (abstract.h).
#include "abstract.h"

#include "../vec.h"

#include <cmath>

namespace abstract {

using avatar::Vec;
using namespace bodyfile;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Told apart at a glance, and by someone who doesn't see red from green.
const float kColour[kColours][3] = {{0.16f, 0.66f, 0.72f}, {0.94f, 0.58f, 0.20f}, {0.60f, 0.47f, 0.90f}, {0.90f, 0.40f, 0.56f}};
const float kVisor[3] = {0.07f, 0.08f, 0.10f};

// The skeleton: bodyfile's rules (feet on y = 0, facing -z, the right hand at +x).
void Bones(bodyfile::Body &b) {
    const auto bone = [&b](int role, const Vec &at) {
        b.present[role] = true;
        b.at[role][0] = float(at.x), b.at[role][1] = float(at.y), b.at[role][2] = float(at.z);
    };
    b.eyes[1] = 1.6f, b.eyes[2] = -0.08f;
    bone(kHips, {0, 0.95, 0}), bone(kSpine, {0, 1.05, 0.01}), bone(kChest, {0, 1.18, 0.02}), bone(kUpperChest, {0, 1.3, 0.02});
    bone(kNeck, {0, 1.45, 0.01}), bone(kHead, {0, 1.53, 0});
    bone(kLeftEye, {-0.032, 1.6, -0.08}), bone(kRightEye, {0.032, 1.6, -0.08}), bone(kJaw, {0, 1.55, -0.03});
    // a hand at rest: flat, its palm down, the thumb ahead. Each finger's knuckle (along the
    // hand from the wrist, and toward the thumb) and its three bones' lengths.
    static const double kKnuckle[5][2] = {{0.025, 0.030}, {0.090, 0.025}, {0.092, 0.003}, {0.086, -0.018}, {0.076, -0.036}};
    static const double kLength[5][3] = {{0.040, 0.032, 0.026}, {0.040, 0.026, 0.021}, {0.044, 0.029, 0.022}, {0.040, 0.027, 0.021}, {0.032, 0.020, 0.019}};
    for (int side = 0; side < 2; ++side) {
        const double x = side ? 1 : -1;
        const int arm = side ? kRightShoulder : kLeftShoulder, leg = side ? kRightUpperLeg : kLeftUpperLeg;
        const Vec wrist{x * 0.70, 1.42, 0}, along{x, 0, 0}, thumbward{0, 0, -1};
        bone(arm, {x * 0.05, 1.42, 0}), bone(arm + 1, {x * 0.17, 1.42, 0}), bone(arm + 2, {x * 0.45, 1.42, 0}), bone(arm + 3, wrist);
        bone(leg, {x * 0.09, 0.90, 0}), bone(leg + 1, {x * 0.09, 0.50, 0}), bone(leg + 2, {x * 0.09, 0.08, 0}), bone(leg + 3, {x * 0.09, 0.02, -0.12});
        for (int f = 0; f < 5; ++f) {
            const Vec heading = f ? along : (along * 0.6 + thumbward * 0.8).unit();
            Vec joint = wrist + along * kKnuckle[f][0] + thumbward * kKnuckle[f][1];
            for (int k = 0; k < 3; ++k) {
                bone((side ? kRightFingers : kLeftFingers) + f * 3 + k, joint);
                joint = joint + heading * kLength[f][k];
            }
        }
    }
}

Vec At(const bodyfile::Body &b, int role) { return {b.at[role][0], b.at[role][1], b.at[role][2]}; }

// A rounded lump hung from one bone: the ellipsoid about `centre` whose half axes are `ax`, `ay`
// and `az` (square to each other). `around` vertices to a ring, `rings` rings pole to pole.
void Lump(bodyfile::Body &b, int role, const Vec &centre, const Vec &ax, const Vec &ay, const Vec &az, int around, int rings) {
    const int first = b.verts();
    const bool flipped = ax.dot(ay.cross(az)) < 0;   // a left hand's axes wind the other way
    for (int i = 0; i <= rings; ++i) {
        const double polar = kPi * i / rings;
        for (int j = 0; j < around; ++j) {
            const double turn = 2 * kPi * j / around;
            const double n[3] = {std::sin(polar) * std::cos(turn), std::cos(polar), std::sin(polar) * std::sin(turn)};
            const Vec at = centre + ax * n[0] + ay * n[1] + az * n[2];
            // an ellipsoid's surface faces along its axes by the ball's normal over each half axis
            const Vec out = (ax * (n[0] / ax.dot(ax)) + ay * (n[1] / ay.dot(ay)) + az * (n[2] / az.dot(az))).unit();
            for (double v : {at.x, at.y, at.z}) b.pos.push_back(int16_t(std::lround(v / b.unit)));
            for (double v : {out.x, out.y, out.z}) b.nrm.push_back(int8_t(std::lround(v * 127)));
            b.uv.insert(b.uv.end(), {0.0f, 0.0f});
            b.bones.insert(b.bones.end(), {uint8_t(role), 0, 0, 0});
            b.weights.insert(b.weights.end(), {255, 0, 0, 0});
        }
    }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < around; ++j) {
            const int a = first + i * around + j, right = first + i * around + (j + 1) % around, down = a + around, across = right + around;
            const int quad[2][3] = {{a, right, down}, {right, across, down}};
            for (const auto &t : quad) {
                if ((i == 0 && t[0] == a && t[1] == right) || (i == rings - 1 && t[1] == across)) continue;   // nothing at a pole: its ring is one point
                b.idx.insert(b.idx.end(), {uint16_t(t[0]), uint16_t(flipped ? t[2] : t[1]), uint16_t(flipped ? t[1] : t[2])});
            }
        }
}

// One hand: a flat palm on the hand's bone, and a lump along each finger bone.
void Hand(bodyfile::Body &b, bool right) {
    const int hand = right ? kRightHand : kLeftHand, fingers = right ? kRightFingers : kLeftFingers;
    const Vec wrist = At(b, hand), along{right ? 1.0 : -1.0, 0, 0}, thumbward{0, 0, -1}, back{0, 1, 0};
    Lump(b, hand, wrist + along * 0.050 + thumbward * -0.004, along * 0.054, back * 0.013, thumbward * 0.042, 10, 6);
    for (int f = 0; f < 5; ++f)
        for (int k = 0; k < 3; ++k) {
            const int role = fingers + f * 3 + k;
            const Vec from = At(b, role);
            // a body has no bone for a fingertip: the last one carries on four fifths as far
            const Vec to = k < 2 ? At(b, role + 1) : from + (from - At(b, role - 1)) * 0.8;
            const Vec d = (to - from).unit();
            const double thick = (f ? 0.0088 : 0.0105) * (1 - 0.08 * k);
            const Vec side = d.cross(back).unit();
            Lump(b, role, (from + to) * 0.5, side * thick, d * ((to - from).norm() / 2 + thick * 0.6), d.cross(side).unit() * (thick * 0.9), 6, 4);
        }
}

}  // namespace

const float *Colour(int place) { return kColour[((place % kColours) + kColours) % kColours]; }

bodyfile::Body Body(int place) {
    bodyfile::Body b;
    Bones(b);
    // the head, then the hands, in the person's colour; the visor over the eyes, dark
    Lump(b, kHead, {0, 1.60, -0.005}, {0.085, 0, 0}, {0, 0.105, 0}, {0, 0, 0.098}, 20, 12);
    Hand(b, false), Hand(b, true);
    const uint32_t coloured = uint32_t(b.tris());
    Lump(b, kHead, {0, 1.615, -0.055}, {0.074, 0, 0}, {0, 0.034, 0}, {0, 0, 0.060}, 16, 8);
    b.parts = {{0, coloured, 0}, {coloured, uint32_t(b.tris()) - coloured, 1}};
    Material skin, visor;
    for (int c = 0; c < 3; ++c) skin.color[c] = Colour(place)[c], visor.color[c] = kVisor[c];
    b.materials = {skin, visor};
    return b;
}

}  // namespace abstract
