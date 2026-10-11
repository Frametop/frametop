// Near hand input on Frametop's panels (see handinput.h).
#include "handinput.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace handinput {
namespace {

using handcut::Hand;

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void ToLocal(const Mat &m, const double p[3], double out[3]) {
    const double d[3] = {p[0] - m.m[0][3], p[1] - m.m[1][3], p[2] - m.m[2][3]};
    for (int i = 0; i < 3; ++i) out[i] = m.m[0][i] * d[0] + m.m[1][i] * d[1] + m.m[2][i] * d[2];
}

void Copy3(const float in[3], double out[3]) { out[0] = in[0], out[1] = in[1], out[2] = in[2]; }

double Dist3(const float a[3], const float b[3]) {
    const double d[3] = {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]};
    return std::sqrt(Dot(d, d));
}

// A finger is straight: its tip much farther from the wrist than its knuckle (`mcp`: 5 index,
// 9 middle, 13 ring, 17 little).
double FingerExtension(const Hand &h, int mcp) {
    return Dist3(h.p[mcp + 3], h.p[0]) / std::max(1e-6, Dist3(h.p[mcp], h.p[0]));
}
double Extension(const Hand &h) { return FingerExtension(h, 5); }
// How open a hand is: its four fingers' mean extension (as FingerExtension; a fist about 1, a pointing
// hand about 1.3, an open one 1.7-1.9).
double Openness(const Hand &h) {
    double sum = 0;
    for (int mcp : {5, 9, 13, 17}) sum += FingerExtension(h, mcp);
    return sum / 4;
}
// ...the middle, ring and little fingers' (a pointing hand's are curled, an open one's straight)
double Curl3(const Hand &h) { return (FingerExtension(h, 9) + FingerExtension(h, 13) + FingerExtension(h, 17)) / 3; }
// The palm's normal, out of the palm (palmmenu's PalmNormal: the left hand's turned round).
void PalmOut(const Hand &h, double out[3]) {
    const double a[3] = {double(h.p[5][0]) - h.p[0][0], double(h.p[5][1]) - h.p[0][1], double(h.p[5][2]) - h.p[0][2]};
    const double b[3] = {double(h.p[17][0]) - h.p[0][0], double(h.p[17][1]) - h.p[0][1], double(h.p[17][2]) - h.p[0][2]};
    const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (h.right ? 1 : -1);
    for (int i = 0; i < 3; ++i) out[i] = l != 0 ? n[i] / l : 0;
}

// The angle (degrees) between the index finger (its knuckle to its tip: the last bone alone is 2 cm
// long, and a few mm of tracking noise on its ends turned it 15-20 degrees) and the way into the
// panel there (its normal, reversed). 180 for a finger pointing out of it.
double FingerAngle(const Panel &p, const Hit &at, const Hand &h) {
    const double d[3] = {double(h.p[8][0]) - h.p[5][0], double(h.p[8][1]) - h.p[5][1], double(h.p[8][2]) - h.p[5][2]};
    double l[3];   // in the panel's frame
    for (int i = 0; i < 3; ++i) l[i] = p.pose.m[0][i] * d[0] + p.pose.m[1][i] * d[1] + p.pose.m[2][i] * d[2];
    // the surface's normal (out of its front): +z, or on a cylinder the way OnSurface turns it
    double n[3] = {0, 0, 1};
    if (p.curve > 0) n[0] = -std::sin(at.u / p.curve), n[2] = std::cos(at.u / p.curve);
    const double len = std::sqrt(Dot(l, l));
    if (len < 1e-6) return 180;
    return std::acos(std::clamp(-Dot(l, n) / len, -1.0, 1.0)) * 180 / M_PI;
}

// The panel's surface normal at u (out of its front), in the room.
void SurfaceNormal(const Panel &p, double u, double out[3]) {
    double n[3] = {0, 0, 1};
    if (p.curve > 0) n[0] = -std::sin(u / p.curve), n[2] = std::cos(u / p.curve);
    for (int i = 0; i < 3; ++i) out[i] = p.pose.m[i][0] * n[0] + p.pose.m[i][1] * n[1] + p.pose.m[i][2] * n[2];
}

// How far u, v is from the panel's border: outside it (> 0) round the corners, or inside it (< 0).
double FromBorder(const Panel &p, double u, double v) {
    const double ou = std::fabs(u) - p.width / 2, ov = std::fabs(v) - p.height / 2;
    return ou > 0 || ov > 0 ? std::hypot(std::max(ou, 0.0), std::max(ov, 0.0)) : std::max(ou, ov);
}

// A hand resting low by the body (typing, in the lap): its palm well under the eyes and close to
// the head across the floor (the standing universe's y is up). `was`: it rested last frame, and
// stays resting until it's restLeave higher or further out.
bool Resting(const Hand &h, const Input &in, const Params &k, bool was) {
    if (!in.haveHead) return false;
    double palm[3];
    h.Palm(palm);
    const double below = in.head.m[1][3] - palm[1];
    const double across = std::hypot(palm[0] - in.head.m[0][3], palm[2] - in.head.m[2][3]);
    const double slack = was ? k.restLeave : 0;
    return below > k.restBelow - slack && across < k.restNear + slack;
}

// How far outside [lo, hi] x is (0 inside).
double Outside(double x, double lo, double hi) { return x < lo ? lo - x : x > hi ? x - hi : 0; }

// The panel and its controls and popups, as u and v ranges.
void Extent(const Panel &p, double *u0, double *u1, double *v0, double *v1) {
    *u0 = -p.width / 2, *u1 = p.width / 2, *v0 = -p.height / 2, *v1 = p.height / 2;
    for (const Spot &s : p.controls)
        *u0 = std::min(*u0, s.u - s.hw), *u1 = std::max(*u1, s.u + s.hw), *v0 = std::min(*v0, s.v - s.hh),
        *v1 = std::max(*v1, s.v + s.hh);
    for (const Popup &q : p.popups)
        *u0 = std::min(*u0, q.u - q.w / 2), *u1 = std::max(*u1, q.u + q.w / 2), *v0 = std::min(*v0, q.v - q.h / 2),
        *v1 = std::max(*v1, q.v + q.h / 2);
}

bool InFootprint(const Panel &p, const Hit &h, double margin) {
    double u0, u1, v0, v1;
    Extent(p, &u0, &u1, &v0, &v1);
    return h.u >= u0 - margin && h.u <= u1 + margin && h.v >= v0 - margin && h.v <= v1 + margin;
}

double PxPerM(const Panel &p) { return (p.floating ? p.cropW : p.pxW) / std::max(1e-6, p.width); }

Mat Facing(const double c[3], const double head[3]) {
    double z[3] = {head[0] - c[0], head[1] - c[1], head[2] - c[2]};
    const double zl = std::sqrt(Dot(z, z));
    if (zl < 1e-6) z[0] = 0, z[1] = 0, z[2] = 1;
    else
        for (double &v : z) v /= zl;
    double x[3] = {z[2], 0, -z[0]};   // up x z
    const double xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = float(y[i]), m.m[i][2] = float(z[i]), m.m[i][3] = float(c[i]);
    return m;
}

// 3x3 parts: a^T b, a b
void MulTA(const Mat &a, const Mat &b, double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = a.m[0][i] * b.m[0][j] + a.m[1][i] * b.m[1][j] + a.m[2][i] * b.m[2][j];
}
void MulRot(const Mat &a, const double b[3][3], Mat *out) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out->m[i][j] = float(a.m[i][0] * b[0][j] + a.m[i][1] * b[1][j] + a.m[i][2] * b[2][j]);
}

// Plain 3x3 rotations (the wrist's turn): a b, a^T b, a b^T.
void Mul3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
void MulT3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
}
void MulBT3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = a[i][0] * b[j][0] + a[i][1] * b[j][1] + a[i][2] * b[j][2];
}
// A rotation's angle (radians) and axis.
double AngleAxis(const double r[3][3], double axis[3]) {
    axis[0] = r[2][1] - r[1][2], axis[1] = r[0][2] - r[2][0], axis[2] = r[1][0] - r[0][1];
    const double s = std::sqrt(Dot(axis, axis)), c = r[0][0] + r[1][1] + r[2][2] - 1;
    if (s > 1e-12)
        for (int i = 0; i < 3; ++i) axis[i] /= s;
    else axis[0] = 1, axis[1] = axis[2] = 0;
    return std::atan2(s, c);
}
// The rotation by `angle` (radians) about a unit axis (Rodrigues).
void FromAxis(const double a[3], double angle, double out[3][3]) {
    const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    out[0][0] = c + t * a[0] * a[0], out[0][1] = t * a[0] * a[1] - s * a[2], out[0][2] = t * a[0] * a[2] + s * a[1];
    out[1][0] = t * a[0] * a[1] + s * a[2], out[1][1] = c + t * a[1] * a[1], out[1][2] = t * a[1] * a[2] - s * a[0];
    out[2][0] = t * a[0] * a[2] - s * a[1], out[2][1] = t * a[1] * a[2] + s * a[0], out[2][2] = c + t * a[2] * a[2];
}
void RotOf(const Mat &m, double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = m.m[i][j];
}

// The hand's frame for its turn: from the wrist toward the middle of the index and little
// knuckles (along), and from the index knuckle to the little one (across). The knuckles hold
// still while the fingers pinch or open, the fingertips don't.
void HandAxes(const Hand &h, fhsmooth::V3 *along, fhsmooth::V3 *across) {
    auto v = [&](int j) { return fhsmooth::V3{h.p[j][0], h.p[j][1], h.p[j][2]}; };
    *along = (v(5) + v(17)) * 0.5 - v(0);
    *across = v(17) - v(5);
}
// ...as a rotation: columns across (made square to along), along, and their cross product.
bool HandFrame(fhsmooth::V3 along, fhsmooth::V3 across, double out[3][3]) {
    const fhsmooth::V3 y = fhsmooth::Norm(along);
    fhsmooth::V3 x = across - y * fhsmooth::Dot(across, y);
    if (fhsmooth::Len(x) < 1e-6) return false;
    x = fhsmooth::Norm(x);
    const fhsmooth::V3 z = fhsmooth::Cross(x, y);
    const fhsmooth::V3 cols[3] = {x, y, z};
    for (int j = 0; j < 3; ++j) out[0][j] = cols[j].x, out[1][j] = cols[j].y, out[2][j] = cols[j].z;
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------- geometry

Hit Locate(const Panel &p, const double pt[3]) {
    double q[3];
    ToLocal(p.pose, pt, q);
    Hit h;
    if (p.curve <= 0) {
        h.u = q[0], h.v = q[1], h.d = q[2];
    } else {
        // the cylinder's axis at (0, *, r), the viewer inside (vr.cpp's OnSurface, RayOnSurface)
        const double r = p.curve;
        h.d = r - std::hypot(q[0], q[2] - r);
        h.u = r * std::atan2(q[0], r - q[2]);
        h.v = q[1];
    }
    return h;
}

Hit Locate(const Panel &p, const float pt[3]) {
    double d[3];
    Copy3(pt, d);
    return Locate(p, d);
}

void FromSurface(const Panel &p, double u, double v, double d, double out[3]) {
    double l[3];
    if (p.curve <= 0) {
        l[0] = u, l[1] = v, l[2] = d;
    } else {
        const double r = p.curve, a = u / r, rho = r - d;
        l[0] = rho * std::sin(a), l[1] = v, l[2] = r - rho * std::cos(a);
    }
    for (int i = 0; i < 3; ++i) out[i] = p.pose.m[i][0] * l[0] + p.pose.m[i][1] * l[1] + p.pose.m[i][2] * l[2] + p.pose.m[i][3];
}

bool ToBuffer(const Panel &p, double u, double v, double *x, double *y) {
    for (const Popup &q : p.popups)   // over the window, a few mm in front of it
        if (std::fabs(u - q.u) <= q.w / 2 && std::fabs(v - q.v) <= q.h / 2 && q.w > 0 && q.h > 0) {
            *x = q.x + (u - (q.u - q.w / 2)) / q.w * q.pw;
            *y = q.y + ((q.v + q.h / 2) - v) / q.h * q.ph;
            return true;
        }
    if (std::fabs(u) > p.width / 2 || std::fabs(v) > p.height / 2) return false;
    if (p.floating) {
        *x = p.cropX + (u / p.width + 0.5) * p.cropW;
        *y = p.cropY + (0.5 - v / p.height) * p.cropH;
    } else {
        *x = (u / p.width + 0.5) * p.pxW;
        *y = (0.5 - v / p.height) * p.pxH;
    }
    *x = std::clamp(*x, 0.0, std::max(0.0, p.pxW - 0.5));
    *y = std::clamp(*y, 0.0, std::max(0.0, p.pxH - 0.5));
    return true;
}

Control ControlAt(const Panel &p, double u, double v, double margin) {
    for (const Spot &s : p.controls)
        if (std::fabs(u - s.u) <= s.hw + margin && std::fabs(v - s.v) <= s.hh + margin) return s.kind;
    return Control::None;
}

const char *ZoneName(Zone z) {
    switch (z) {
        case Zone::Bar: return "the bar";
        case Zone::Tab: return "the resize tab";
        case Zone::Corner: return "a corner";
        case Zone::Edge: return "the border";
        case Zone::Title: return "the title bar";
        case Zone::Card: return "its edge, like a card";
        case Zone::Fist: return "the fist";
        default: return "nothing";
    }
}

Zone GrabZone(const Panel &p, const Hit &h, const Params &k, int *sx, int *sy, double *excess) {
    const double W = p.width / 2, H = p.height / 2, depth = std::fabs(h.d) - k.bandDepth;
    struct Cand { Zone z; double e; int sx, sy; };
    std::vector<Cand> c;   // in priority order
    for (const Spot &s : p.controls)   // (the resize handle: at whichever corner vr.cpp has it now)
        if (s.kind == Control::Tab)
            c.push_back({Zone::Tab, std::max(std::hypot(h.u - s.u, h.v - s.v) - k.cornerReach, depth), s.u >= 0 ? 1 : -1, s.v >= 0 ? 1 : -1});
    for (int cx : {1, -1})
        for (int cy : {-1, 1}) {
            c.push_back({Zone::Corner, std::max(std::hypot(h.u - cx * W, h.v - cy * H) - k.cornerReach, depth), cx, cy});
        }
    for (const Spot &s : p.controls)
        if (s.kind == Control::Bar) {
            const double du = std::max(0.0, std::fabs(h.u - s.u) - s.hw), dv = h.v - s.v;
            c.push_back({Zone::Bar, std::max(std::hypot(du, dv) - k.barReach, depth), 0, 0});
        }
    if (p.floating && p.titleH > 0 && p.cropH > 0) {
        const double band = p.titleH * p.height / p.cropH;
        c.push_back({Zone::Title, std::max({Outside(h.u, -W, W), Outside(h.v, H - band, H), depth}), 0, 0});
    }
    // the border band: bandOut outside the edges (round the corners) to bandIn inside
    const double ou = std::fabs(h.u) - W, ov = std::fabs(h.v) - H;
    const double e = ou > 0 || ov > 0 ? std::hypot(std::max(ou, 0.0), std::max(ov, 0.0)) - k.bandOut
                                      : std::min(-ou, -ov) - k.bandIn;
    c.push_back({Zone::Edge, std::max(e, depth), 0, 0});
    const Cand *best = nullptr;
    for (const Cand &x : c)
        if (x.e <= 0) {
            best = &x;
            break;
        }
    if (!best)
        for (const Cand &x : c)
            if (!best || x.e < best->e) best = &x;
    *sx = best->sx, *sy = best->sy, *excess = best->e;
    return best->z;
}

void CarryAnchor(const Mat &pose, const double grabLocal[3], const double head[3], double out[3]) {
    double p[3];
    for (int i = 0; i < 3; ++i)
        p[i] = pose.m[i][0] * grabLocal[0] + pose.m[i][1] * grabLocal[1] + pose.m[i][2] * grabLocal[2] + pose.m[i][3];
    const double lever = std::sqrt(Dot(grabLocal, grabLocal)), d[3] = {p[0] - head[0], p[1] - head[1], p[2] - head[2]};
    const double s = lever < 1e-6 ? 1.0 : std::min(1.0, std::sqrt(Dot(d, d)) / (kCarryShare * lever));
    for (int i = 0; i < 3; ++i) out[i] = p[i] + s * (pose.m[i][3] - p[i]);
}

Mat CarryPose(const Mat &start, const double startHead[3], const double grabLocal[3], const double pinch[3],
              const double head[3], const Mat &previous, const double (*turn)[3], double rollSnap) {
    // the point that faces the head (CarryAnchor), its share of the way from the hand to the centre
    const double lever = std::sqrt(Dot(grabLocal, grabLocal));
    const double toHead[3] = {pinch[0] - head[0], pinch[1] - head[1], pinch[2] - head[2]};
    const double s = lever < 1e-6 ? 1.0 : std::min(1.0, std::sqrt(Dot(toHead, toHead)) / (kCarryShare * lever));
    // the turn from facing the head the panel had at the start, and the wrist's on top
    double q0[3];
    CarryAnchor(start, grabLocal, startHead, q0);
    double off[3][3];
    MulTA(Facing(q0, startHead), start, off);
    if (turn) {
        double t[3][3];
        Mul3(turn, off, t);
        std::memcpy(off, t, sizeof off);
    }
    Mat r = previous;
    for (int it = 0; it < 4; ++it) {
        double c[3], q[3];
        for (int i = 0; i < 3; ++i) {
            c[i] = pinch[i] - (r.m[i][0] * grabLocal[0] + r.m[i][1] * grabLocal[1] + r.m[i][2] * grabLocal[2]);
            q[i] = pinch[i] + s * (c[i] - pinch[i]);
        }
        // A point near the eyes faces nowhere in particular: a panel pulled in close turns less and
        // less toward them (none within 15 cm, fully from 30), where the facing would spin it round.
        const double away[3] = {q[0] - head[0], q[1] - head[1], q[2] - head[2]};
        const double k = std::clamp((std::sqrt(Dot(away, away)) - 0.15) / 0.15, 0.0, 1.0);
        if (k >= 1) {
            MulRot(Facing(q, head), off, &r);
        } else if (k > 0) {
            Mat f;
            MulRot(Facing(q, head), off, &f);
            double a[3][3], b[3][3], d[3][3], axis[3], step[3][3], out[3][3];
            RotOf(f, a), RotOf(r, b);
            MulBT3(a, b, d);   // from where it is to the facing
            FromAxis(axis, k * AngleAxis(d, axis), step);
            Mul3(step, b, out);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) r.m[i][j] = float(out[i][j]);
        }
        // nearly level: level (about its own front, so it stays facing the same way)
        const double roll = std::atan2(-double(r.m[1][0]), double(r.m[1][1]));
        if (rollSnap > 0 && std::fabs(roll) < rollSnap * M_PI / 180) {
            const double cr = std::cos(roll), sr = std::sin(roll);
            for (int i = 0; i < 3; ++i) {
                const double x = r.m[i][0], y = r.m[i][1];
                r.m[i][0] = float(x * cr + y * sr), r.m[i][1] = float(-x * sr + y * cr);
            }
        }
        for (int i = 0; i < 3; ++i) r.m[i][3] = float(c[i]);
    }
    // the centre for the final turn
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = float(pinch[i] - (r.m[i][0] * grabLocal[0] + r.m[i][1] * grabLocal[1] + r.m[i][2] * grabLocal[2]));
    return r;
}

double ResizeFromCorner(double width, double aspect, int sx, int sy, double cu, double cv, double minW, double maxW,
                        double *u, double *v) {
    // the opposite corner, and the wanted corner projected on the diagonal from it
    const double ou = -sx * width / 2, ov = -sy * aspect * width / 2;
    const double w = std::clamp((sx * (cu - ou) + sy * aspect * (cv - ov)) / (1 + aspect * aspect), minW, maxW);
    *u = ou + sx * w / 2, *v = ov + sy * aspect * w / 2;
    return w;
}

Mat SurfacePose(const Mat &pose, double curve, double u, double v) {
    double l[3][4] = {{1, 0, 0, u}, {0, 1, 0, v}, {0, 0, 1, 0}};
    if (curve > 0) {   // vr.cpp's OnSurface
        const double a = u / curve, c = std::cos(a), sn = std::sin(a);
        l[0][0] = c, l[0][2] = -sn, l[2][0] = sn, l[2][2] = c;
        l[0][3] = curve * sn, l[2][3] = curve - curve * c;
    }
    Mat out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            out.m[i][j] = float(pose.m[i][0] * l[0][j] + pose.m[i][1] * l[1][j] + pose.m[i][2] * l[2][j] + (j == 3 ? pose.m[i][3] : 0));
    return out;
}

Mat StretchPose(const Mat &start, double width0, const double l0[3], const double r0[3], const double l[3],
                const double r[3], double dead, double minW, double maxW, bool scale, double *width) {
    const double v0[3] = {r0[0] - l0[0], r0[1] - l0[1], r0[2] - l0[2]}, v[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
    const double n0 = std::sqrt(Dot(v0, v0)), n = std::sqrt(Dot(v, v));
    double s = 1;
    if (scale && n0 > 0.02) {
        if (n > n0 + dead) s = (n - dead) / n0;
        else if (n < n0 - dead) s = (n + dead) / n0;
    }
    const double w = std::clamp(width0 * s, minW, maxW);
    s = w / width0;
    *width = scale ? w : -1;
    // the turn about the vertical: the change in the line's heading
    double t = 0;
    if (std::hypot(v0[0], v0[2]) > 0.02 && std::hypot(v[0], v[2]) > 0.02)
        t = std::remainder(std::atan2(v[0], v[2]) - std::atan2(v0[0], v0[2]), 2 * M_PI);
    const double c = std::cos(t), sn = std::sin(t);
    auto ry = [&](const double in[3], double out[3]) {
        out[0] = in[0] * c + in[2] * sn, out[1] = in[1], out[2] = -in[0] * sn + in[2] * c;
    };
    const double m0[3] = {(l0[0] + r0[0]) / 2, (l0[1] + r0[1]) / 2, (l0[2] + r0[2]) / 2};
    const double m[3] = {(l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2};
    const double rel[3] = {(start.m[0][3] - m0[0]) * s, (start.m[1][3] - m0[1]) * s, (start.m[2][3] - m0[2]) * s};
    double moved[3];
    ry(rel, moved);
    Mat out{};
    for (int j = 0; j < 3; ++j) {
        const double col[3] = {start.m[0][j], start.m[1][j], start.m[2][j]};
        double rc[3];
        ry(col, rc);
        for (int i = 0; i < 3; ++i) out.m[i][j] = float(rc[i]);
    }
    for (int i = 0; i < 3; ++i) out.m[i][3] = float(m[i] + moved[i]);
    return out;
}

Mat StretchTurn(const Mat &start, double width0, const double l0[3], const double r0[3], const double l[3],
                const double r[3], double twist, double dead, double minW, double maxW, bool scale, double rollSnap,
                double *width) {
    const double v0[3] = {r0[0] - l0[0], r0[1] - l0[1], r0[2] - l0[2]}, v[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
    const double n0 = std::sqrt(Dot(v0, v0)), n = std::sqrt(Dot(v, v));
    double s = 1;
    if (scale && n0 > 0.02) {
        if (n > n0 + dead) s = (n - dead) / n0;
        else if (n < n0 - dead) s = (n + dead) / n0;
    }
    const double w = std::clamp(width0 * s, minW, maxW);
    s = w / width0;
    *width = scale ? w : -1;
    // the line's turn (the shortest from where it was), then the wrists' about it
    double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (n0 > 0.02 && n > 0.02) {
        const double a[3] = {v0[0] / n0, v0[1] / n0, v0[2] / n0}, b[3] = {v[0] / n, v[1] / n, v[2] / n};
        double ax[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const double sn = std::sqrt(Dot(ax, ax)), cs = Dot(a, b);
        if (sn > 1e-9) {
            for (double &x : ax) x /= sn;
            FromAxis(ax, std::atan2(sn, cs), R);
        }
        if (twist != 0) {
            double T[3][3], RT[3][3];
            FromAxis(b, twist, T);
            Mul3(T, R, RT);
            std::memcpy(R, RT, sizeof R);
        }
    }
    const double m0[3] = {(l0[0] + r0[0]) / 2, (l0[1] + r0[1]) / 2, (l0[2] + r0[2]) / 2};
    const double m[3] = {(l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2};
    const double rel[3] = {(start.m[0][3] - m0[0]) * s, (start.m[1][3] - m0[1]) * s, (start.m[2][3] - m0[2]) * s};
    Mat out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.m[i][j] = float(R[i][0] * start.m[0][j] + R[i][1] * start.m[1][j] + R[i][2] * start.m[2][j]);
        out.m[i][3] = float(m[i] + R[i][0] * rel[0] + R[i][1] * rel[1] + R[i][2] * rel[2]);
    }
    // nearly level: level (about its own front)
    const double roll = std::atan2(-double(out.m[1][0]), double(out.m[1][1]));
    if (rollSnap > 0 && std::fabs(roll) < rollSnap * M_PI / 180) {
        const double cr = std::cos(roll), sr = std::sin(roll);
        for (int i = 0; i < 3; ++i) {
            const double x = out.m[i][0], y = out.m[i][1];
            out.m[i][0] = float(x * cr + y * sr), out.m[i][1] = float(-x * sr + y * cr);
        }
    }
    return out;
}

bool HandAtPanel(const Hand &h, const std::vector<Panel> &panels, const Params &k, double front, double margin) {
    double palm[3];
    h.Palm(palm);
    float palmF[3] = {float(palm[0]), float(palm[1]), float(palm[2])};
    for (const Panel &P : panels)
        for (const float *pt : {h.p[8], h.contact, (const float *)palmF}) {
            const Hit t = Locate(P, pt);
            if (t.d > k.nearBack - 0.02 && t.d <= front && InFootprint(P, t, margin)) return true;
        }
    return false;
}

bool HandAtPanel(const handcut::Hands &hands, int side, const std::vector<Panel> &panels, const Params &k, double front,
                 double margin) {
    for (const Hand &h : hands.seen())
        if (h.right == (side == 1) && HandAtPanel(h, panels, k, front, margin)) return true;
    return false;
}

// ----------------------------------------------------------------------------- the machine

namespace {
int64_t g_now = 0;   // Update's time, for the helpers below
const char *SideName(int s) { return s ? "right" : "left"; }
// A point of the hand as float[3] (the palm's centre is computed)
void ToF(const double in[3], float out[3]) { out[0] = float(in[0]), out[1] = float(in[1]), out[2] = float(in[2]); }
}

void HandInput::Log(const char *fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    logs_.push_back(b);
}

const Panel *HandInput::Find(const std::vector<Panel> &panels, int index) const {
    for (const Panel &p : panels)
        if (p.index == index) return &p;
    return nullptr;
}

const Hand *HandInput::HandFor(int side, const std::vector<Hand> &hands, uint32_t prefer) const {
    if (prefer)
        for (const Hand &h : hands)
            if (h.id == prefer) return &h;   // the hand it follows, whatever its side reads now
    const Hand *best = nullptr;
    for (const Hand &h : hands)
        if (h.right == (side == 1) && (!best || h.confidence > best->confidence)) best = &h;
    return best;
}

void HandInput::Emit(Event::Type t, int s, int panel, double x, double y, bool lost) {
    Event e;
    e.type = t, e.side = s, e.panel = panel, e.x = x, e.y = y, e.lost = lost;
    events_.push_back(e);
}

void HandInput::EndPress(int s, bool lost, const char *why) {
    Side &sd = side_[s];
    if (sd.state != State::Press) return;
    if (sd.control == Control::None) {
        Emit(Event::Release, s, sd.panel, sd.lastX, sd.lastY, lost);
        ++stats.releases, stats.lostReleases += lost;
        Log("%s %s on screen %d released at %.0f,%.0f after %.2f s (%s)", SideName(s), sd.byPinch ? "pinch" : "poke",
            sd.panel + 1, sd.lastX, sd.lastY, (g_now - sd.pressedNs) / 1e9,
            why ? why : lost ? "the hand was lost, or hand input was taken away" : "let go");
    }
    sd.state = State::Hover, sd.releasedAt = g_now, sd.below = 0, sd.control = Control::None;
    sd.byPinch = false, sd.attempt.on = false, sd.rearmed = false;
}

void HandInput::EndGrab(int s, bool lost) {
    Side &sd = side_[s];
    if (sd.state != State::Grab) return;
    Emit(Event::GrabEnd, s, sd.panel, 0, 0, lost);
    // let go fast, outward: thrown on along that line (Params::cardThrow)
    const bool carried = sd.zone == Zone::Card || sd.zone == Zone::Fist || sd.zone == Zone::Edge || sd.zone == Zone::Bar ||
                         sd.zone == Zone::Title;
    fhsmooth::V3 v;
    if (!lost && p_.cardThrow && carried && stretch_.panel != sd.panel && haveHead_ && !sd.track.empty() &&
        Velocity(s, sd.upNs ? sd.upNs : 0, p_.throwFitMs, &v)) {
        const double speed = fhsmooth::Len(v);
        // outward: away from the eyes, from where the hand was as the throw's last fitMs began
        fhsmooth::V3 hand = sd.track.back().p;
        const int64_t end = sd.upNs ? sd.upNs : sd.track.back().t;
        for (const Side::Pt &q : sd.track)
            if (q.t >= end - int64_t(p_.throwFitMs * 1e6)) {
                hand = q.p;
                break;
            }
        const fhsmooth::V3 eye{lastHead_.m[0][3], lastHead_.m[1][3], lastHead_.m[2][3]};
        const double out = fhsmooth::Dot(fhsmooth::Norm(v), fhsmooth::Norm(hand - eye));
        const double c[3] = {sd.last.m[0][3], sd.last.m[1][3], sd.last.m[2][3]};
        const fhsmooth::V3 n = fhsmooth::Norm(v);
        const double dir[3] = {n.x, n.y, n.z};
        Mat to;
        if (speed >= p_.throwSpeed && out >= std::cos(p_.throwAngle * M_PI / 180) && ThrowTarget(c, dir, &to)) {
            Log("%s %s let go of screen %d at %.2f m/s, %.0f deg off straight out: thrown", SideName(s),
                sd.byGrip ? "fist" : "pinch", sd.panel + 1, speed, std::acos(std::clamp(out, -1.0, 1.0)) * 180 / M_PI);
            EmitThrow(s, sd.panel, to, false);
        } else if (speed >= 0.5) {
            Log("%s %s let go of screen %d at %.2f m/s, %.0f deg off straight out: not thrown (%.1f m/s and %.0f deg)", SideName(s),
                sd.byGrip ? "fist" : "pinch", sd.panel + 1, speed, std::acos(std::clamp(out, -1.0, 1.0)) * 180 / M_PI,
                p_.throwSpeed, p_.throwAngle);
        }
    }
    ++stats.grabEnds;
    Log("%s %s let go of screen %d after %.2f s%s", SideName(s), sd.byGrip ? "fist" : "pinch", sd.panel + 1,
        (g_now - sd.grabbedNs) / 1e9, lost ? " (the hand was lost, or hand input was taken away)" : "");
    const int other = 1 - s;
    if (stretch_.panel == sd.panel) {
        // two hands to one: the other carries on alone, from where the panel is now
        stretch_.panel = -1;
        Side &o = side_[other];
        if (o.state == State::Grab && o.panel == sd.panel) {
            if (o.zone != Zone::Fist) o.zone = Zone::Edge;
            o.width0 = -1;   // re-seeded in Carries
        }
    }
    sd.state = State::Hover, sd.below = 0, sd.byGrip = sd.fistByPinch = false;
}

void HandInput::ForgetPress(int s) {
    Side &sd = side_[s];
    if (sd.state == State::Press) {
        sd.state = State::Hover, sd.releasedAt = g_now, sd.control = Control::None, sd.byPinch = false;
        sd.attempt.on = false;
    }
}

void HandInput::GrabThatFired(int s) {
    Side &sd = side_[s];
    if (!sd.fistWait && !sd.pinchWait) return;
    Log("%s %s by screen %d: \"grab that\", not a %s", SideName(s), sd.fistWait ? "fist" : "pinch", sd.waitPanel + 1,
        sd.fistWait ? "fist grab" : "pinch");
    sd.fistWait = sd.pinchWait = false, ++stats.fistsCancelled;
}

void HandInput::EndAttempt(int s) {
    Side &sd = side_[s];
    Side::Attempt &a = sd.attempt;
    if (!a.on) return;
    a.on = false;
    if (a.pressed) return;
    ++stats.refused;
    std::string why;
    auto add = [&](const char *t) { why += (why.empty() ? "" : "; ") + std::string(t); };
    char b[200];
    if (a.why & kOffContent) {
        std::snprintf(b, sizeof b, "off the content (a control that takes no poke, or past the edge: at %+.1f, %+.1f cm from "
                      "its centre, %.1f cm outside its edge)", a.u * 100, a.v * 100, a.outside * 100);
        add(b);
    }
    if (a.frames < (a.quick ? 1 : p_.pressFrames)) {
        std::snprintf(b, sizeof b, "past the surface for %d frame(s); one frame presses after %.1f cm in at %.2f m/s, "
                      "it came %.1f cm at %.2f m/s", a.frames, p_.travel * 100, p_.quickSpeed, a.travel * 100, a.speed);
        add(b);
    }
    if (a.why & kLate) std::snprintf(b, sizeof b, "reaching through: past it over %d frames or %.0f cm", p_.lateFrames, -p_.lateDepth * 100), add(b);
    if (a.why & kOneView) add("seen by one camera");
    if (a.why & kFlick) std::snprintf(b, sizeof b, "an open hand coming at %.2f m/s (a flick, not a poke)", a.speed), add(b);
    if (a.why & kOpenHand) add("an open hand (a push, not a poke)");
    if (a.why & kBent) std::snprintf(b, sizeof b, "the index bent (%.2f, needs %.2f)", a.ext, p_.extended), add(b);
    if (a.why & kNotNearest) {
        static const char *tips[] = {"middle", "ring", "little"};
        std::snprintf(b, sizeof b, "another fingertip nearer the panel than the index (the %s %+.1f cm from the index tip's "
                      "depth, the index %.2f straight, the straightest other %.2f, the finger %.0f deg off)",
                      tips[std::clamp((a.otherTip - 12) / 4, 0, 2)], a.otherGap * 100, a.ext, a.straightest, a.angle);
        add(b);
    }
    if (a.why & kPinching) add("pinching or gripping");
    if (a.why & kBusy) add("a laser or the mouse holds the button");
    if (a.why & kOtherHand) add("the other hand is pressing");
    if (a.why & kTooSoon) std::snprintf(b, sizeof b, "within %.0f ms of the last release", p_.rePressMs), add(b);
    if (a.why & kSpent) add("came back from through the panel");
    if (a.why & kNotOut) std::snprintf(b, sizeof b, "not out %.1f cm in front since the last release", p_.rearm * 100), add(b);
    if (a.why & kResting) add("the hand rests low by the body");
    if (a.why & kTyping) add("typing holds the hand back");
    if (a.why & kAskew) std::snprintf(b, sizeof b, "the finger %.0f deg off the panel's normal (at most %.0f)", a.angle, p_.fingerAngle), add(b);
    if (a.why & kSlow) std::snprintf(b, sizeof b, "coming in at %.2f m/s (at least %.2f)", a.speed, p_.approach), add(b);
    if (why.empty()) add("?");
    Log("%s poke on screen %d didn't press (%.1f cm deep at most): %s", SideName(s), sd.panel + 1, a.deepest * 100, why.c_str());
}

void HandInput::Update(const Input &in, const std::vector<Panel> &panels) {
    g_now = in.nowNs;
    if (in.haveHead) lastHead_ = in.head, haveHead_ = true;
    events_.clear(), carries_.clear(), glows_.clear(), hovered_.clear(), corners_.clear(), nearPanels_.clear(), logs_.clear();
    const handcut::Hands &H = *in.hands;
    newFrame_ = H.frame() != frame_;
    frame_ = H.frame();
    if (newFrame_) frameNs_ = H.captureNs(), ++stats.frames, approaching_ = false;
    if (in.haveHead) {
        head_.Set({p_.grabHeadMinCutoff, p_.grabHeadBeta, 1.0});
        const double t = in.nowNs / 1e9;
        head_.Sample({in.head.m[0][3], in.head.m[1][3], in.head.m[2][3]}, t, t);
    }
    for (int s = 0; s < 2; ++s) {
        Side &sd = side_[s];
        if (!in.allowed[s]) {
            EndPress(s, true), EndGrab(s, true), EndAttempt(s);
            sd.state = State::Off, sd.panel = -1;
            sd.knowBegins = sd.knowGrip = sd.fistWait = sd.pinchWait = false;
            sd.hist.clear();
            continue;
        }
        if (sd.state == State::Off) sd.state = State::Idle;
        const Hand *h = HandFor(s, H.seen(), sd.state == State::Idle ? 0 : sd.handId);
        if (h) {
            if (h->id != sd.handId) sd.hist.clear();
            sd.lastSeen = in.nowNs, sd.handId = h->id;
        }
        if (newFrame_ && h) {
            Side::Sample q;
            q.t = h->captureNs;
            std::memcpy(q.tip, h->p[8], sizeof q.tip), std::memcpy(q.thumb, h->p[4], sizeof q.thumb);
            std::memcpy(q.contact, h->contact, sizeof q.contact);
            for (int i = 0; i < 3; ++i) q.along[i] = (h->p[5][i] + h->p[17][i]) / 2 - h->p[0][i];
            {
                double pc[3], n[3];
                h->Palm(pc);
                ToF(pc, q.palm);
                PalmOut(*h, n);
                ToF(n, q.normal);
                q.open = float(Openness(*h)), q.curl3 = float(Curl3(*h));
            }
            if (sd.hist.empty() || q.t > sd.hist.back().t) sd.hist.push_back(q);
            while (sd.hist.size() > 2 && q.t - sd.hist.front().t > 1'000'000'000) sd.hist.erase(sd.hist.begin());
            double palm[3];
            h->Palm(palm);
            for (const Panel &P : panels)
                for (const Hit &t : {Locate(P, h->p[8]), Locate(P, palm)})
                    if (t.d > p_.nearBack && t.d <= p_.rateReach && InFootprint(P, t, 0.10)) approaching_ = true;
        }
        if ((sd.state == State::Press || sd.state == State::Grab) && !Find(panels, sd.panel)) {   // hidden, gone
            EndPress(s, true), EndGrab(s, true);
            sd.state = State::Idle, sd.panel = -1;
        }
        // (holding: through a longer loss, Params::holdLostMs)
        const bool holding = sd.state == State::Grab || sd.state == State::Press;
        if (!h && sd.state != State::Idle && in.nowNs - sd.lastSeen > int64_t((holding ? p_.holdLostMs : p_.lostMs) * 1e6)) {
            if (holding) {
                ++stats.lostEnds;
                Log("%s hand unseen for %.2f s: what it held stays where it is", SideName(s), (in.nowNs - sd.lastSeen) / 1e9);
            }
            EndPress(s, true), EndGrab(s, true), EndAttempt(s);
            sd.state = State::Idle, sd.panel = -1, sd.fistWait = sd.pinchWait = false;
        }
        // which panel it's near first, so a pinch or grip that begins on the frame the hand comes
        // near is ours (the near report for that frame says so, and the helper leaves it)
        if (newFrame_ && h && sd.state != State::Grab && sd.state != State::Press) Near(s, in, panels, h);
        Grab(s, in, panels);
        if (newFrame_ && h && sd.state != State::Grab) {
            if (sd.state == State::Press && sd.byPinch) PinchPress(s, in, panels, h);
            else Poke(s, in, panels, h);
        }
        if (sd.state == State::Press || sd.state == State::Grab) sd.busyNs = in.nowNs;
        if (newFrame_ && h && p_.flick && (sd.state == State::Idle || sd.state == State::Hover)) Flick(s, in, panels, *h);
        if (newFrame_ && h && p_.push && (sd.state == State::Idle || sd.state == State::Hover)) Push(s, in, panels);
    }
    if (newFrame_ && anyNear()) ++stats.nearFrames;
    Carries(in, panels);
    Glows(in, panels);
}

void HandInput::Near(int s, const Input &in, const std::vector<Panel> &panels, const Hand *h) {
    Side &sd = side_[s];
    // the panel it's near: the index tip, the contact, the knuckles or the palm over it, close in
    // front. A resting hand (typing, the lap) is near only within restFront; the panel the hand is
    // near already keeps it nearLeave farther (no flicker of the ray at the zone's edge)
    sd.resting = Resting(*h, in, p_, sd.resting);
    const double margin = std::max(p_.nearMargin, p_.bandOut + 0.005);
    double palm[3];
    h->Palm(palm);
    float palmF[3];
    ToF(palm, palmF);
    int best = -1;
    double key = 1e9;
    sd.inZone = false;
    for (const Panel &P : panels) {
        const double zone = sd.resting ? std::min(p_.nearFront, p_.restFront) : p_.nearFront;
        const double reach = sd.state == State::Hover && P.index == sd.panel ? zone + p_.nearLeave : zone;
        for (const float *pt : {h->p[8], h->contact, h->pinchPoint, (const float *)palmF}) {
            const Hit t = Locate(P, pt);
            if (t.d <= p_.nearBack || t.d > reach || !InFootprint(P, t, margin)) continue;
            if (t.d <= zone) sd.inZone = true;
            if (std::fabs(t.d) < key) key = std::fabs(t.d), best = P.index;
        }
    }
    if (best >= 0) {
        if (sd.state == State::Idle || (sd.state == State::Hover && sd.panel != best)) {
            EndAttempt(s);
            sd.below = 0, sd.hoverX = sd.hoverY = -1, sd.hoverNs = -1;
            sd.spent = false, sd.rearmed = true;
        }
        if (sd.state == State::Idle) sd.state = State::Hover;
        sd.panel = best, sd.outSince = -1;
    } else if (sd.state == State::Hover) {
        if (sd.outSince < 0) sd.outSince = in.nowNs;
        if (in.nowNs - sd.outSince > int64_t(p_.leaveMs * 1e6)) EndAttempt(s), sd.state = State::Idle, sd.panel = -1;
    }
    if (sd.state != State::Hover) sd.inZone = false;
}

void HandInput::Poke(int s, const Input &in, const std::vector<Panel> &panels, const Hand *h) {
    Side &sd = side_[s];
    if (sd.state != State::Hover && sd.state != State::Press) return;
    const Panel *P = Find(panels, sd.panel);
    if (!P) {
        sd.state = State::Idle, sd.panel = -1;
        return;
    }
    const Hit t = Locate(*P, h->p[8]);
    const double ext = Extension(*h);
    const bool extended = ext >= p_.extended;
    double others = 1e9;
    int otherTip = 12;
    for (int j : {12, 16, 20}) {
        const double d = Locate(*P, h->p[j]).d;
        if (d < others) others = d, otherTip = j;
    }
    double straightest = 0;   // of the other fingers
    for (int f : {9, 13, 17}) straightest = std::max(straightest, FingerExtension(*h, f));
    const bool nearest = t.d + p_.nearest <= others || ext - straightest >= p_.pointing;
    double x = 0, y = 0;
    bool over = ToBuffer(*P, t.u, t.v, &x, &y);
    const Control ctl = over ? Control::None : ControlAt(*P, t.u, t.v, 0.01);
    // just past the content's edge (not on a control): at the edge
    const bool edge = !over && ctl == Control::None && FromBorder(*P, t.u, t.v) <= p_.edgeSlack &&
                      ToBuffer(*P, std::clamp(t.u, -P->width / 2, P->width / 2), std::clamp(t.v, -P->height / 2, P->height / 2), &x, &y);
    over = over || edge;
    const bool otherPressing = side_[1 - s].state == State::Press;
    char why[96];

    if (sd.state == State::Press) {
        sd.deepest = std::min(sd.deepest, t.d);
        if (t.d < p_.tooDeep) {   // reaching through the panel isn't a drag
            ++stats.deepReleases;
            std::snprintf(why, sizeof why, "%.1f cm through the panel", -t.d * 100);
            EndPress(s, false, why);
            sd.spent = true;
            return;
        }
        if (t.d >= p_.release || t.d - sd.deepest >= p_.pullBack) {
            if (t.d >= p_.release) std::snprintf(why, sizeof why, "out to %+.1f cm", t.d * 100);
            else std::snprintf(why, sizeof why, "pulled back to %+.1f cm from %+.1f", t.d * 100, sd.deepest * 100);
            EndPress(s, false, why);
            return;
        }
        if (sd.control != Control::None) return;
        if (!sd.dragging && std::hypot(t.u - sd.lockU, t.v - sd.lockV) >= p_.slide) sd.dragging = true;
        if (sd.dragging) {
            const double u = std::clamp(t.u, -P->width / 2, P->width / 2), v = std::clamp(t.v, -P->height / 2, P->height / 2);
            double dx, dy;
            if (ToBuffer(*P, u, v, &dx, &dy) && (std::fabs(dx - sd.lastX) >= 0.5 || std::fabs(dy - sd.lastY) >= 0.5)) {
                sd.lastX = dx, sd.lastY = dy;
                Emit(Event::Motion, s, P->index, dx, dy);
            }
        }
        return;
    }
    // hovering (resting: from the claim above)
    const bool resting = sd.resting;
    if (sd.spent && t.d >= 0) sd.spent = false;
    if (t.d >= p_.rearm) sd.rearmed = true;
    const bool crossed = t.d <= 0;
    sd.below = crossed ? sd.below + 1 : 0;
    // coming in: when it crossed the surface, it came toward it at approach or faster from some
    // point in the last approachMs (or from the frame before, at a low rate); and how far it came
    // within travelMs (from the hand's recent fingertips, wherever they were)
    if (sd.below == 1 && !sd.hist.empty()) {
        sd.fastIn = false, sd.inSpeed = 0;
        // how far it came: from where it was on average approachMs to travelMs before (a mean, so a
        // still hand's jitter doesn't read as travel; with too few samples there, the farthest)
        double sum = 0, far = t.d, prev = 1e9;
        int n = 0;
        const Side::Sample &now = sd.hist.back();
        for (size_t i = 0; i + 1 < sd.hist.size(); ++i) {
            const Side::Sample &q = sd.hist[i];
            const double dt = (now.t - q.t) / 1e9;
            if (dt <= 1e-3) continue;
            const double qd = Locate(*P, q.tip).d;
            if (dt * 1e3 <= p_.travelMs + 20) {
                far = std::max(far, qd);
                if (dt * 1e3 >= p_.approachMs - 20) sum += qd, ++n;
            }
            if (i + 2 == sd.hist.size()) prev = qd;
            if (dt * 1e3 <= p_.approachMs + 20 || i + 2 == sd.hist.size()) {
                const double v = (qd - t.d) / dt;
                sd.inSpeed = std::max(sd.inSpeed, v);
                if (v >= p_.approach) sd.fastIn = true;
            }
        }
        sd.travelled = (n >= 2 ? sum / n : far) - t.d;
        // ...and on its way in on the frame before too (between there and here): a still finger's
        // jitter dips past the surface on one frame from anywhere
        const double base = n >= 2 ? sum / n : far;
        const bool onTheWay = prev < 1e8 && prev - t.d >= 0.002 && base - prev >= 0.005;
        // a short one (under steadyTravel): coming in steadily, each of the last steadySteps frames
        // nearer than the one before by 2 mm or more (a still finger's jitter dips 1.5 cm from 1.5 cm out
        // now and then, but rarely three frames running)
        bool steady = sd.travelled >= p_.steadyTravel;
        if (!steady && int(sd.hist.size()) > p_.steadySteps) {
            steady = true;
            double later = t.d;
            for (int k = 1; k <= p_.steadySteps && steady; ++k) {
                const double qd = Locate(*P, sd.hist[sd.hist.size() - 1 - k].tip).d;
                steady = qd - later >= 0.002, later = qd;
            }
        }
        sd.quick = sd.fastIn && onTheWay && steady && sd.inSpeed >= p_.quickSpeed && sd.travelled >= p_.travel;
    }
    if (over && extended && !resting && !in.typing[s] && t.d <= p_.cursor && t.d > p_.nearBack && !in.pointerBusy && !otherPressing) {
        sd.hoverNs = in.nowNs, sd.hoverU = t.u, sd.hoverV = t.v;
        if (std::fabs(x - sd.hoverX) >= 0.5 || std::fabs(y - sd.hoverY) >= 0.5) {
            sd.hoverX = x, sd.hoverY = y;
            Emit(Event::Motion, s, P->index, x, y);
        }
    }
    // an attempt: from crossing the surface until it's back out past `release` (or leaves); not one
    // off the content and its buttons (a hand reaching for the bar or a corner): counted, logged
    // once, no poke
    Side::Attempt &a = sd.attempt;
    const bool button = ctl == Control::Curve || ctl == Control::Reset || ctl == Control::Dock || ctl == Control::Close;
    if (crossed && !a.on && !over && !button) {
        if (sd.below == 1) {
            ++stats.offContent;
            Log("%s index past screen %d's plane off its content (at %+.1f, %+.1f cm from its centre, %.1f cm outside its "
                "edge%s): not a poke", SideName(s), P->index + 1, t.u * 100, t.v * 100, FromBorder(*P, t.u, t.v) * 100,
                ctl == Control::Bar ? ", on the bar" : ctl == Control::Tab ? ", on the resize tab" : ctl == Control::Roll ? ", on the roll button" : "");
        }
        return;
    }
    if (crossed && !a.on) {
        a = Side::Attempt{};
        a.on = true, a.quick = sd.quick, a.speed = sd.inSpeed, a.travel = sd.travelled;
        ++stats.attempts;
    }
    if (!crossed) {
        if (a.on && t.d >= p_.release) EndAttempt(s);
        return;
    }
    ++a.frames, a.deepest = std::min(a.deepest, t.d);
    a.u = t.u, a.v = t.v, a.over = over, a.outside = FromBorder(*P, t.u, t.v);
    a.otherGap = others - t.d, a.otherTip = otherTip, a.straightest = straightest;
    if (!(over || button)) return (void)(a.why |= kOffContent);   // (slid off it, past the edge)
    if (sd.below < (sd.quick ? 1 : p_.pressFrames)) return;
    if (sd.below > p_.lateFrames || t.d < p_.lateDepth) return (void)(a.why |= kLate);
    const bool fast = sd.fastIn;
    const bool rePress = sd.releasedAt >= 0 && in.nowNs - sd.releasedAt < int64_t(p_.rePressMs * 1e6);
    const handcut::Gesture &pinch = in.hands->pinch(s), &grip = in.hands->grip(s);
    const double angle = FingerAngle(*P, t, *h);
    a.angle = angle, a.ext = ext;
    if (!h->stereo) return (void)(++stats.heldNotStereo, a.why |= kOneView);
    if (!extended) return (void)(++stats.heldNotExtended, a.why |= kBent);
    if (!nearest) return (void)(++stats.heldNotNearest, a.why |= kNotNearest);
    if (p_.flick && sd.inSpeed >= p_.flickPokeSpeed && Openness(*h) >= p_.flickOpen)   // a flick through it
        return (void)(++stats.heldFlick, a.why |= kFlick);
    if (p_.push && ((Curl3(*h) >= p_.pokeOpen && Openness(*h) >= p_.pokeOpen) ||
                    (Curl3(*h) >= p_.pushCurl && sd.inSpeed >= p_.pushSpeed)))   // an open hand: a push, not a poke
        return (void)(++stats.heldOpen, a.why |= kOpenHand);
    if (pinch.down || grip.down) return (void)(++stats.heldPinching, a.why |= kPinching);
    if (in.pointerBusy) return (void)(++stats.heldBusy, a.why |= kBusy);
    if (otherPressing) return (void)(++stats.heldBusy, a.why |= kOtherHand);
    if (rePress) return (void)(++stats.heldBusy, a.why |= kTooSoon);
    if (sd.spent) return (void)(++stats.heldBusy, a.why |= kSpent);
    if (!sd.rearmed) return (void)(++stats.heldBusy, a.why |= kNotOut);
    if (resting) return (void)(++stats.heldResting, a.why |= kResting);
    if (in.typing[s]) return (void)(++stats.heldTyping, a.why |= kTyping);
    if (angle > p_.fingerAngle) return (void)(++stats.heldAskew, a.why |= kAskew);
    if (!fast) return (void)(++stats.heldSlow, a.why |= kSlow);
    // pressed
    a.pressed = true;
    stats.quickPresses += sd.below < p_.pressFrames;
    sd.state = State::Press, sd.lockU = t.u, sd.lockV = t.v, sd.deepest = t.d, sd.dragging = false, sd.byPinch = false;
    sd.pressedNs = in.nowNs;
    if (over) {
        sd.control = Control::None, sd.lastX = x, sd.lastY = y;
        Emit(Event::Press, s, P->index, x, y);
        ++stats.presses, stats.edgePresses += edge;
        Log("%s poke pressed screen %d at %.0f,%.0f%s (%+.1f cm, in at %.2f m/s from %.1f cm out, %d frame(s) past, "
            "the finger %.0f deg off)", SideName(s), P->index + 1, x, y, edge ? " (just past its edge)" : "", t.d * 100,
            sd.inSpeed, sd.travelled * 100, sd.below, angle);
    } else {
        sd.control = ctl;
        Event e;
        e.type = Event::Control, e.side = s, e.panel = P->index, e.control = ctl;
        events_.push_back(e);
        ++stats.controls;
        Log("%s poke pressed a button of screen %d", SideName(s), P->index + 1);
    }
}

void HandInput::PinchPress(int s, const Input &in, const std::vector<Panel> &panels, const Hand *h) {
    Side &sd = side_[s];
    const Panel *P = Find(panels, sd.panel);
    if (!P) return;
    // the knuckles' move since the pinch began, on the panel: they hold still as the fingers close
    const Hit k = Locate(*P, h->pinchPoint);
    const double u = sd.lockU + (k.u - sd.pinchU0), v = sd.lockV + (k.v - sd.pinchV0);
    if (!sd.dragging && std::hypot(u - sd.lockU, v - sd.lockV) >= p_.slide) sd.dragging = true;
    if (!sd.dragging) return;
    double dx, dy;
    if (ToBuffer(*P, std::clamp(u, -P->width / 2, P->width / 2), std::clamp(v, -P->height / 2, P->height / 2), &dx, &dy) &&
        (std::fabs(dx - sd.lastX) >= 0.5 || std::fabs(dy - sd.lastY) >= 0.5)) {
        sd.lastX = dx, sd.lastY = dy;
        Emit(Event::Motion, s, P->index, dx, dy);
    }
}

void HandInput::Grab(int s, const Input &in, const std::vector<Panel> &panels) {
    Side &sd = side_[s];
    const handcut::Gesture &g = in.hands->pinch(s), &gr = in.hands->grip(s);
    if (!sd.knowBegins) sd.begins = g.begins, sd.knowBegins = true;   // not a pinch that began before
    if (!sd.knowGrip) sd.gripBegins = gr.begins, sd.knowGrip = true;
    const bool newPinch = g.begins != sd.begins, newGrip = gr.begins != sd.gripBegins;
    sd.begins = g.begins, sd.gripBegins = gr.begins;
    if (sd.state == State::Grab) {
        bool held;
        if (sd.byGrip) {   // a fist: held while its grip or its pinch is (ft-hands calls a fist either, Params::fistIndex)
            const handcut::Gesture &m = gr.down || !g.down ? gr : g;
            held = StillHeld(s, in, m, gr.down ? newGrip : g.down && newPinch, true);
        } else {
            held = StillHeld(s, in, g, newPinch, false);
        }
        if (!held) EndGrab(s, false);
        return;
    }
    if (sd.state == State::Press && sd.byPinch) {
        if (!StillHeld(s, in, g, newPinch, false)) EndPress(s, false, "the pinch opened");
        return;
    }
    // a fist or pinch waiting on "grab that": it let the hand go (or the wait is over), so it's ours
    const bool waited = in.nowNs - sd.waitSince > int64_t(p_.grabThatWaitMs * 1e6);
    if (sd.fistWait) {
        const bool down = sd.fistByPinch ? g.down : gr.down, again = sd.fistByPinch ? newPinch : newGrip;
        if (!down || again || sd.state != State::Hover || in.typing[s]) sd.fistWait = false;
        else if (!in.grabThatArmed[s] || waited) sd.fistWait = false, FistBegin(s, in, panels, true, sd.fistByPinch);
    }
    if (sd.pinchWait) {
        if (!g.down || newPinch || sd.state != State::Hover || in.typing[s]) sd.pinchWait = false;
        else if (!in.grabThatArmed[s] || waited) sd.pinchWait = false, PinchBegin(s, in, panels, true);
    }
    if (newGrip && gr.down) FistBegin(s, in, panels, false);
    // (in the zone's margin the hand isn't ours: the pointer helper has its pinches). A pinch by a
    // closed hand is a fist (Params::fistIndex): the thumb on the index of a fist reads as one.
    if (newPinch && g.down && sd.state == State::Hover && sd.inZone) {
        const Hand *h = HandFor(s, in.hands->seen(), g.handId ? g.handId : sd.handId);
        if (p_.fistGrab && h && Extension(*h) <= p_.fistIndex && Curl3(*h) <= p_.fistCurl) FistBegin(s, in, panels, false, true);
        else PinchBegin(s, in, panels, false);
    }
}

bool HandInput::StillHeld(int s, const Input &in, const handcut::Gesture &g, bool isNew, bool grip) {
    Side &sd = side_[s];
    if (g.down) {
        if (sd.upFrames > 0 || isNew) {
            ++stats.heldThrough;
            Log("%s %s held on through %d frame(s) %s", SideName(s), grip ? "fist" : "pinch", sd.upFrames,
                isNew ? "(it ended and began again)" : "with it up");
        }
        sd.upFrames = 0;
        return true;
    }
    if (!newFrame_) return true;   // (counted on hands frames)
    const Hand *h = HandFor(s, in.hands->seen(), sd.handId);
    if (!h) return true;           // unseen: holdLostMs decides
    const bool closed = grip ? Openness(*h) < p_.holdGripOpen : Dist3(h->p[4], h->p[8]) < p_.holdPinchOpen;
    if (sd.upFrames == 0) sd.upNs = h->captureNs;   // (a throw's velocity is taken up to here)
    ++sd.upFrames;
    if (closed && g.lost) {        // ft-hands lost it, and the hand is back still closed: held
        sd.upFrames = 0;
        return true;
    }
    return sd.upFrames < (closed ? p_.releaseMaxFrames : p_.releaseFrames);
}

bool HandInput::Velocity(int s, int64_t endNs, double ms, fhsmooth::V3 *out) const {
    const std::vector<Side::Pt> &tr = side_[s].track;
    if (tr.empty()) return false;
    const int64_t end = endNs ? endNs : tr.back().t, from = end - int64_t(ms * 1e6);
    double mt = 0;
    fhsmooth::V3 mp;
    int n = 0;
    for (const Side::Pt &q : tr)
        if (q.t >= from && q.t <= end) mt += (q.t - end) / 1e9, mp = mp + q.p, ++n;
    if (n < 3) return false;
    mt /= n, mp = mp * (1.0 / n);
    double tt = 0;
    fhsmooth::V3 tp;
    for (const Side::Pt &q : tr)
        if (q.t >= from && q.t <= end) {
            const double dt = (q.t - end) / 1e9 - mt;
            tt += dt * dt, tp = tp + (q.p - mp) * dt;
        }
    if (tt < 1e-6) return false;
    *out = tp * (1 / tt);
    return true;
}

fhsmooth::V3 HandInput::Shown(int s, double now) {
    Side &sd = side_[s];
    if (sd.coasting) {   // the last velocity, slowing to a stop over coastMs
        const double dt = std::max(0.0, now - sd.coastNs / 1e9), T = std::max(1e-3, p_.coastMs / 1000);
        return sd.coastFrom + sd.coastVel * (dt < T ? dt - dt * dt / (2 * T) : T / 2);
    }
    fhsmooth::V3 p = sd.point.At(now);
    if (sd.blending) {   // from where the coast left it to the hand: smoothly, at most catchUp (m/s) on average
        const double T = std::max({1e-3, p_.blendMs / 1000, fhsmooth::Len(sd.blendOff) / std::max(0.1, p_.catchUp)});
        const double u = (now - sd.blendNs / 1e9) / T;
        if (u >= 1) sd.blending = false;
        else p = p + sd.blendOff * (1 - u * u * (3 - 2 * u));
    }
    return p;
}

double HandInput::Weight(const Input &in, const Hand &h) {
    double w = h.stereo ? 1.0 : p_.monoWeight;
    // the edge of view: how far the knuckles are off the head's forward (as the cameras saw them)
    const Mat &H = in.hands->captureHead();
    const double f[3] = {-H.m[0][2], -H.m[1][2], -H.m[2][2]};
    const double d[3] = {h.pinchPoint[0] - H.m[0][3], h.pinchPoint[1] - H.m[1][3], h.pinchPoint[2] - H.m[2][3]};
    const double l = std::sqrt(Dot(d, d));
    if (l < 1e-6) return w;
    const double a = std::acos(std::clamp(Dot(f, d) / l, -1.0, 1.0)) * 180 / M_PI;
    if (a >= p_.edgeLostDeg) return 0;
    if (a > p_.edgeDeg)
        w = std::min(w, 1 - (1 - p_.monoWeight) * (a - p_.edgeDeg) / std::max(1e-6, p_.edgeLostDeg - p_.edgeDeg));
    return w;
}

void HandInput::Track(int s, const Input &in, const Hand *h) {
    Side &sd = side_[s];
    const double now = in.nowNs / 1e9;
    bool ok = h != nullptr, jumped = false;
    double w = 1;
    fhsmooth::V3 raw;
    if (ok) {
        raw = {h->pinchPoint[0], h->pinchPoint[1], h->pinchPoint[2]};
        if (p_.gate) {
            w = Weight(in, *h);
            if (w <= 0) {   // past the edge of view: as unseen (the panel holds)
                ok = false, ++stats.edgeFrames;
            } else if (!sd.track.empty() && h->captureNs > sd.track.back().t) {
                // an impossible move from the last frame kept (Params::gate): dropped, unless it's the
                // gateFrames-th in a row at one new place (it did move: the blend takes it there)
                const Side::Pt &last = sd.track.back();
                const double dt = (h->captureNs - last.t) / 1e9;
                fhsmooth::V3 v;
                if (!Velocity(s, 0, p_.velocityMs, &v)) v = {};
                if (fhsmooth::Len(v) > p_.gateSpeed) v = v * (p_.gateSpeed / fhsmooth::Len(v));
                const double step = fhsmooth::Len(raw - last.p) / dt, off = fhsmooth::Len(raw - (last.p + v * dt));
                const bool bad = dt < 0.3 && (step > p_.gateSpeed || off > 0.5 * p_.gateAccel * dt * dt + p_.gateNoise);
                if (bad) {
                    const double since = sd.drops ? (h->captureNs - sd.droppedNs) / 1e9 : 0;
                    const bool agrees = sd.drops > 0 && since < 0.3 && fhsmooth::Len(raw - sd.dropped) <= p_.gateNoise + p_.gateSpeed * since;
                    sd.drops = agrees ? sd.drops + 1 : 1, sd.dropped = raw, sd.droppedNs = h->captureNs;
                    if (sd.drops < p_.gateFrames) {
                        ++stats.gated;
                        // dropped frames for a while (the hand nowhere sane): coast as if unseen
                        if (h->captureNs - last.t > 150'000'000) ok = false;
                        else return;
                    } else {
                        jumped = true, sd.drops = 0, ++stats.moved;
                    }
                } else {
                    // kept after drops, somewhere else (the gap let it through): it moved there, as above
                    if (sd.drops > 0 && fhsmooth::Len(raw - last.p) > 2 * p_.gateNoise) jumped = true, ++stats.moved;
                    sd.drops = 0;
                }
            }
            if (ok) stats.monoFrames += !h->stereo, stats.edgeFrames += h->stereo && w < 1;
        } else {
            if (!sd.track.empty() && h->captureNs - sd.track.back().t < 200'000'000 && fhsmooth::Len(raw - sd.track.back().p) > p_.jumpMax)
                jumped = true;
            if (jumped && ++sd.jumps < p_.jumpFrames) ok = false, ++stats.glitches;   // a glitch: as unseen
            else if (!jumped) sd.jumps = 0;
        }
    }
    const fhsmooth::Params smooth{p_.grabMinCutoff * w, p_.grabBeta * w, 1.0};
    if (!ok) {
        if (!sd.coasting && sd.point.Ready()) {
            fhsmooth::V3 v;
            if (!Velocity(s, 0, p_.velocityMs, &v)) v = {};
            if (p_.gate && fhsmooth::Len(v) > p_.coastSpeed) v = v * (p_.coastSpeed / fhsmooth::Len(v));
            sd.coastFrom = Shown(s, now), sd.coastVel = v, sd.coastNs = in.nowNs;
            sd.coasting = true, sd.blending = false;
            ++stats.coasts;
        }
        if (sd.unseenNs < 0) sd.unseenNs = in.nowNs;
        return;
    }
    if (sd.coasting || jumped) {
        // back (or somewhere else, jumpFrames in a row): the smoothing afresh from the hand, moving at
        // the gap's average velocity (the missed path, inferred: from the last point seen to this one),
        // and the shown point blending over to it from where the coast had it; the track afresh (a
        // throw's velocity doesn't see the gap)
        const fhsmooth::V3 shown = Shown(s, now);
        fhsmooth::V3 rate;
        if (!sd.track.empty() && h->captureNs > sd.track.back().t && !jumped)
            rate = (raw - sd.track.back().p) * (1e9 / double(h->captureNs - sd.track.back().t));
        if (p_.gate && fhsmooth::Len(rate) > p_.coastSpeed) rate = rate * (p_.coastSpeed / fhsmooth::Len(rate));
        sd.point.Set(smooth);
        sd.point.Reset();
        sd.point.Sample(raw, in.hands->captureNs() / 1e9, now);
        sd.point.Seed(rate);
        sd.coasting = false;
        sd.blendOff = shown - raw, sd.blendNs = in.nowNs, sd.blending = true;
        sd.track.clear(), sd.jumps = 0;
        ++stats.reacquired, ++sd.reacquired;
        Log("%s hand %s after %.0f ms holding screen %d: %.1f cm from where the panel went", SideName(s),
            jumped && p_.gate ? "moved (the gate took the new place)" : "back",
            sd.unseenNs >= 0 ? (in.nowNs - sd.unseenNs) / 1e6 : 0.0, sd.panel + 1, fhsmooth::Len(shown - raw) * 100);
    } else {
        sd.point.Set(smooth);
        sd.point.Sample(raw, in.hands->captureNs() / 1e9, now);
    }
    sd.unseenNs = -1, sd.weight = w;
    sd.track.push_back({h->captureNs, raw});
    while (sd.track.size() > 2 && h->captureNs - sd.track.front().t > 500'000'000) sd.track.erase(sd.track.begin());
    // the wrist's turn: not from one camera's frame or one near the edge of view (their frames
    // flip), nor a turn no wrist makes (Params::gateTurn)
    if (!p_.gate) SampleTurn(s, *h, in);
    else if (h->stereo && w >= 0.7 && TurnOk(s, *h)) SampleTurn(s, *h, in, w);
}

bool HandInput::TurnOk(int s, const Hand &h) {
    Side &sd = side_[s];
    fhsmooth::V3 along, across;
    HandAxes(h, &along, &across);
    double R[3][3], R0[3][3], rel[3][3], axis[3];
    if (!HandFrame(along, across, R)) return false;
    auto keep = [&] {
        sd.turnAlong = along, sd.turnAcross = across, sd.turnNs = h.captureNs, sd.turnDrops = 0;
        return true;
    };
    if (sd.turnNs == 0 || h.captureNs <= sd.turnNs || h.captureNs - sd.turnNs > 300'000'000 || !HandFrame(sd.turnAlong, sd.turnAcross, R0))
        return keep();
    MulBT3(R, R0, rel);
    const double deg = AngleAxis(rel, axis) * 180 / M_PI, dt = (h.captureNs - sd.turnNs) / 1e9;
    if (deg <= std::max(p_.gateTurnMin, p_.gateTurn * dt)) return keep();
    // a flip: dropped, unless gateFrames in a row agree on the new turn
    bool agrees = false;
    if (sd.turnDrops > 0 && HandFrame(sd.droppedAlong, sd.droppedAcross, R0)) {
        MulBT3(R, R0, rel);
        agrees = AngleAxis(rel, axis) * 180 / M_PI <= p_.gateTurnMin;
    }
    sd.turnDrops = agrees ? sd.turnDrops + 1 : 1, sd.droppedAlong = along, sd.droppedAcross = across;
    if (sd.turnDrops >= p_.gateFrames) return keep();
    ++stats.turnsGated;
    return false;
}

bool HandInput::Straddled(int s, const Panel &P, int64_t now) const {
    const Side &sd = side_[s];
    const double cosMax = std::cos(p_.cardAngle * M_PI / 180);
    for (const Side::Sample &q : sd.hist) {
        if (now - q.t > int64_t(p_.cardMs * 1e6) || q.t > now) continue;
        const Hit ti = Locate(P, q.tip), th = Locate(P, q.thumb), c = Locate(P, q.contact);
        if (ti.d * th.d >= 0 || std::min(std::fabs(ti.d), std::fabs(th.d)) < p_.cardSide) continue;
        const double du = ti.u - th.u, dv = ti.v - th.v, dd = ti.d - th.d, len = std::sqrt(du * du + dv * dv + dd * dd);
        if (len < 1e-6 || std::fabs(dd) / len < cosMax) continue;
        // the hand along the panel, not pointing into it (a front pinch whose index went through)
        double al[3];
        for (int i = 0; i < 3; ++i) al[i] = P.pose.m[0][i] * q.along[0] + P.pose.m[1][i] * q.along[1] + P.pose.m[2][i] * q.along[2];
        const double alen = std::sqrt(Dot(al, al));
        if (alen < 1e-6 || std::fabs(al[2]) / alen > std::cos(p_.cardAlong * M_PI / 180)) continue;
        if (std::fabs(FromBorder(P, c.u, c.v)) <= p_.cardBand) return true;
    }
    return false;
}

bool HandInput::CardFingers(const Hand &h, const char **how) const {
    if (Dist3(h.p[4], h.p[12]) <= p_.cardMiddle && Dist3(h.p[8], h.p[12]) <= p_.cardTogether) {
        if (how) *how = "the index and middle against the thumb";
        return true;
    }
    for (int j : {8, 12, 16, 20})
        if (Dist3(h.p[4], h.p[j]) > p_.cardBunch) return false;
    if (how) *how = "the fingertips bunched";
    return true;
}

void HandInput::PinchBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited) {
    Side &sd = side_[s];
    const handcut::Gesture &g = in.hands->pinch(s);
    if (in.typing[s]) return;
    if (!waited && in.grabThatArmed[s]) {   // "grab that" may be under way: wait for it to fire or not
        sd.pinchWait = true, sd.waitSince = in.nowNs, sd.waitPanel = sd.panel;
        ++stats.fistsWaited;
        return;
    }
    const Hand *h = HandFor(s, in.hands->seen(), g.handId ? g.handId : sd.handId);
    // where the fingers meet (the gestures file's point is the knuckles, 5-8 cm behind)
    const float *c = h ? h->contact : g.beginPoint;
    // the card pinch: the thumb and index tips were either side of a panel's plane at its border, and
    // the middle finger is with the index (or all four bunched): a plain pinch there is a border move,
    // a card if the middle joins it soon (cardCandidate)
    const Panel *straddled = nullptr;
    if (p_.cardPinch && h) {
        double key = 1e9;
        for (const Panel &P : panels) {
            if (P.held) continue;
            const Hit b = Locate(P, c);
            if (std::fabs(b.d) <= p_.bandDepth && std::fabs(FromBorder(P, b.u, b.v)) <= p_.cardBand + 0.01 &&
                Straddled(s, P, h->captureNs) && std::fabs(b.d) < key)
                key = std::fabs(b.d), straddled = &P;
        }
        const char *how = nullptr;
        if (straddled && CardFingers(*h, &how)) {
            ++stats.cards;
            StartGrab(s, in, *straddled, Zone::Card, 1, -1, h, g.point, false);
            Log("%s card: %s", SideName(s), how);
            return;
        }
    }
    // a grab zone (the border band, the bar, the tab, a floating window's title bar); the hand is
    // ours (claimed), so a little farther out than the band's depth counts (Params::pinchSlack)
    Params kz = p_;
    kz.bandDepth += p_.pinchSlack;
    const Panel *bestP = nullptr;
    Zone zone = Zone::None;
    int sx = 1, sy = -1;
    double key = 1e9;
    for (const Panel &P : panels) {
        if (P.held) continue;
        const Hit b = Locate(P, c);
        int x, y;
        double e;
        const Zone z = GrabZone(P, b, kz, &x, &y, &e);
        if (e <= 0 && std::fabs(b.d) < key) key = std::fabs(b.d), bestP = &P, zone = z, sx = x, sy = y;
    }
    if (bestP) {
        StartGrab(s, in, *bestP, zone, sx, sy, h, g.point, false);
        sd.cardCandidate = straddled == bestP && (zone == Zone::Edge || zone == Zone::Bar || zone == Zone::Title);
        return;
    }
    if (straddled) {   // straddling the edge, out of the band's zones: a border move, a card if the fingers join
        StartGrab(s, in, *straddled, Zone::Edge, 1, -1, h, g.point, false);
        sd.cardCandidate = true;
        return;
    }
    // the content: a click where the pointer is (the fingertip's cursor, or where the fingers meet)
    const Panel *P = Find(panels, sd.panel);
    if (!P) return;
    const Hit t = Locate(*P, c);
    double u = t.u, v = t.v, x, y;
    if (sd.hoverNs >= 0 && in.nowNs - sd.hoverNs <= int64_t(p_.pinchCursorMs * 1e6) &&
        std::hypot(sd.hoverU - t.u, sd.hoverV - t.v) <= p_.pinchCursor)
        u = sd.hoverU, v = sd.hoverV;
    if (!p_.pinchClick || !h) return;
    if (t.d > p_.nearFront || t.d < p_.nearBack) {
        Log("%s pinch near screen %d: the fingers %+.1f cm from it, out of the touch zone: nothing", SideName(s), P->index + 1, t.d * 100);
        return;
    }
    if (!ToBuffer(*P, u, v, &x, &y)) {
        Log("%s pinch near screen %d: not on its content or a grab zone (%.1f cm from its border, %+.1f cm out): nothing",
            SideName(s), P->index + 1, FromBorder(*P, t.u, t.v) * 100, t.d * 100);
        return;
    }
    // (not from a hand resting low by the body, typing's thumbs touch fingers too; nor one camera's)
    if (in.pointerBusy || side_[1 - s].state == State::Press || sd.resting || !h->stereo) {
        Log("%s pinch on screen %d: no click (%s)", SideName(s), P->index + 1,
            in.pointerBusy ? "a laser or the mouse holds the button" : side_[1 - s].state == State::Press ? "the other hand is pressing"
            : sd.resting ? "the hand rests low by the body" : "seen by one camera");
        return;
    }
    EndAttempt(s);
    const Hit k = Locate(*P, h->pinchPoint);
    sd.state = State::Press, sd.byPinch = true, sd.control = Control::None, sd.dragging = false, sd.upFrames = 0;
    sd.lockU = u, sd.lockV = v, sd.pinchU0 = k.u, sd.pinchV0 = k.v, sd.lastX = x, sd.lastY = y, sd.deepest = t.d;
    sd.pressedNs = in.nowNs;
    if (std::fabs(x - sd.hoverX) >= 0.5 || std::fabs(y - sd.hoverY) >= 0.5) Emit(Event::Motion, s, P->index, x, y);
    sd.hoverX = x, sd.hoverY = y;
    Emit(Event::Press, s, P->index, x, y);
    ++stats.presses, ++stats.pinchClicks;
    Log("%s pinch pressed screen %d at %.0f,%.0f (on its content, %+.1f cm out)", SideName(s), P->index + 1, x, y, t.d * 100);
}

void HandInput::FistBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited, bool byPinch) {
    Side &sd = side_[s];
    const handcut::Gesture &gr = byPinch ? in.hands->pinch(s) : in.hands->grip(s);
    if (!p_.fistGrab || sd.state != State::Hover || !sd.inZone || in.typing[s]) return;
    const Panel *P = Find(panels, sd.panel);
    if (!P || P->held) return;
    const Hand *h = HandFor(s, in.hands->seen(), gr.handId ? gr.handId : sd.handId);
    if (!h) return;
    // the hand at the panel: its palm, knuckles or curled middle finger within the near zone, over it
    // or by its edge (a fist holding a side has its palm beside it)
    double palm[3];
    h->Palm(palm);
    float palmF[3];
    ToF(palm, palmF);
    double nearest = 1e9;
    bool at = false;
    for (const float *pt : {(const float *)palmF, h->pinchPoint, h->p[10]}) {
        const Hit t = Locate(*P, pt);
        nearest = std::min(nearest, t.d);
        if (t.d > p_.nearBack && t.d <= p_.nearFront && InFootprint(*P, t, std::max(p_.nearMargin, p_.bandOut))) at = true;
    }
    if (!at) {
        Log("%s fist by screen %d: not at it (the knuckles or palm %.1f cm out): nothing", SideName(s), P->index + 1, nearest * 100);
        return;
    }
    if (!waited && in.grabThatArmed[s]) {   // "grab that" may be under way: wait for it to fire or not
        sd.fistWait = true, sd.fistByPinch = byPinch, sd.waitSince = in.nowNs, sd.waitPanel = P->index;
        ++stats.fistsWaited;
        return;
    }
    ++stats.fists, stats.fistPinches += byPinch;
    StartGrab(s, in, *P, Zone::Fist, 1, -1, h, gr.point, true);
    sd.fistByPinch = byPinch;
    if (byPinch) Log("%s fist on screen %d: ft-hands calls it a pinch (the hand %.2f open)", SideName(s), P->index + 1, Openness(*h));
}

void HandInput::StartGrab(int s, const Input &in, const Panel &P, Zone zone, int sx, int sy, const Hand *h,
                          const float *fallback, bool byGrip) {
    Side &sd = side_[s];
    EndAttempt(s);
    // the knuckles as the cameras saw them (the smoothing starts afresh from them: Carried)
    double pinch[3];
    if (h) Copy3(h->pinchPoint, pinch);
    else Copy3(fallback, pinch);
    sd.state = State::Grab, sd.panel = P.index, sd.zone = zone, sd.sx = sx, sd.sy = sy, sd.byGrip = byGrip;
    sd.start = sd.last = P.pose, sd.width0 = P.width;
    sd.grabbedNs = in.nowNs;
    sd.point.Set({p_.grabMinCutoff, p_.grabBeta, 1.0});
    sd.point.Reset();
    sd.point.Sample({pinch[0], pinch[1], pinch[2]}, in.hands->captureNs() / 1e9, in.nowNs / 1e9);
    sd.from = {pinch[0], pinch[1], pinch[2]}, sd.shift = {}, sd.engaged = false;
    sd.upFrames = 0, sd.coasting = sd.blending = false, sd.jumps = 0, sd.unseenNs = -1, sd.reacquired = 0;
    sd.cardCandidate = false, sd.drops = sd.turnDrops = 0, sd.turnNs = 0, sd.weight = 1;
    sd.track.assign(1, {h ? h->captureNs : in.hands->captureNs(), sd.from});
    sd.along.Reset(), sd.across.Reset(), sd.haveRot0 = sd.turning = false;   // the wrist's turn: afresh
    if (h) {
        if (p_.gate) TurnOk(s, *h);   // (the gate's first frame)
        SampleTurn(s, *h, in);
    }
    ToLocal(P.pose, pinch, sd.grabLocal);
    if (in.haveHead) {
        const fhsmooth::V3 hd = head_.Last();
        sd.startHead[0] = hd.x, sd.startHead[1] = hd.y, sd.startHead[2] = hd.z;
    } else {
        std::copy(pinch, pinch + 3, sd.startHead), sd.startHead[2] += 1;
    }
    const Hit n = Locate(P, pinch);
    sd.grabX = n.u - sx * P.width / 2, sd.grabY = n.v - sy * P.height / 2;
    Event e;
    e.type = Event::GrabStart, e.side = s, e.panel = P.index, e.zone = zone, e.sx = sx, e.sy = sy;
    events_.push_back(e);
    ++stats.grabs;
    const bool resize = zone == Zone::Tab || zone == Zone::Corner;
    Log("%s %s took screen %d by %s (%s)", SideName(s), byGrip ? "fist" : "pinch", P.index + 1, ZoneName(zone),
        resize ? "resize" : zone == Zone::Card ? "it goes with the hand" : "move, the wrist turns it");
}

void HandInput::SampleTurn(int s, const Hand &h, const Input &in, double w) {
    Side &sd = side_[s];
    fhsmooth::V3 along, across;
    HandAxes(h, &along, &across);
    const fhsmooth::Params k{p_.rotMinCutoff * w, p_.rotBeta * w, 1.0};
    sd.along.Set(k), sd.across.Set(k);
    const double tc = h.captureNs / 1e9, now = in.nowNs / 1e9;
    sd.along.Sample(along, tc, now), sd.across.Sample(across, tc, now);
}

bool HandInput::Turn(int s, const Input &in, const double pinch[3], const double head[3], double out[3][3]) {
    Side &sd = side_[s];
    if (!p_.grabRotate || !sd.along.Ready() || !sd.across.Ready()) return false;
    const double now = in.nowNs / 1e9;
    double hand[3][3], face[3][3], rel[3][3];
    if (!HandFrame(sd.along.At(now), sd.across.At(now), hand)) return false;
    // the hand, in the frame facing the head across the floor from the pinch point: a hand carried
    // round you turns with it (no turn), and one below the eyes still turns about the vertical
    // when it turns about the vertical (a frame pitched toward the eyes would mix yaw and roll)
    const double level[3] = {head[0], pinch[1], head[2]};
    RotOf(Facing(pinch, level), face);
    MulT3(face, hand, rel);
    if (!sd.haveRot0) {
        std::memcpy(sd.rot0, rel, sizeof rel);
        sd.haveRot0 = true, sd.turning = false;
        return false;
    }
    MulBT3(rel, sd.rot0, out);   // its turn since the start
    if (!sd.turning) {
        double axis[3];
        const double a = AngleAxis(out, axis), dead = p_.rotDeadzone * M_PI / 180;
        if (a <= dead) return false;
        // out of the dead zone: the start moves on by it, so the turn starts from nothing
        double by[3][3], moved[3][3];
        FromAxis(axis, dead, by);
        Mul3(by, sd.rot0, moved);
        std::memcpy(sd.rot0, moved, sizeof moved);
        MulBT3(rel, sd.rot0, out);
        sd.turning = true;
    }
    return true;
}

bool HandInput::RoomTurn(int s, const Input &in, double out[3][3]) {
    Side &sd = side_[s];
    if (!sd.along.Ready() || !sd.across.Ready()) return false;
    double hand[3][3];
    if (!HandFrame(sd.along.At(in.nowNs / 1e9), sd.across.At(in.nowNs / 1e9), hand)) return false;
    if (!sd.haveRot0) {
        std::memcpy(sd.rot0, hand, sizeof hand);
        sd.haveRot0 = true, sd.turning = false;
        return false;
    }
    MulBT3(hand, sd.rot0, out);   // the hand's turn in the room since the grab
    if (!sd.turning) {
        double axis[3];
        const double a = AngleAxis(out, axis), dead = p_.cardRotDeadzone * M_PI / 180;
        if (a <= dead) return false;
        double by[3][3], moved[3][3];
        FromAxis(axis, dead, by);
        Mul3(by, sd.rot0, moved);
        std::memcpy(sd.rot0, moved, sizeof moved);
        MulBT3(hand, sd.rot0, out);
        sd.turning = true;
    }
    return true;
}

bool HandInput::Carried(int s, const Input &in, double out[3], bool deadzone) {
    Side &sd = side_[s];
    if (!sd.point.Ready()) return false;
    fhsmooth::V3 p = Shown(s, in.nowNs / 1e9);
    if (!sd.engaged && fhsmooth::Len(p - sd.from) > p_.grabDeadzone) sd.engaged = true, sd.shift = p - sd.from;
    if (deadzone) p = sd.engaged ? p - sd.shift : sd.from;
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
    return true;
}

void HandInput::Carries(const Input &in, const std::vector<Panel> &panels) {
    double head[3] = {0, 0, 0};
    if (in.haveHead) {
        const fhsmooth::V3 hd = head_.At(in.nowNs / 1e9);
        head[0] = hd.x, head[1] = hd.y, head[2] = hd.z;
    }
    // each grabbing hand's new frame into its smoothing: the pinch point as the cameras saw it
    // (unseen, or a glitch: it coasts, Params::holdLostMs)
    if (newFrame_)
        for (int s = 0; s < 2; ++s)
            if (side_[s].state == State::Grab) Track(s, in, HandFor(s, in.hands->seen(), side_[s].handId));
    // a border move begun straddling the edge: the middle finger joining within cardJoinMs makes it a
    // card, from where it is now
    if (newFrame_)
        for (int s = 0; s < 2; ++s) {
            Side &sd = side_[s];
            if (sd.state != State::Grab || !sd.cardCandidate) continue;
            if (in.nowNs - sd.grabbedNs > int64_t(p_.cardJoinMs * 1e6) || side_[1 - s].state == State::Grab) {
                sd.cardCandidate = false;
                continue;
            }
            const Hand *h = HandFor(s, in.hands->seen(), sd.handId);
            const Panel *P = Find(panels, sd.panel);
            const char *how = nullptr;
            if (!h || !P || !CardFingers(*h, &how) || !sd.point.Ready()) continue;
            sd.cardCandidate = false, sd.zone = Zone::Card;
            sd.start = sd.last = P->pose, sd.from = Shown(s, in.nowNs / 1e9), sd.shift = {}, sd.engaged = false;
            sd.haveRot0 = sd.turning = false;
            ++stats.cards;
            Log("%s pinch on screen %d became a card (%s, %.0f ms in)", SideName(s), P->index + 1, how,
                (in.nowNs - sd.grabbedNs) / 1e6);
        }
    // (a stretch has its own dead zone, on the hands' distance)
    auto pinchOf = [&](int s, double out[3]) { return Carried(s, in, out, false); };
    for (int s = 0; s < 2; ++s) {
        Side &sd = side_[s];
        if (sd.state != State::Grab) continue;
        const Panel *P = Find(panels, sd.panel);
        if (!P) continue;
        Side &o = side_[1 - s];
        const bool both = o.state == State::Grab && o.panel == sd.panel;
        if (both && s == 1) continue;   // the left one does it for both
        double p[3];
        if (both) {
            double l[3], r[3];
            if (!pinchOf(0, l) || !pinchOf(1, r)) continue;
            if (stretch_.panel != sd.panel) {
                stretch_.panel = sd.panel, stretch_.start = P->pose, stretch_.width0 = P->width;
                std::copy(l, l + 3, stretch_.l0), std::copy(r, r + 3, stretch_.r0);
                // each hand's frame now, for their twist about the line between them
                for (int k = 0; k < 2; ++k) {
                    Side &x = side_[k];
                    stretch_.haveHand0[k] = x.along.Ready() && x.across.Ready() &&
                                            HandFrame(x.along.At(in.nowNs / 1e9), x.across.At(in.nowNs / 1e9), stretch_.hand0[k]);
                }
                stretch_.twisting = false, stretch_.twistFrom = 0;
                ++stats.stretches;
                Log("both hands on screen %d (%s and %s): stretch and turn", sd.panel + 1, side_[0].byGrip ? "a fist" : "a pinch",
                    side_[1].byGrip ? "a fist" : "a pinch");
            }
            Carry c;
            c.panel = sd.panel, c.sides = 3, c.kind = Carry::Stretch;
            if (p_.stretchTurn) {
                const double v[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]}, n = std::sqrt(Dot(v, v));
                const double u[3] = {n > 1e-6 ? v[0] / n : 1, n > 1e-6 ? v[1] / n : 0, n > 1e-6 ? v[2] / n : 0};
                const double twist = p_.grabRotate ? StretchTwist(in, u) : 0;
                c.pose = StretchTurn(stretch_.start, stretch_.width0, stretch_.l0, stretch_.r0, l, r, twist, p_.stretchDead,
                                     p_.minWidth, p_.maxWidth, !P->floating, p_.grabRotate ? p_.rollSnap : 0, &c.width);
            } else {
                c.pose = StretchPose(stretch_.start, stretch_.width0, stretch_.l0, stretch_.r0, l, r, p_.stretchDead,
                                     p_.minWidth, p_.maxWidth, !P->floating, &c.width);
            }
            sd.last = o.last = c.pose;
            carries_.push_back(c);
            continue;
        }
        if (sd.width0 < 0 && sd.point.Ready()) sd.from = sd.point.At(in.nowNs / 1e9), sd.engaged = false;   // (below)
        if (!Carried(s, in, p)) continue;
        if (sd.width0 < 0) {
            // the other hand let go: carry on alone from where it is now (its dead zone afresh)
            sd.start = sd.last = P->pose, sd.width0 = P->width;
            ToLocal(P->pose, p, sd.grabLocal);
            std::copy(head, head + 3, sd.startHead);
            sd.haveRot0 = sd.turning = false;   // the wrist's turn from the hand as it is now
        }
        Carry c;
        c.panel = sd.panel, c.sides = 1 << s;
        if (sd.zone == Zone::Tab || sd.zone == Zone::Corner) {
            c.kind = Carry::Resize;
            const Hit n = Locate(*P, p);
            c.hx = n.u, c.hy = n.v, c.grabX = sd.grabX, c.grabY = sd.grabY, c.sx = sd.sx, c.sy = sd.sy;
            if (!P->floating) {
                // the corner follows the pinch along the diagonal, the opposite one staying put (vr.cpp's
                // laser resize likewise)
                double u, v;
                const double a = P->height / std::max(1e-6, P->width);
                c.width = ResizeFromCorner(P->width, a, sd.sx, sd.sy, n.u - sd.grabX, n.v - sd.grabY, p_.minWidth, p_.maxWidth, &u, &v);
                c.pose = SurfacePose(P->pose, P->curve, u, v);
            }
        } else if (sd.zone == Zone::Card) {
            // rigidly with the hand: its turn in the room since the grab, about the knuckles
            c.kind = Carry::Move;
            double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            RoomTurn(s, in, R);
            const double rel[3] = {sd.start.m[0][3] - sd.from.x, sd.start.m[1][3] - sd.from.y, sd.start.m[2][3] - sd.from.z};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j)
                    c.pose.m[i][j] = float(R[i][0] * sd.start.m[0][j] + R[i][1] * sd.start.m[1][j] + R[i][2] * sd.start.m[2][j]);
                c.pose.m[i][3] = float(p[i] + R[i][0] * rel[0] + R[i][1] * rel[1] + R[i][2] * rel[2]);
            }
            sd.last = c.pose;
        } else {
            c.kind = Carry::Move;
            const double *faces = in.haveHead ? head : sd.startHead;
            double turn[3][3];
            const bool turned = Turn(s, in, p, faces, turn);
            c.pose = CarryPose(sd.start, sd.startHead, sd.grabLocal, p, faces, sd.last, turned ? turn : nullptr,
                               turned ? p_.rollSnap : 0);
            sd.last = c.pose;
        }
        carries_.push_back(c);
    }
}

double HandInput::StretchTwist(const Input &in, const double axis[3]) {
    double sum = 0;
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        Side &x = side_[k];
        double H[3][3], Q[3][3], a[3];
        if (!stretch_.haveHand0[k] || !x.along.Ready() || !x.across.Ready() ||
            !HandFrame(x.along.At(in.nowNs / 1e9), x.across.At(in.nowNs / 1e9), H))
            continue;
        MulBT3(H, stretch_.hand0[k], Q);   // its turn in the room since the stretch began
        const double ang = AngleAxis(Q, a);
        sum += ang * Dot(a, axis), ++n;     // ...about the line between the hands
    }
    if (n < 2) return 0;
    const double mean = sum / 2, dead = p_.rotDeadzone * M_PI / 180;
    if (!stretch_.twisting) {
        if (std::fabs(mean) <= dead) return 0;
        stretch_.twisting = true, stretch_.twistFrom = mean > 0 ? dead : -dead;   // from there: no jump
    }
    return mean - stretch_.twistFrom;
}

bool HandInput::ThrowTarget(const double from[3], const double dir[3], Mat *to) const {
    if (!haveHead_) return false;
    const double head[3] = {lastHead_.m[0][3], lastHead_.m[1][3], lastHead_.m[2][3]};
    const double w[3] = {from[0] - head[0], from[1] - head[1], from[2] - head[2]};
    const double D = p_.throwDistance, b = Dot(w, dir), c = Dot(w, w) - D * D;
    if (c >= 0) return false;   // that far already
    const double t = -b + std::sqrt(b * b - c);
    const double at[3] = {from[0] + t * dir[0], from[1] + t * dir[1], from[2] + t * dir[2]};
    *to = Facing(at, head);
    return true;
}

void HandInput::EmitThrow(int s, int panel, const Mat &to, bool flick, bool push) {
    Event e;
    e.type = Event::Throw, e.side = s, e.panel = panel, e.to = to, e.ms = p_.throwMs, e.flick = flick, e.push = push;
    events_.push_back(e);
    ++(push ? stats.pushes : flick ? stats.flicks : stats.throws);
}

void HandInput::Push(int s, const Input &in, const std::vector<Panel> &panels) {
    Side &sd = side_[s];
    if (in.typing[s] || !haveHead_ || sd.resting || sd.hist.size() < 3) return;
    if (in.nowNs - sd.flickNs < int64_t(p_.flickRefractoryMs * 1e6) || in.nowNs - sd.busyNs < int64_t(p_.pushAfterMs * 1e6)) return;
    if (in.hands->pinch(s).down || in.hands->grip(s).down) return;
    const Side::Sample &now = sd.hist.back();
    if (now.open < p_.pushOpen || now.curl3 < p_.pushCurl) return;
    const double cosFacing = std::cos(p_.pushFacing * M_PI / 180), cosLoose = std::cos((p_.pushFacing + 10) * M_PI / 180);
    // the palm toward the panel: its normal (out of the palm) into the surface there
    auto facing = [&](const Panel &P, const Side::Sample &q, const Hit &t) {
        double n[3];
        SurfaceNormal(P, t.u, n);
        return -(q.normal[0] * n[0] + q.normal[1] * n[1] + q.normal[2] * n[2]);
    };
    for (const Panel &P : panels) {
        if (P.held) continue;
        const Hit t = Locate(P, now.palm);
        if (t.d > p_.pushDepth || t.d < -0.30 || !InFootprint(P, t, 0.02) || facing(P, now, t) < cosFacing) continue;
        // where it came from: the farthest out the palm was in the last second, at or in front of the
        // surface (a palm resting on it, then pushed, counts from there); open and facing it all the
        // way since (a poke that opens past the surface isn't a push), not across a gap in the tracking
        const Side::Sample *from = nullptr;
        double fromD = -1e9;
        for (size_t i = sd.hist.size(); i-- > 0;) {
            const Side::Sample &q = sd.hist[i];
            if (now.t - q.t > 1'000'000'000 || (i + 1 < sd.hist.size() && sd.hist[i + 1].t - q.t > 100'000'000)) break;
            const Hit qt = Locate(P, q.palm);
            if (q.open < p_.pushOpen - 0.1 || q.curl3 < p_.pushCurl - 0.1 || facing(P, q, qt) < cosLoose) break;
            if (qt.d > fromD) fromD = qt.d, from = &q;
        }
        if (!from || fromD < p_.pushFrom) continue;
        const double dt = (now.t - from->t) / 1e9, travel = fromD - t.d;
        if (dt <= 0) continue;
        // straight into it: the palm's way from there within pushAngle of the panel's normal (a hand
        // swept or waved through a panel crosses it at a slant)
        {
            double n[3], way[3];
            SurfaceNormal(P, t.u, n);
            for (int i = 0; i < 3; ++i) way[i] = double(now.palm[i]) - from->palm[i];
            const double wl = std::sqrt(Dot(way, way));
            if (wl < 1e-6 || -Dot(way, n) / wl < std::cos(p_.pushAngle * M_PI / 180)) continue;
        }
        // how fast into it now: a line through the palm's depth over the last flickFitMs
        int n = 0;
        double mt = 0, md = 0;
        for (const Side::Sample &q : sd.hist)
            if (now.t - q.t <= int64_t(p_.flickFitMs * 1e6) + 5'000'000) mt += (q.t - now.t) / 1e9, md += Locate(P, q.palm).d, ++n;
        double vin = 0;
        if (n >= 2) {
            mt /= n, md /= n;
            double tt = 0, td = 0;
            for (const Side::Sample &q : sd.hist)
                if (now.t - q.t <= int64_t(p_.flickFitMs * 1e6) + 5'000'000) {
                    const double x = (q.t - now.t) / 1e9 - mt;
                    tt += x * x, td += x * (Locate(P, q.palm).d - md);
                }
            if (tt > 1e-9) vin = -td / tt;
        }
        // ...or slowly, far into it: pushTravel past the surface, at pushSlow or more since it crossed it
        double crossed = 0;
        for (size_t i = sd.hist.size(); i-- > 0;)
            if (Locate(P, sd.hist[i].palm).d >= 0 || &sd.hist[i] == from) {
                crossed = (now.t - sd.hist[i].t) / 1e9;
                break;
            }
        const bool fast = vin >= p_.pushSpeed && travel >= p_.pushFastTravel;
        const bool far = -t.d >= p_.pushTravel && crossed > 0 && -t.d / crossed >= p_.pushSlow;
        if (!fast && !far) continue;
        const double eye[3] = {lastHead_.m[0][3], lastHead_.m[1][3], lastHead_.m[2][3]};
        const double c[3] = {P.pose.m[0][3], P.pose.m[1][3], P.pose.m[2][3]};
        double dir[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
        const double dl = std::sqrt(Dot(dir, dir));
        if (dl < 1e-3) continue;
        for (double &x : dir) x /= dl;
        Mat to;
        sd.flickNs = in.nowNs;
        if (!ThrowTarget(eye, dir, &to) || dl >= p_.throwDistance - 0.05) {
            Log("%s push at screen %d (%.2f m/s, %.0f cm in): it's %.1f m away already", SideName(s), P.index + 1, vin, travel * 100, dl);
            return;
        }
        EndAttempt(s);
        Log("%s push at screen %d: the palm %.0f cm in from %.0f cm out over %.2f s (%.2f m/s into it now), the hand %.2f open: "
            "thrown %.1f m out", SideName(s), P.index + 1, -t.d * 100, fromD * 100, dt, vin, now.open,
            p_.throwDistance);
        EmitThrow(s, P.index, to, true, true);
        return;
    }
}

void HandInput::Flick(int s, const Input &in, const std::vector<Panel> &panels, const Hand &h) {
    Side &sd = side_[s];
    if (in.typing[s] || !haveHead_ || in.nowNs - sd.flickNs < int64_t(p_.flickRefractoryMs * 1e6)) return;
    if (in.hands->pinch(s).down || in.hands->grip(s).down || sd.hist.size() < 4) return;
    const Side::Sample &now = sd.hist.back();
    if (now.open < p_.flickOpen) return;
    // the palm's velocity over the last flickFitMs (a least-squares line)
    int n = 0;
    double mt = 0, mp[3] = {0, 0, 0};
    for (const Side::Sample &q : sd.hist)   // (a gap in the tracking leaves too few samples here)
        if (now.t - q.t <= int64_t(p_.flickFitMs * 1e6) + 5'000'000)
            mt += (q.t - now.t) / 1e9, mp[0] += q.palm[0], mp[1] += q.palm[1], mp[2] += q.palm[2], ++n;
    if (n < 3) return;
    mt /= n;
    for (double &x : mp) x /= n;
    double tt = 0, v[3] = {0, 0, 0};
    for (const Side::Sample &q : sd.hist)
        if (now.t - q.t <= int64_t(p_.flickFitMs * 1e6) + 5'000'000) {
            const double dt = (q.t - now.t) / 1e9 - mt;
            tt += dt * dt;
            for (int i = 0; i < 3; ++i) v[i] += dt * (q.palm[i] - mp[i]);
        }
    if (tt < 1e-6) return;
    for (double &x : v) x /= tt;
    const double speed = std::sqrt(Dot(v, v));
    if (speed < p_.flickSpeed) return;
    // where it started: the oldest frame in flickMs with the palm at a panel (just in front of it, over
    // it) and nearly still there (a hand reaching through a panel passes it at speed: the recordings'
    // probe moves did, a flick starts from it)
    const double eye[3] = {lastHead_.m[0][3], lastHead_.m[1][3], lastHead_.m[2][3]};
    const Panel *P = nullptr;
    const Side::Sample *from = nullptr;
    // (not from before a gap in the tracking: the hand coming back elsewhere isn't a flick)
    size_t first = 1;
    for (size_t i = sd.hist.size() - 1; i >= 1; --i)
        if (sd.hist[i].t - sd.hist[i - 1].t > 100'000'000) {
            first = i + 1;
            break;
        }
    for (size_t i = first; i < sd.hist.size(); ++i) {
        const Side::Sample &q = sd.hist[i], &before = sd.hist[i - 1];
        if (now.t - q.t > int64_t(p_.flickMs * 1e6) || &q == &now || q.t <= before.t) continue;
        const double step[3] = {double(q.palm[0]) - before.palm[0], double(q.palm[1]) - before.palm[1], double(q.palm[2]) - before.palm[2]};
        if (std::sqrt(Dot(step, step)) / ((q.t - before.t) / 1e9) > p_.flickStartSpeed) continue;
        double key = 1e9;
        for (const Panel &c : panels) {
            if (c.held) continue;
            const Hit t = Locate(c, q.palm);
            if (t.d >= -p_.flickBack && t.d <= p_.flickNear && InFootprint(c, t, 0.05) && t.d < key) key = t.d, P = &c;
        }
        if (P) {
            from = &q;
            break;
        }
    }
    if (!P) return;
    const double moved[3] = {now.palm[0] - from->palm[0], now.palm[1] - from->palm[1], now.palm[2] - from->palm[2]};
    const double travel = std::sqrt(Dot(moved, moved));
    if (travel < p_.flickTravel) return;
    // the fingers opening on the way out (a flick), not a hand already open throughout (a reach)
    if (p_.flickOpening > 0 && now.open - from->open < p_.flickOpening) {
        double least = from->open;
        for (const Side::Sample &q : sd.hist)
            if (q.t >= from->t) least = std::min(least, double(q.open));
        if (now.open - least < p_.flickOpening) return;
    }
    // away from the eyes: along the line from them to the palm where it started
    double away[3] = {from->palm[0] - eye[0], from->palm[1] - eye[1], from->palm[2] - eye[2]};
    const double al = std::sqrt(Dot(away, away));
    if (al < 1e-3) return;
    for (double &x : away) x /= al;
    const double cosMin = std::cos(p_.flickAngle * M_PI / 180);
    const double off = std::acos(std::clamp(Dot(v, away) / speed, -1.0, 1.0)) * 180 / M_PI;
    if (Dot(v, away) / speed < cosMin || Dot(moved, away) / travel < cosMin) return;
    // the panel straight out: on the line from the eyes through its centre
    const double c[3] = {P->pose.m[0][3], P->pose.m[1][3], P->pose.m[2][3]};
    double dir[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
    const double dl = std::sqrt(Dot(dir, dir));
    if (dl < 1e-3) return;
    for (double &x : dir) x /= dl;
    Mat to;
    sd.flickNs = in.nowNs;
    if (!ThrowTarget(eye, dir, &to) || dl >= p_.throwDistance - 0.05) {
        Log("%s flick at screen %d (%.2f m/s, %.0f cm): it's %.1f m away already", SideName(s), P->index + 1, speed, travel * 100, dl);
        return;
    }
    EndAttempt(s);
    Log("%s flick at screen %d: %.2f m/s, %.0f cm, %.0f deg off straight out, the hand %.2f open (%.2f at the start): thrown "
        "%.1f m out", SideName(s), P->index + 1, speed, travel * 100, off, now.open, from->open, p_.throwDistance);
    EmitThrow(s, P->index, to, true);
}

void HandInput::Glows(const Input &in, const std::vector<Panel> &panels) {
    // white: the fingertip; blue: a pinch here moves the panel; amber: it resizes it; green: it
    // takes it like a card. A ring until it holds, then a dot.
    static const float kWhite[3] = {0.92f, 0.96f, 1.0f}, kBlue[3] = {90 / 255.f, 170 / 255.f, 1.0f},
                       kAmber[3] = {1.0f, 0.72f, 0.25f}, kGreen[3] = {0.45f, 1.0f, 0.6f};
    throwAims_.clear();
    // an open hand's palm at a panel, toward it: a flick or a push would throw it (vr.cpp lights it)
    for (int s = 0; s < 2 && (p_.push || p_.flick); ++s) {
        const Side &sd = side_[s];
        if ((sd.state != State::Idle && sd.state != State::Hover) || in.typing[s] || sd.resting) continue;
        if (in.hands->pinch(s).down || in.hands->grip(s).down) continue;
        const Hand *h = HandFor(s, in.hands->shown(), sd.state == State::Idle ? 0 : sd.handId);
        if (!h || Openness(*h) < p_.pushOpen || Curl3(*h) < p_.pushCurl) continue;
        double palm[3], out[3], n[3];
        h->Palm(palm);
        PalmOut(*h, out);
        for (const Panel &P : panels) {
            if (P.held) continue;
            const Hit t = Locate(P, palm);
            if (t.d < -p_.flickBack || t.d > p_.flickNear || !InFootprint(P, t, 0.02)) continue;
            SurfaceNormal(P, t.u, n);
            if (-Dot(out, n) < std::cos(p_.pushFacing * M_PI / 180)) continue;
            const double k = std::clamp(1 - std::max(0.0, t.d) / p_.flickNear, 0.3, 1.0);
            double &a = throwAims_[P.index];
            a = std::max(a, k);
        }
    }
    for (int s = 0; s < 2; ++s) {
        const Side &sd = side_[s];
        if (sd.state != State::Hover && sd.state != State::Press && sd.state != State::Grab) continue;
        const Panel *P = Find(panels, sd.panel);
        if (!P) continue;
        nearPanels_.insert(P->index);
        const Hand *h = HandFor(s, in.hands->shown(), sd.handId);
        const double k = PxPerM(*P);
        auto glow = [&](double u, double v, double rm, double alpha, bool ring, const float rgb[3]) {
            double x, y;
            if (!P->glows || alpha <= 0.01) return;
            u = std::clamp(u, -P->width / 2 + 0.004, P->width / 2 - 0.004), v = std::clamp(v, -P->height / 2 + 0.004, P->height / 2 - 0.004);
            if (!ToBuffer(*P, u, v, &x, &y)) return;
            glows_[P->index].push_back({float(x), float(y), float(rm * k), float(alpha), ring, {rgb[0], rgb[1], rgb[2]}});
        };
        // the fingertip: a ring that tightens and brightens as it comes to the surface, a dot
        // while pressed
        if (sd.state == State::Press && sd.control == Control::None) {
            if (P->glows)   // where it's pressed (or dragged to), in pixels
                glows_[P->index].push_back({float(sd.lastX), float(sd.lastY), float(0.005 * k), 0.9f, false,
                                            {kWhite[0], kWhite[1], kWhite[2]}});
        } else if (sd.state == State::Hover && h && Extension(*h) >= p_.extended) {
            const Hit t = Locate(*P, h->p[8]);
            double x, y;
            if (t.d > p_.nearBack && t.d <= p_.nearFront && ToBuffer(*P, t.u, t.v, &x, &y)) {
                const double f = std::clamp(t.d / p_.nearFront, 0.0, 1.0);
                glow(t.u, t.v, 0.004 + 0.008 * f, 0.8 * (1 - f), true, kWhite);
            }
        }
        if (!h || sd.state == State::Press) continue;
        // what a pinch (or the fist) does here: at the corner it resizes, on the border it moves,
        // either side of the edge it carries like a card
        const handcut::Gesture &g = in.hands->pinch(s);
        const double strength = g.tracked ? g.strength : std::clamp((0.06 - h->pinchD[0]) / 0.04, 0.0, 1.0);
        if (sd.state == State::Grab) {
            const float *col = sd.zone == Zone::Corner || sd.zone == Zone::Tab ? kAmber : sd.zone == Zone::Card ? kGreen : kBlue;
            double pt[3];
            if (sd.byGrip) h->Palm(pt);
            else Copy3(h->contact, pt);
            const Hit q = Locate(*P, pt);
            if (sd.zone == Zone::Corner || sd.zone == Zone::Tab) corners_[P->index] = {sd.sx, sd.sy, 1.0, true};
            if (sd.zone == Zone::Corner) glow(sd.sx * P->width / 2, sd.sy * P->height / 2, 0.014, 0.9, false, col);
            else glow(q.u, q.v, sd.byGrip ? 0.02 : 0.012, 0.9, false, col);
            if (sd.zone == Zone::Bar) hovered_[P->index] |= 1u << int(Control::Bar);
            if (sd.zone == Zone::Tab) hovered_[P->index] |= 1u << int(Control::Tab);
            continue;
        }
        const Hit q = Locate(*P, h->contact);
        if (p_.cardPinch && std::fabs(q.d) <= p_.bandDepth && std::fabs(FromBorder(*P, q.u, q.v)) <= p_.cardBand &&
            Straddled(s, *P, h->captureNs) && CardFingers(*h)) {
            glow(q.u, q.v, 0.010 + 0.008 * strength, 0.85, true, kGreen);
            continue;
        }
        int sx, sy;
        double excess;
        const Zone z = GrabZone(*P, q, p_, &sx, &sy, &excess);
        if (excess > p_.glowReach) continue;
        if (z == Zone::Bar) hovered_[P->index] |= 1u << int(Control::Bar);
        if (z == Zone::Tab) hovered_[P->index] |= 1u << int(Control::Tab);
        const double alpha = 0.35 + 0.55 * (1 - std::clamp(excess / p_.glowReach, 0.0, 1.0));
        if (z == Zone::Corner || z == Zone::Tab) {   // the resize handle there, brighter the closer
            const double close = 1 - std::clamp(excess / p_.glowReach, 0.0, 1.0);
            auto it = corners_.find(P->index);
            if (it == corners_.end() || (!it->second.held && it->second.closeness < close))
                corners_[P->index] = {sx, sy, close, false};
        }
        if (z == Zone::Corner) glow(sx * P->width / 2, sy * P->height / 2, 0.012 + 0.008 * strength, alpha, true, kAmber);
        else if (z == Zone::Edge || z == Zone::Title) glow(q.u, q.v, 0.010 + 0.008 * strength, alpha, true, kBlue);
    }
}

std::string HandInput::Describe() const {
    static const char *names[] = {"off", "idle", "hover", "press", "grab"};
    std::string out;
    for (int s = 0; s < 2; ++s) {
        const Side &sd = side_[s];
        char b[96];
        std::snprintf(b, sizeof b, "%s%s %s", s ? ", " : "", s ? "right" : "left", names[int(sd.state)]);
        out += b;
        if (sd.panel >= 0 && sd.state != State::Idle && sd.state != State::Off) out += " " + std::to_string(sd.panel + 1);
        if (sd.state == State::Grab && (sd.zone == Zone::Card || sd.zone == Zone::Fist)) out += sd.zone == Zone::Card ? " (card)" : " (fist)";
        if (sd.state == State::Press && sd.byPinch) out += " (pinch)";
        if (sd.fistWait || sd.pinchWait) out += sd.fistWait ? " (fist waits)" : " (pinch waits)";
    }
    return out;
}

}  // namespace handinput
