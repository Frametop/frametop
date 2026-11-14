// Demo wearers (demo.h).
#include "demo.h"

#include "handskin.h"

#include <algorithm>
#include <cstring>

namespace demo {

using avatar::Vec;
using bodypose::Turn;

const char *const kNames = "stand wave reach type sit lean look";

namespace {

// One hand: a demo hand pose, its wrist (for the right hand, in a body 1.6 m to the eyes: the
// left is mirrored), the way its palm faces and its fingers point.
struct HandAt {
    const char *pose;
    Vec wrist, palm, up;
};

void Hold(const HandAt &at, bool right, double size, double t, wearer::Seen &out) {
    const double side = right ? 1 : -1;
    const double wrist[3] = {at.wrist.x * side * size, at.wrist.y * size, at.wrist.z * size};
    const double palm[3] = {at.palm.x * side, at.palm.y, at.palm.z}, up[3] = {at.up.x * side, at.up.y, at.up.z};
    handcut::Hand h{};
    if (handskin::DemoPose(at.pose, right, wrist, palm, up, t, h)) out.has[right] = handfit::Of(h, out.hand[right]);
}

}  // namespace

bool Wearer(const char *name, double t, double tall, double ahead, wearer::Seen &out) {
    out = wearer::Seen();
    const double size = tall / 1.6;
    const double in = std::min(1.0, std::max(0.0, t)), eased = in * in * (3 - 2 * in);
    out.eyeHeight = tall;
    out.eyes = {0, tall, ahead};
    if (!std::strcmp(name, "stand")) return true;
    if (!std::strcmp(name, "wave")) {
        Hold({"wave", {0.34, 1.52, -0.16}, {0, 0, -1}, {0.15, 1, 0}}, true, size, t, out);
    } else if (!std::strcmp(name, "reach")) {
        for (int right = 0; right < 2; ++right) Hold({"open", {0.17, 1.22, -0.46}, {0, -1, 0}, {-0.2, 0.15, -1}}, right, size, t, out);
    } else if (!std::strcmp(name, "type")) {   // at a desk
        out.eyes.y -= 0.40 * size * eased;
        out.head = Turn::About({1, 0, 0}, -0.25 * eased);
        for (int right = 0; right < 2; ++right) Hold({"open", {0.13, 1.6 - 0.40 * eased - 0.52, -0.36}, {0, -1, 0}, {-0.25, 0.1, -1}}, right, size, t, out);
    } else if (!std::strcmp(name, "sit")) {
        out.eyes.y -= 0.42 * size * eased;
    } else if (!std::strcmp(name, "lean")) {   // over something on the floor ahead
        out.head = Turn::About({1, 0, 0}, -1.0 * eased);
        out.eyes = out.eyes + Vec{0, -0.28, -0.36} * (size * eased);
        Hold({"point", {0.2, 0.8, -0.62}, {0, -1, -0.2}, {0, -0.5, -1}}, true, size, t, out);
    } else if (!std::strcmp(name, "look")) {   // over the left shoulder
        out.head = Turn::About({0, 1, 0}, 1.1 * eased) * Turn::About({1, 0, 0}, -0.15 * eased);
    } else {
        return false;
    }
    return true;
}

bool Pose(const char *name, double t, const bodyfile::Body &body, const handfit::Fit &fit, bodypose::Input &out) {
    wearer::Seen seen;
    out = bodypose::Input();
    if (!Wearer(name, t, body.eyes[1] > 0.2f ? body.eyes[1] : 1.6, body.eyes[2], seen)) return false;
    out = wearer::Pose(seen, fit, 0);
    return true;
}

}  // namespace demo
