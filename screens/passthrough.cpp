#include "passthrough.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace passthrough {
bool Valid(const Volume &v) {
    for (double c : v.center)
        if (!std::isfinite(c) || std::fabs(c) > 100) return false;
    for (double s : v.size)
        if (!std::isfinite(s) || s < 0.05 || s > 5) return false;
    return std::isfinite(v.yaw) && std::fabs(v.yaw) <= 180 &&
           std::isfinite(v.pitch) && std::fabs(v.pitch) <= 180 &&
           std::isfinite(v.roll) && std::fabs(v.roll) <= 180;
}

Pose VolumePose(const Volume &v) {
    const double d = std::acos(-1.0) / 180;
    const double cy=std::cos(v.yaw*d), sy=std::sin(v.yaw*d);
    const double cp=std::cos(v.pitch*d), sp=std::sin(v.pitch*d);
    const double cr=std::cos(v.roll*d), sr=std::sin(v.roll*d);
    return {{{cy*cr+sy*sp*sr, -cy*sr+sy*sp*cr, sy*cp, v.center[0]},
             {cp*sr, cp*cr, -sp, v.center[1]},
             {-sy*cr+cy*sp*sr, sy*sr+cy*sp*cr, cy*cp, v.center[2]}}};
}

Point ToLocal(const Volume &v, const Point &p) {
    const auto m = VolumePose(v);
    Point out{};
    for (int i=0;i<3;++i)
        for (int j=0;j<3;++j) out[i] += m[j][i]*(p[j]-v.center[j]);
    return out;
}

Point ToWorld(const Volume &v, const Point &p) {
    const auto m = VolumePose(v);
    Point out = v.center;
    for (int i=0;i<3;++i)
        for (int j=0;j<3;++j) out[i] += m[i][j]*p[j];
    return out;
}

Point GrabCenter(const Volume &v) {
    return ToWorld(v, {0, v.size[1] / 2, v.size[2] / 2 + 0.085});
}

std::array<Edge, 4> RectangleEdges(const Volume &v, double outset) {
    const double x = v.size[0] / 2 + outset;
    const double y = v.size[1] / 2;
    const double z = v.size[2] / 2 + outset;
    const Point corners[] = {ToWorld(v, {-x, y, -z}), ToWorld(v, {x, y, -z}),
                             ToWorld(v, {x, y, z}), ToWorld(v, {-x, y, z})};
    std::array<Edge, 4> edges{};
    for (int i = 0; i < 4; ++i) edges[i] = {corners[i], corners[(i + 1) % 4]};
    return edges;
}

std::array<Point, 6> RectangleControls(const Volume &v) {
    std::array<Point, 6> controls{};
    for (int face : {0, 1, 4, 5}) {
        Point local{0, v.size[1] / 2, 0};
        local[face / 2] = (face % 2 ? 1 : -1) * v.size[face / 2] / 2;
        controls[face] = ToWorld(v, local);
    }
    return controls;
}

bool ResizeFace(Volume &out, const Volume &start, int face, double delta) {
    if (!Valid(start) || (face != 0 && face != 1 && face != 4 && face != 5) || !std::isfinite(delta)) return false;
    const int axis = face / 2;
    const double sign = face % 2 ? 1 : -1;
    // Use dimension change as the bounded variable, including the center constraints.
    double lo = 0.05 - start.size[axis], hi = 5 - start.size[axis];
    Point local{};
    local[axis] = sign / 2;
    const Point world = ToWorld(start, local);
    Point shift{};
    for (int i = 0; i < 3; ++i) {
        shift[i] = world[i] - start.center[i];
        if (std::fabs(shift[i]) < 1e-12) continue;
        double a = (-100 - start.center[i]) / shift[i], b = (100 - start.center[i]) / shift[i];
        if (a > b) std::swap(a, b);
        lo = std::max(lo, a), hi = std::min(hi, b);
    }
    const double change = std::clamp(sign * delta, lo, hi);
    Volume next = start;
    next.size[axis] = std::clamp(start.size[axis] + change, 0.05, 5.0);
    for (int i = 0; i < 3; ++i) next.center[i] += shift[i] * change;
    if (!Valid(next)) return false;
    out = next;
    return true;
}

namespace {
double Dot(const Point &a, const Point &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
bool UnitRay(const Ray &ray, Point &direction) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(ray.origin[i]) || !std::isfinite(ray.direction[i])) return false;
    const double len = std::sqrt(Dot(ray.direction, ray.direction));
    if (!std::isfinite(len) || len < 1e-9) return false;
    for (int i = 0; i < 3; ++i) direction[i] = ray.direction[i] / len;
    return true;
}
}

bool ResizeDrag::Coordinate(const Ray &ray, double &value) const {
    Point dir{};
    if (!UnitRay(ray, dir)) return false;
    double t = distance_;
    if (Dot(normal_, normal_) > 0) {
        const double denom = Dot(normal_, dir);
        if (std::fabs(denom) < 1e-4) return false;
        Point to{};
        for (int i = 0; i < 3; ++i) to[i] = point_[i] - ray.origin[i];
        t = Dot(to, normal_) / denom;
    }
    if (!std::isfinite(t) || t <= 0 || t > 1000) return false;
    Point rel{};
    for (int i = 0; i < 3; ++i) rel[i] = ray.origin[i] + t * dir[i] - point_[i];
    value = Dot(rel, axis_) + Dot(rel, gesture_);
    return std::isfinite(value);
}

bool ResizeDrag::Begin(const Volume &v, int face, unsigned device, const Ray &ray) {
    if (Active() || !Valid(v) || (face != 0 && face != 1 && face != 4 && face != 5)) return false;
    Point dir{};
    if (!UnitRay(ray, dir)) return false;
    start_ = v;
    point_ = RectangleControls(v)[face];
    Point local{}, to{};
    local[face / 2] = 1;
    const Point world = ToWorld(v, local);
    for (int i = 0; i < 3; ++i) axis_[i] = world[i] - v.center[i], to[i] = point_[i] - ray.origin[i];
    const double along = Dot(dir, axis_);
    for (int i = 0; i < 3; ++i) normal_[i] = dir[i] - axis_[i] * along;
    // Plane contains the resize axis and faces the laser as far as possible. End-on
    // axes instead use a fixed-distance ray point, plus an up-drag gesture (sideways
    // when looking vertically). Both physical push/pull and a 3D mouse's tangent
    // motion resize depth without amplifying near-parallel angular/tracking jitter.
    gesture_ = {};
    if (Dot(normal_, normal_) < 0.0225) {
        normal_ = {};
        gesture_[std::fabs(dir[1]) < 0.9 ? 1 : 0] = 1;
        const double projection = Dot(gesture_, dir);
        for (int i = 0; i < 3; ++i) gesture_[i] -= projection * dir[i];
        const double scale = (face % 2 ? 1 : -1) / std::sqrt(Dot(gesture_, gesture_));
        for (double &value : gesture_) value *= scale;
    }
    distance_ = Dot(to, dir);
    if (!Coordinate(ray, grab_)) return false;
    face_ = face, device_ = device;
    return true;
}

bool ResizeDrag::Update(Volume &v, const Ray &ray) {
    double coordinate;
    return Active() && Coordinate(ray, coordinate) && ResizeFace(v, start_, face_, coordinate - grab_);
}

bool ResizeDrag::Finish(const Volume &v, unsigned device) {
    if (!Active() || device != device_) return false;
    const bool changed = v.center != start_.center || v.size != start_.size;
    Reset();
    return changed;
}

void ResizeDrag::Cancel(Volume &v) {
    if (Active()) v = start_;
    Reset();
}

bool TranslateDrag::Begin(const Volume &v, unsigned device) {
    if (Active() || !Valid(v)) return false;
    start_ = v;
    device_ = device;
    active_ = true;
    return true;
}

Plane TopFace(const Volume &v) {
    const auto box = VolumePose(v);
    Plane plane{{}, v.size[0], v.size[2]};
    for (int k = 0; k < 3; ++k) {
        plane.pose[k][0] = box[k][0];
        plane.pose[k][1] = -box[k][2];
        plane.pose[k][2] = box[k][1];
        plane.pose[k][3] = box[k][3] + box[k][1] * v.size[1] / 2;
    }
    return plane;
}

bool ValidPose(const Pose &pose) {
    for (const auto &row : pose) for (double x : row) if (!std::isfinite(x)) return false;
    // Only proper rigid transforms, never scaling or reflection from bad tracking.
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
        double dot=0;
        for (int k=0;k<3;++k) dot+=pose[k][i]*pose[k][j];
        if (std::fabs(dot-(i==j ? 1 : 0))>1e-5) return false;
    }
    const double det=pose[0][0]*(pose[1][1]*pose[2][2]-pose[1][2]*pose[2][1])
                    -pose[0][1]*(pose[1][0]*pose[2][2]-pose[1][2]*pose[2][0])
                    +pose[0][2]*(pose[1][0]*pose[2][1]-pose[1][1]*pose[2][0]);
    return det > 0;
}

bool TranslateDrag::UpdatePose(Volume &v, const Pose &pose) {
    if (!Active() || !ValidPose(pose)) return false;
    Volume next=start_;
    const auto initial=VolumePose(start_);
    bool rotated=false;
    for(int i=0;i<3;++i) {
        next.center[i]=std::clamp(pose[i][3],-100.,100.);
        // Float OpenVR roundoff is not a drag, and must not trigger a capture.
        if(std::fabs(next.center[i]-start_.center[i])<1e-7) next.center[i]=start_.center[i];
        for(int j=0;j<3;++j) rotated |= std::fabs(initial[i][j]-pose[i][j])>1e-7;
    }
    if(rotated) {
        const double cp=std::hypot(pose[1][0],pose[1][1]), deg=180/std::acos(-1.0);
        next.pitch=std::atan2(-pose[1][2],cp)*deg;
        if(cp>1e-7) {
            next.yaw=std::atan2(pose[0][2],pose[2][2])*deg;
            next.roll=std::atan2(pose[1][0],pose[1][1])*deg;
        } else {
            // At the Euler pole yaw and roll are coupled. Choose roll=0 without
            // losing the rigid orientation; this is not a level-plane constraint.
            next.yaw=std::atan2(-pose[2][0],pose[0][0])*deg;
            next.roll=0;
        }
    }
    if (!Valid(next)) return false;
    v=next;
    return true;
}

bool TranslateDrag::Finish(const Volume &v, unsigned device) {
    if (!Active() || device != device_) return false;
    const bool changed = v.center != start_.center || v.yaw != start_.yaw ||
                         v.pitch != start_.pitch || v.roll != start_.roll;
    Reset();
    return changed;
}

void TranslateDrag::Cancel(Volume &v) {
    if (Active()) v = start_;
    Reset();
}

bool Command(const char *cmd, Volume &v, char *reply, int size, ResizeDrag *drag, TranslateDrag *translation) {
    std::istringstream input(cmd);
    std::string name, action, flag, extra;
    input >> name;
    if (name != "passthrough") return false;
    input >> action;
    Volume next;
    bool parsed = false;
    if (action == "set" && input >> flag >> next.center[0] >> next.center[1] >> next.center[2] >>
                              next.size[0] >> next.size[1] >> next.size[2] >> next.yaw) {
        input >> std::ws;
        parsed = input.eof();
        if (!parsed) parsed = bool(input >> next.pitch >> next.roll) && !(input >> extra);
    }
    if (parsed && (flag == "0" || flag == "1") && Valid(next)) {
        next.enabled = flag == "1";
        if (drag) drag->Reset();
        if (translation) translation->Reset();
        v = next;
    } else if ((action != "state" && action != "state3d") || input >> extra) {
        std::snprintf(reply, size, "error passthrough set <0|1> <x y z> <width height depth> <yaw> [pitch roll] | state | state3d");
        return true;
    }
    std::snprintf(reply, size, "ok %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g", v.enabled,
                  v.center[0], v.center[1], v.center[2], v.size[0], v.size[1], v.size[2], v.yaw);
    if (action == "state3d") {
        const int used = int(std::char_traits<char>::length(reply));
        if (used < size) std::snprintf(reply+used, size-used, " %.6g %.6g", v.pitch, v.roll);
    }
    return true;
}
}  // namespace passthrough
