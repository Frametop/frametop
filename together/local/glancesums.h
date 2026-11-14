// Where on a shared panel someone looks: the sums (glances.h has the rules).
//
// A pose says where its person's eyes are in the room, how their head is turned, and where they
// look from the way it faces (proto/posewire.h). A panel says where it stands in the room, which
// way it faces, how wide it is and how it is bent (proto/panel.h). From the eyes a line goes the
// way they look; where it meets a panel's picture, from in front, is where they look on it.
//
// A flat panel is the part of its own z = 0 that is `metres` wide, seen from where z is more. A
// bent one is that width of a cylinder of radius `curve` whose middle line is where the flat
// panel's was and whose axis is `curve` in front of it: its picture is on the cylinder's inside,
// so it is met where the line leaves the cylinder.
//
// Sums only: every number a peer sent is bounded where it is read. Header only.
#pragma once

#include "../proto/panel.h"
#include "../proto/posewire.h"

#include <cmath>
#include <vector>

namespace local {

// A place on a panel's picture: across and down it, each 0 to 1, and how far from the eyes.
struct Spot {
    double x = 0, y = 0, far = 0;
};

namespace glance {

struct V {
    double x = 0, y = 0, z = 0;
};

// `v` turned by the unit quaternion `q` (w, x, y, z), or turned back by it.
inline V Turned(const float q[4], const V &v, bool back = false) {
    const double w = q[0], k = back ? -1 : 1, x = k * q[1], y = k * q[2], z = k * q[3];
    const V t = {2 * (y * v.z - z * v.y), 2 * (z * v.x - x * v.z), 2 * (x * v.y - y * v.x)};
    return {v.x + w * t.x + (y * t.z - z * t.y), v.y + w * t.y + (z * t.x - x * t.z), v.z + w * t.z + (x * t.y - y * t.x)};
}

// The way `pose`'s person looks, in the room: a head with no turn faces -z with +x to its
// right, and a gaze is degrees to its left and up from there.
inline V Along(const proto::Pose &pose) {
    const double yaw = pose.gaze[0] * M_PI / 180, pitch = pose.gaze[1] * M_PI / 180;
    return Turned(pose.head, {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)});
}

}  // namespace glance

// Where `pose`'s person looks on `panel`. False when they look past it, away from it or at its
// back.
inline bool Lands(const proto::Panel &panel, const proto::Pose &pose, Spot &out) {
    using glance::V;
    const V from = glance::Turned(panel.turn, {double(pose.eyes[0]) - panel.at[0], double(pose.eyes[1]) - panel.at[1], double(pose.eyes[2]) - panel.at[2]}, true);
    const V way = glance::Turned(panel.turn, glance::Along(pose), true);
    const double tall = double(panel.metres) * panel.height / panel.width;
    double far = 0, across = 0;
    if (panel.curve <= 0) {
        if (!(from.z > 0) || !(way.z < 0)) return false;
        far = -from.z / way.z;
        across = from.x + far * way.x;
    } else {
        // across the floor, the cylinder is a circle about (0, r): the line leaves it at the further of the two places it meets it
        const double r = panel.curve, cz = from.z - r;
        const double a = way.x * way.x + way.z * way.z, b = from.x * way.x + cz * way.z, c = from.x * from.x + cz * cz - r * r, inside = b * b - a * c;
        if (!(a > 1e-9) || !(inside >= 0)) return false;
        far = (-b + std::sqrt(inside)) / a;
        if (!(far > 0)) return false;
        across = std::atan2(from.x + far * way.x, r - (from.z + far * way.z)) * r;
    }
    const double up = from.y + far * way.y;
    if (!(std::fabs(across) <= panel.metres / 2) || !(std::fabs(up) <= tall / 2)) return false;
    out.x = across / panel.metres + 0.5, out.y = 0.5 - up / tall, out.far = far;
    return true;
}

// The panel of `panels` that `pose`'s person looks at, and where on it: the nearest their look
// meets. False when it meets none.
inline bool LooksAt(const std::vector<proto::Panel> &panels, const proto::Pose &pose, uint8_t &id, Spot &spot) {
    bool any = false;
    for (const proto::Panel &panel : panels) {
        Spot on;
        if (!Lands(panel, pose, on) || (any && on.far >= spot.far)) continue;
        any = true, id = panel.id, spot = on;
    }
    return any;
}

}  // namespace local
