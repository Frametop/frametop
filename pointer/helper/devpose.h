// The pointer device's poses as ft-pointer sends them to its driver: a drag's tilt about the
// grab point, the "posq" line (a pose given in the standing universe, sent in raw space), and
// when a plain "pose" line is due (the driver keeps the last one: only a change goes out).
// Header-only, no OpenVR (the matrices are anything with OpenVR's m[3][4]), so
// pointer/tests/devpose-test.cpp runs anywhere.
#pragma once

#include "../common/vec3.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace md {

struct DevicePose {
    Vec3 origin;
    Basis basis;
};

// A pose turned about pivot: pitch (degrees) about the basis's side axis (x), then yaw about up.
inline DevicePose TiltAbout(Vec3 pivot, Vec3 origin, const Basis &base, double yawDeg, double pitchDeg) {
    const double yr = yawDeg * M_PI / 180, pr = pitchDeg * M_PI / 180;
    const Vec3 up{0, 1, 0}, side = base.x;
    auto turn = [&](Vec3 v) { return RotateAbout(RotateAbout(v, side, pr), up, yr); };
    return {pivot + turn(origin - pivot), {turn(base.x), turn(base.y), turn(base.z)}};
}

// "posq x y z qw qx qy qz": a device pose given in the standing universe, in raw space. S and R
// are the headset's standing and raw poses, read together.
template <typename M>
inline std::string PosqLine(const M &S, const M &R, Vec3 originStanding, const Basis &b) {
    auto toRaw = [&](Vec3 v) { return RotateBy(R, RotateInverseBy(S, v)); };  // directions: raw <- standing
    const Vec3 o = PositionOf(R) + toRaw(originStanding - PositionOf(S));
    double q[4];
    BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
    char msg[200];
    std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", o.x, o.y, o.z, q[0], q[1], q[2], q[3]);
    return msg;
}

// The last plain "pose" sent (raw origin, aim yaw and pitch in degrees).
struct PoseSent {
    bool valid = false;
    std::chrono::steady_clock::time_point at{};
    Vec3 origin;
    double yaw = 0, pitch = 0;
};

// A plain pose goes out when none has, every 100 ms, or when it moved 0.2 mm or turned 0.04 degrees.
inline bool PoseDue(const PoseSent &last, std::chrono::steady_clock::time_point now, Vec3 origin, double yaw,
                    double pitch) {
    return !last.valid || now - last.at >= std::chrono::milliseconds(100) || Length(origin - last.origin) >= 0.0002 ||
           std::fabs(std::remainder(yaw - last.yaw, 360.0)) >= 0.04 || std::fabs(pitch - last.pitch) >= 0.04;
}

inline std::string PoseLine(Vec3 originRaw, double yaw, double pitch) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "pose %.5f %.5f %.5f %.4f %.4f", originRaw.x, originRaw.y, originRaw.z, yaw, pitch);
    return msg;
}

}  // namespace md
