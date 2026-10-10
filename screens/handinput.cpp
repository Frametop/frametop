// Near hand input on Frametop's panels (see handinput.h).
#include "handinput.h"

#include <algorithm>
#include <cmath>
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

// The index's last bone (DIP to tip) points into the panel within maxDeg of its normal there.
bool Along(const Panel &p, const Hit &at, const Hand &h, double maxDeg) {
    const double d[3] = {double(h.p[8][0]) - h.p[7][0], double(h.p[8][1]) - h.p[7][1], double(h.p[8][2]) - h.p[7][2]};
    double l[3];   // in the panel's frame
    for (int i = 0; i < 3; ++i) l[i] = p.pose.m[0][i] * d[0] + p.pose.m[1][i] * d[1] + p.pose.m[2][i] * d[2];
    // the surface's normal (out of its front): +z, or on a cylinder the way OnSurface turns it
    double n[3] = {0, 0, 1};
    if (p.curve > 0) n[0] = -std::sin(at.u / p.curve), n[2] = std::cos(at.u / p.curve);
    const double len = std::sqrt(Dot(l, l));
    return len > 1e-6 && -Dot(l, n) / len >= std::cos(maxDeg * M_PI / 180);
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

Zone GrabZone(const Panel &p, const Hit &h, const Params &k, int *sx, int *sy, double *excess) {
    const double W = p.width / 2, H = p.height / 2, depth = std::fabs(h.d) - k.zoneDepth;
    struct Cand { Zone z; double e; int sx, sy; };
    std::vector<Cand> c;   // in priority order
    for (const Spot &s : p.controls)
        if (s.kind == Control::Tab)
            c.push_back({Zone::Tab, std::sqrt((h.u - s.u) * (h.u - s.u) + (h.v - s.v) * (h.v - s.v) + h.d * h.d) - k.cornerReach, 1, -1});
    for (int cx : {1, -1})
        for (int cy : {-1, 1}) {
            if (p.floating && !(cx == 1 && cy == -1)) continue;   // a window resizes from its bottom right only
            const double du = h.u - cx * W, dv = h.v - cy * H;
            c.push_back({Zone::Corner, std::sqrt(du * du + dv * dv + h.d * h.d) - k.cornerReach, cx, cy});
        }
    for (const Spot &s : p.controls)
        if (s.kind == Control::Bar) {
            const double du = std::max(0.0, std::fabs(h.u - s.u) - s.hw), dv = h.v - s.v;
            c.push_back({Zone::Bar, std::max(std::sqrt(du * du + dv * dv) - k.barReach, depth), 0, 0});
        }
    if (p.floating && p.titleH > 0 && p.cropH > 0) {
        const double band = p.titleH * p.height / p.cropH;
        c.push_back({Zone::Title, std::max({Outside(h.u, -W, W), Outside(h.v, H - band, H), depth}), 0, 0});
    }
    c.push_back({Zone::Edge, std::max({Outside(h.u, -W - k.edgeOut, -W + k.edgeIn), Outside(h.v, -H, H), depth}), 0, 0});
    c.push_back({Zone::Edge, std::max({Outside(h.u, W - k.edgeIn, W + k.edgeOut), Outside(h.v, -H, H), depth}), 0, 0});
    c.push_back({Zone::Edge, std::max({Outside(h.u, -W, W), Outside(h.v, H - k.edgeIn, H + k.edgeOut), depth}), 0, 0});
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

Mat CarryPose(const Mat &start, const double startHead[3], const double grabLocal[3], const double pinch[3],
              const double head[3], const Mat &previous) {
    // the turn from facing the head the panel had at the start
    const double c0[3] = {start.m[0][3], start.m[1][3], start.m[2][3]};
    double off[3][3];
    MulTA(Facing(c0, startHead), start, off);
    Mat r = previous;
    for (int it = 0; it < 3; ++it) {
        double c[3];
        for (int i = 0; i < 3; ++i)
            c[i] = pinch[i] - (r.m[i][0] * grabLocal[0] + r.m[i][1] * grabLocal[1] + r.m[i][2] * grabLocal[2]);
        MulRot(Facing(c, head), off, &r);
        for (int i = 0; i < 3; ++i) r.m[i][3] = float(c[i]);
    }
    // the centre for the final turn
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = float(pinch[i] - (r.m[i][0] * grabLocal[0] + r.m[i][1] * grabLocal[1] + r.m[i][2] * grabLocal[2]));
    return r;
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

// ----------------------------------------------------------------------------- the machine

namespace {
int64_t g_now = 0;   // Update's time, for the helpers below
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

void HandInput::EndPress(int s, bool lost) {
    Side &sd = side_[s];
    if (sd.state != State::Press) return;
    if (sd.control == Control::None) {
        Emit(Event::Release, s, sd.panel, sd.lastX, sd.lastY, lost);
        ++stats.releases, stats.lostReleases += lost;
    }
    sd.state = State::Hover, sd.releasedAt = g_now, sd.below = 0, sd.control = Control::None;
}

void HandInput::EndGrab(int s, bool lost) {
    Side &sd = side_[s];
    if (sd.state != State::Grab) return;
    Emit(Event::GrabEnd, s, sd.panel, 0, 0, lost);
    ++stats.grabEnds;
    const int other = 1 - s;
    if (stretch_.panel == sd.panel) {
        // two hands to one: the other carries on alone, from where the panel is now
        stretch_.panel = -1;
        Side &o = side_[other];
        if (o.state == State::Grab && o.panel == sd.panel) o.zone = Zone::Edge, o.width0 = -1;   // re-seeded in Carries
    }
    sd.state = State::Hover, sd.below = 0;
}

void HandInput::ForgetPress(int s) {
    Side &sd = side_[s];
    if (sd.state == State::Press) sd.state = State::Hover, sd.releasedAt = g_now, sd.control = Control::None;
}

void HandInput::Update(const Input &in, const std::vector<Panel> &panels) {
    g_now = in.nowNs;
    events_.clear(), carries_.clear(), glows_.clear(), hovered_.clear(), nearPanels_.clear();
    const handcut::Hands &H = *in.hands;
    newFrame_ = H.frame() != frame_;
    frame_ = H.frame();
    if (newFrame_) frameNs_ = H.captureNs(), ++stats.frames;
    if (in.haveHead) {
        head_.Set({p_.grabHeadMinCutoff, p_.grabHeadBeta, 1.0});
        const double t = in.nowNs / 1e9;
        head_.Sample({in.head.m[0][3], in.head.m[1][3], in.head.m[2][3]}, t, t);
    }
    for (int s = 0; s < 2; ++s) {
        Side &sd = side_[s];
        if (!in.allowed[s]) {
            EndPress(s, true), EndGrab(s, true);
            sd.state = State::Off, sd.panel = -1;
            sd.knowBegins = false;
            continue;
        }
        if (sd.state == State::Off) sd.state = State::Idle;
        const Hand *h = HandFor(s, H.seen(), sd.state == State::Idle ? 0 : sd.handId);
        if (h) sd.lastSeen = in.nowNs, sd.handId = h->id;
        if ((sd.state == State::Press || sd.state == State::Grab) && !Find(panels, sd.panel)) {   // hidden, gone
            EndPress(s, true), EndGrab(s, true);
            sd.state = State::Idle, sd.panel = -1;
        }
        if (!h && sd.state != State::Idle && in.nowNs - sd.lastSeen > int64_t(p_.lostMs * 1e6)) {
            EndPress(s, true), EndGrab(s, true);
            sd.state = State::Idle, sd.panel = -1;
        }
        Grab(s, in, panels);
        if (newFrame_ && h && sd.state != State::Grab) Poke(s, in, panels, h);
    }
    if (newFrame_ && anyNear()) ++stats.nearFrames;
    Carries(in, panels);
    Glows(in, panels);
}

void HandInput::Poke(int s, const Input &in, const std::vector<Panel> &panels, const Hand *h) {
    Side &sd = side_[s];
    // the panel it's near: the index tip or the pinch point over it, close in front
    int best = -1;
    double key = 1e9;
    if (sd.state == State::Press) {
        best = sd.panel;
    } else {
        const double margin = std::max(p_.nearMargin, p_.edgeOut + 0.005);
        for (const Panel &P : panels) {
            for (const float *pt : {h->p[8], h->pinchPoint}) {
                const Hit t = Locate(P, pt);
                if (t.d > p_.nearBack && t.d <= p_.nearFront && InFootprint(P, t, margin) && std::fabs(t.d) < key)
                    key = std::fabs(t.d), best = P.index;
            }
        }
    }
    if (best >= 0) {
        if (sd.state == State::Idle || (sd.state == State::Hover && sd.panel != best)) {
            sd.below = 0, sd.recent.clear(), sd.hoverX = sd.hoverY = -1;
            sd.spent = false;
        }
        if (sd.state == State::Idle) sd.state = State::Hover;
        sd.panel = best, sd.outSince = -1;
    } else if (sd.state == State::Hover) {
        if (sd.outSince < 0) sd.outSince = in.nowNs;
        if (in.nowNs - sd.outSince > int64_t(p_.leaveMs * 1e6)) sd.state = State::Idle, sd.panel = -1;
    }
    if (sd.state != State::Hover && sd.state != State::Press) return;
    const Panel *P = Find(panels, sd.panel);
    if (!P) {
        sd.state = State::Idle, sd.panel = -1;
        return;
    }
    const Hit t = Locate(*P, h->p[8]);
    sd.recent.push_back({h->captureNs, t.d});
    while (sd.recent.size() > 2 && h->captureNs - sd.recent.front().t > 250'000'000) sd.recent.erase(sd.recent.begin());
    const bool extended = Extension(*h) >= p_.extended;
    double others = 1e9;
    for (int j : {12, 16, 20}) others = std::min(others, Locate(*P, h->p[j]).d);
    double straightest = 0;   // of the other fingers
    for (int f : {9, 13, 17}) straightest = std::max(straightest, FingerExtension(*h, f));
    const bool nearest = t.d + p_.nearest <= others || Extension(*h) - straightest >= p_.pointing;
    double x = 0, y = 0;
    const bool over = ToBuffer(*P, t.u, t.v, &x, &y);
    const Control ctl = over ? Control::None : ControlAt(*P, t.u, t.v, 0.01);
    const bool otherPressing = side_[1 - s].state == State::Press;

    if (sd.state == State::Press) {
        sd.deepest = std::min(sd.deepest, t.d);
        if (t.d < p_.tooDeep) {   // reaching through the panel isn't a drag
            ++stats.deepReleases;
            EndPress(s, false);
            sd.spent = true;
            return;
        }
        if (t.d >= p_.release || t.d - sd.deepest >= p_.pullBack) {
            EndPress(s, false);
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
    // hovering
    const bool resting = sd.resting = Resting(*h, in, p_, sd.resting);
    if (sd.spent && t.d >= 0) sd.spent = false;
    sd.below = t.d <= 0 ? sd.below + 1 : 0;
    // coming in: when it crossed the surface, it came toward it at approach or faster from some
    // point in the last approachMs (or from the frame before, at a low rate)
    if (sd.below == 1) {
        sd.fastIn = false;
        const Side::Sample &now = sd.recent.back();
        for (size_t i = 0; i + 1 < sd.recent.size(); ++i) {
            const Side::Sample &q = sd.recent[i];
            const double dt = (now.t - q.t) / 1e9;
            if ((dt * 1e3 <= p_.approachMs + 20 || i + 2 == sd.recent.size()) && dt > 1e-3 &&
                (q.d - now.d) / dt >= p_.approach)
                sd.fastIn = true;
        }
    }
    if (over && extended && !resting && !in.typing[s] && t.d <= p_.cursor && t.d > p_.nearBack && !in.pointerBusy && !otherPressing &&
        (std::fabs(x - sd.hoverX) >= 0.5 || std::fabs(y - sd.hoverY) >= 0.5)) {
        sd.hoverX = x, sd.hoverY = y;
        Emit(Event::Motion, s, P->index, x, y);
    }
    const bool button = ctl == Control::Curve || ctl == Control::Reset || ctl == Control::Dock || ctl == Control::Close;
    if (sd.below < p_.pressFrames || !(over || button)) return;
    const bool fast = sd.fastIn;
    const bool rePress = sd.releasedAt >= 0 && in.nowNs - sd.releasedAt < int64_t(p_.rePressMs * 1e6);
    const handcut::Gesture &pinch = in.hands->pinch(s), &grip = in.hands->grip(s);
    if (!h->stereo) return (void)++stats.heldNotStereo;
    if (!extended) return (void)++stats.heldNotExtended;
    if (!nearest) return (void)++stats.heldNotNearest;
    if (pinch.down || grip.down) return (void)++stats.heldPinching;
    if (in.pointerBusy || otherPressing || rePress || sd.spent) return (void)++stats.heldBusy;
    if (resting) return (void)++stats.heldResting;
    if (in.typing[s]) return (void)++stats.heldTyping;
    if (!Along(*P, t, *h, p_.fingerAngle)) return (void)++stats.heldAskew;
    if (!fast) return (void)++stats.heldSlow;
    // pressed
    sd.state = State::Press, sd.lockU = t.u, sd.lockV = t.v, sd.deepest = t.d, sd.dragging = false;
    if (over) {
        sd.control = Control::None, sd.lastX = x, sd.lastY = y;
        Emit(Event::Press, s, P->index, x, y);
        ++stats.presses;
    } else {
        sd.control = ctl;
        Event e;
        e.type = Event::Control, e.side = s, e.panel = P->index, e.control = ctl;
        events_.push_back(e);
        ++stats.controls;
    }
}

void HandInput::Grab(int s, const Input &in, const std::vector<Panel> &panels) {
    Side &sd = side_[s];
    const handcut::Gesture &g = in.hands->pinch(s);
    if (!sd.knowBegins) sd.begins = g.begins, sd.knowBegins = true;   // not a pinch that began before
    if (sd.state == State::Grab) {
        if (!g.down || g.begins != sd.begins) EndGrab(s, g.lost);
        if (g.begins != sd.begins) sd.begins = g.begins;
        return;
    }
    if (g.begins == sd.begins) return;
    sd.begins = g.begins;
    // a new pinch: the near claim had this hand, and it began in a grab zone (decided once,
    // at the pinch's begin; on the content a near pinch does nothing)
    if (!g.down || sd.state != State::Hover || in.typing[s]) return;
    const Panel *bestP = nullptr;
    Zone zone = Zone::None;
    int sx = 1, sy = -1;
    double key = 1e9;
    for (const Panel &P : panels) {
        if (P.held) continue;
        const Hit b = Locate(P, g.beginPoint);
        int x, y;
        double e;
        const Zone z = GrabZone(P, b, p_, &x, &y, &e);
        if (e <= 0 && std::fabs(b.d) < key) key = std::fabs(b.d), bestP = &P, zone = z, sx = x, sy = y;
    }
    if (!bestP) return;
    // the pinch point as the cameras saw it (the smoothing starts afresh from it: Carried)
    const Hand *h = HandFor(s, in.hands->seen(), g.handId ? g.handId : sd.handId);
    double pinch[3];
    if (h) Copy3(h->pinchPoint, pinch);
    else Copy3(g.point, pinch);
    sd.state = State::Grab, sd.panel = bestP->index, sd.zone = zone, sd.sx = sx, sd.sy = sy;
    sd.start = sd.last = bestP->pose, sd.width0 = bestP->width;
    sd.point.Set({p_.grabMinCutoff, p_.grabBeta, 1.0});
    sd.point.Reset();
    sd.point.Sample({pinch[0], pinch[1], pinch[2]}, in.hands->captureNs() / 1e9, in.nowNs / 1e9);
    sd.from = {pinch[0], pinch[1], pinch[2]}, sd.shift = {}, sd.engaged = false;
    ToLocal(bestP->pose, pinch, sd.grabLocal);
    if (in.haveHead) {
        const fhsmooth::V3 hd = head_.Last();
        sd.startHead[0] = hd.x, sd.startHead[1] = hd.y, sd.startHead[2] = hd.z;
    } else {
        std::copy(pinch, pinch + 3, sd.startHead), sd.startHead[2] += 1;
    }
    const Hit n = Locate(*bestP, pinch);
    sd.grabX = n.u - sx * bestP->width / 2, sd.grabY = n.v - sy * bestP->height / 2;
    Event e;
    e.type = Event::GrabStart, e.side = s, e.panel = bestP->index, e.zone = zone;
    events_.push_back(e);
    ++stats.grabs;
}

bool HandInput::Carried(int s, const Input &in, double out[3], bool deadzone) {
    Side &sd = side_[s];
    if (!sd.point.Ready()) return false;
    fhsmooth::V3 p = sd.point.At(in.nowNs / 1e9);
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
    if (newFrame_)
        for (int s = 0; s < 2; ++s) {
            Side &sd = side_[s];
            const Hand *h = sd.state == State::Grab ? HandFor(s, in.hands->seen(), sd.handId) : nullptr;
            if (!h) continue;   // briefly unseen: the panel stays where the smoothing has it
            sd.point.Set({p_.grabMinCutoff, p_.grabBeta, 1.0});
            sd.point.Sample({h->pinchPoint[0], h->pinchPoint[1], h->pinchPoint[2]}, in.hands->captureNs() / 1e9, in.nowNs / 1e9);
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
                ++stats.stretches;
            }
            Carry c;
            c.panel = sd.panel, c.sides = 3, c.kind = Carry::Stretch;
            c.pose = StretchPose(stretch_.start, stretch_.width0, stretch_.l0, stretch_.r0, l, r, p_.stretchDead,
                                 p_.minWidth, p_.maxWidth, !P->floating, &c.width);
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
        }
        Carry c;
        c.panel = sd.panel, c.sides = 1 << s;
        if (sd.zone == Zone::Tab || sd.zone == Zone::Corner) {
            c.kind = Carry::Resize;
            const Hit n = Locate(*P, p);
            c.hx = n.u, c.hy = n.v, c.grabX = sd.grabX, c.grabY = sd.grabY, c.sx = sd.sx, c.sy = sd.sy;
            if (!P->floating) {
                // the corner follows the pinch along the diagonal (vr.cpp's laser resize)
                const double a = P->height / std::max(1e-6, P->width), cx = n.u - sd.grabX, cy = n.v - sd.grabY;
                c.width = std::clamp(2 * (sd.sx * cx + sd.sy * a * cy) / (1 + a * a), p_.minWidth, p_.maxWidth);
            }
        } else {
            c.kind = Carry::Move;
            c.pose = CarryPose(sd.start, sd.startHead, sd.grabLocal, p, in.haveHead ? head : sd.startHead, sd.last);
            sd.last = c.pose;
        }
        carries_.push_back(c);
    }
}

void HandInput::Glows(const Input &in, const std::vector<Panel> &panels) {
    static const float kWhite[3] = {0.92f, 0.96f, 1.0f}, kBlue[3] = {90 / 255.f, 170 / 255.f, 1.0f};
    for (int s = 0; s < 2; ++s) {
        const Side &sd = side_[s];
        if (sd.state != State::Hover && sd.state != State::Press && sd.state != State::Grab) continue;
        const Panel *P = Find(panels, sd.panel);
        if (!P) continue;
        nearPanels_.insert(P->index);
        const Hand *h = HandFor(s, in.hands->shown(), sd.handId);
        const double k = PxPerM(*P);
        auto glow = [&](double x, double y, double rm, double alpha, bool ring, const float rgb[3]) {
            if (!P->glows || alpha <= 0.01) return;
            glows_[P->index].push_back({float(x), float(y), float(rm * k), float(alpha), ring, {rgb[0], rgb[1], rgb[2]}});
        };
        // the fingertip: a ring that tightens and brightens as it comes to the surface, a dot
        // while pressed
        if (sd.state == State::Press && sd.control == Control::None) {
            glow(sd.lastX, sd.lastY, 0.005, 0.9, false, kWhite);
        } else if (sd.state == State::Hover && h && Extension(*h) >= p_.extended) {
            const Hit t = Locate(*P, h->p[8]);
            double x, y;
            if (t.d > p_.nearBack && t.d <= p_.nearFront && ToBuffer(*P, t.u, t.v, &x, &y)) {
                const double f = std::clamp(t.d / p_.nearFront, 0.0, 1.0);
                glow(x, y, 0.004 + 0.008 * f, 0.8 * (1 - f), true, kWhite);
            }
        }
        // the pinch point near a grab zone: bigger as the pinch closes, blue while it holds
        if (!h) continue;
        const handcut::Gesture &g = in.hands->pinch(s);
        double strength = g.tracked ? g.strength : std::clamp((0.06 - h->pinchD[0]) / 0.04, 0.0, 1.0);
        const Hit q = Locate(*P, h->pinchPoint);
        int sx, sy;
        double excess;
        const Zone z = sd.state == State::Grab ? sd.zone : GrabZone(*P, q, p_, &sx, &sy, &excess);
        if (sd.state != State::Grab && excess > p_.glowReach) continue;
        if (z == Zone::Bar) hovered_[P->index] |= 1u << int(Control::Bar);
        if (z == Zone::Tab) hovered_[P->index] |= 1u << int(Control::Tab);
        double x, y;
        const double u = std::clamp(q.u, -P->width / 2, P->width / 2), v = std::clamp(q.v, -P->height / 2, P->height / 2);
        if (std::hypot(u - q.u, v - q.v) > 0.01 || !ToBuffer(*P, u, v, &x, &y)) continue;   // off the panel (the bar)
        if (sd.state == State::Grab) glow(x, y, 0.006 + 0.008 * strength, 0.9, false, kBlue);
        else glow(x, y, 0.006 + 0.008 * strength, 0.3 + 0.5 * (1 - std::clamp(excess / p_.glowReach, 0.0, 1.0)), true, kWhite);
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
    }
    return out;
}

}  // namespace handinput
