// A turn in three dimensions (a unit quaternion), for the pose solver (bodypose.h): a bone of an
// avatar's body has a place and a turn from its rest. Header only.
#pragma once

#include "../vec.h"

namespace bodypose {

using avatar::Vec;

constexpr double kPi = 3.14159265358979323846;

// An angle brought into -pi..pi.
inline double Wrap(double a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return a < 0 ? a + kPi : a - kPi;
}

struct Turn {
    double w = 1, x = 0, y = 0, z = 0;

    // o first, then this.
    Turn operator*(const Turn &o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Vec operator*(const Vec &v) const {
        const Vec q{x, y, z}, t = q.cross(v) * 2;
        return v + t * w + q.cross(t);
    }
    Turn inverse() const { return {w, -x, -y, -z}; }
    double dot(const Turn &o) const { return w * o.w + x * o.x + y * o.y + z * o.z; }
    bool finite() const { return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    // No turn when it has no length to speak of.
    Turn unit() const {
        const double n = std::sqrt(dot(*this));
        return n > 1e-12 ? Turn{w / n, x / n, y / n, z / n} : Turn{};
    }

    // By `angle` radians about `axis`, the way a right hand's fingers curl about its thumb.
    static Turn About(const Vec &axis, double angle) {
        const Vec u = axis.unit();
        const double s = std::sin(angle / 2);
        return {std::cos(angle / 2), u.x * s, u.y * s, u.z * s};
    }
    // The shortest turn that takes the direction `from` to `to`. None when either has no length.
    static Turn Between(const Vec &from, const Vec &to) {
        const Vec a = from.unit(), b = to.unit();
        if (a.norm() < 0.5 || b.norm() < 0.5) return {};
        const double d = a.dot(b);
        if (d < -1 + 1e-9) {   // straight back: half a turn about any axis square to it
            const Vec axis = a.cross(std::fabs(a.x) < 0.9 ? Vec{1, 0, 0} : Vec{0, 1, 0}).unit();
            return {0, axis.x, axis.y, axis.z};
        }
        const Vec c = a.cross(b);
        return Turn{1 + d, c.x, c.y, c.z}.unit();
    }
    // The turn that takes the x, y and z axes to these (square to each other, a right hand's).
    static Turn FromAxes(const Vec &ax, const Vec &ay, const Vec &az) {
        const double trace = ax.x + ay.y + az.z;
        Turn q;
        if (trace > 0) {
            const double s = std::sqrt(trace + 1) * 2;
            q = {s / 4, (ay.z - az.y) / s, (az.x - ax.z) / s, (ax.y - ay.x) / s};
        } else if (ax.x > ay.y && ax.x > az.z) {
            const double s = std::sqrt(1 + ax.x - ay.y - az.z) * 2;
            q = {(ay.z - az.y) / s, s / 4, (ay.x + ax.y) / s, (az.x + ax.z) / s};
        } else if (ay.y > az.z) {
            const double s = std::sqrt(1 + ay.y - ax.x - az.z) * 2;
            q = {(az.x - ax.z) / s, (ay.x + ax.y) / s, s / 4, (az.y + ay.z) / s};
        } else {
            const double s = std::sqrt(1 + az.z - ax.x - ay.y) * 2;
            q = {(ax.y - ay.x) / s, (az.x + ax.z) / s, (az.y + ay.z) / s, s / 4};
        }
        return q.finite() ? q.unit() : Turn{};
    }
    // From a to b by the short way: a at t = 0, b at 1.
    static Turn Mix(const Turn &a, const Turn &b, double t) {
        const double k = a.dot(b) < 0 ? -1 : 1;
        return Turn{a.w + (k * b.w - a.w) * t, a.x + (k * b.x - a.x) * t, a.y + (k * b.y - a.y) * t, a.z + (k * b.z - a.z) * t}.unit();
    }
    // How far this turns about `axis`, radians in -pi..pi: its twist (the rest swings the axis).
    double TwistAbout(const Vec &axis) const {
        const Vec u = axis.unit();
        return Wrap(2 * std::atan2(x * u.x + y * u.y + z * u.z, w));
    }
    // The angle between this and another turn, radians, 0..pi.
    double AngleTo(const Turn &o) const {
        const double d = std::fabs(dot(o));
        return 2 * std::acos(d > 1 ? 1 : d);
    }
};

}  // namespace bodypose
