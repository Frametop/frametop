// The drawn hands between, across and past ft-hands' frames (ft-handview). The tracking comes about
// 30 times a second, so a hand drawn as each frame comes steps along; and when the tracking loses a
// hand, or misreads one for a frame, the drawn hand vanished or jumped about. User, 2026-10-13:
// "implement interpolation for the rendered hands so that they feel smoother", and "if the hand is
// lost it's preferable the rendered hand display freeze instead of make impossible movements, spaz
// out, or vanish until a few seconds have passed". Per side, so:
// - Interpolation: a new frame blends in from the pose drawn now over about the time since the frame
//   before (Params::blendMinMs to blendMaxMs), joint by joint, and is drawn in between.
// - Freezing: a hand that stops being tracked stays where it was last drawn for holdS, then goes.
// - Impossible movements: a frame whose palm is further from the last one taken than the hand could have
//   moved (jumpM, or jumpSpeed m/s over the time since), or turned over (flipDeg) in one frame, isn't
//   drawn until confirmFrames frames in a row agree with each other by the same test (or confirmMaxS
//   passes); the drawn hand holds still meanwhile, then goes straight there. So does a hand coming back
//   after a loss, and one first seen.
#pragma once

#include "handcut.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace handhold {

struct Params {
    double holdS = 3.0;           // a lost hand stays, frozen, this long
    double blendMinMs = 15, blendMaxMs = 70;
    // the palm's farthest plausible move: jumpM, or jumpSpeed m/s (the lazy susan's strokes peak at 4.5)
    double jumpM = 0.15, jumpSpeed = 6.0;
    double flipDeg = 100;         // the palm turning more than this within 0.2 s is a misreading too
    int confirmFrames = 2;
    double confirmMaxS = 0.25;    // frames that never agree are taken after this
    double snapM = 0.05;          // a confirmed jump or a return further than this goes straight there
};

inline void Normal(const handcut::Hand &h, double n[3]) {   // the palm's normal (its sign per side)
    double a[3], b[3];
    for (int i = 0; i < 3; ++i) a[i] = h.p[5][i] - h.p[0][i], b[i] = h.p[17][i] - h.p[0][i];
    n[0] = a[1] * b[2] - a[2] * b[1], n[1] = a[2] * b[0] - a[0] * b[2], n[2] = a[0] * b[1] - a[1] * b[0];
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-12;
    for (int i = 0; i < 3; ++i) n[i] /= l;
}

inline double PalmDistance(const handcut::Hand &a, const handcut::Hand &b) {
    double pa[3], pb[3];
    a.Palm(pa), b.Palm(pb);
    return std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
}

// Could the hand have gone from a to b in gapS seconds?
inline bool Plausible(const handcut::Hand &a, const handcut::Hand &b, double gapS, const Params &k) {
    if (PalmDistance(a, b) > std::max(k.jumpM, k.jumpSpeed * gapS)) return false;
    if (gapS > 0.2) return true;
    double na[3], nb[3];
    Normal(a, na), Normal(b, nb);
    const double c = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
    return c >= std::cos(k.flipDeg * M_PI / 180);
}

// a + (b - a) t, joint by joint (the side, the ids and the flags are b's)
inline handcut::Hand Lerp(const handcut::Hand &a, const handcut::Hand &b, double t) {
    handcut::Hand o = b;
    const float u = float(std::clamp(t, 0.0, 1.0));
    auto mix = [u](const float *x, const float *y, float *out, int n) {
        for (int i = 0; i < n; ++i) out[i] = x[i] + (y[i] - x[i]) * u;
    };
    for (int j = 0; j < 21; ++j) mix(a.p[j], b.p[j], o.p[j], 3);
    mix(a.radius, b.radius, o.radius, 21);
    mix(a.bone, b.bone, o.bone, 20);
    mix(a.elbow, b.elbow, o.elbow, 3);
    mix(a.pinchPoint, b.pinchPoint, o.pinchPoint, 3);
    mix(a.contact, b.contact, o.contact, 3);
    return o;
}

class Side {
public:
    // Once per tick: the tracked hand now (null: not tracked), the hands frame counter (handcut::Hands
    // ::frame()), now (CLOCK_MONOTONIC ns). The pose to draw, or null: nothing to draw.
    const handcut::Hand *Update(const handcut::Hand *hand, uint64_t frame, int64_t nowNs, const Params &k) {
        const bool newFrame = frame != frame_;
        frame_ = frame;
        if (!hand) {
            pending_ = 0;
            if (!have_) return nullptr;
            if (!lostNs_) lostNs_ = nowNs, ++holds;
            if (nowNs - lostNs_ > int64_t(k.holdS * 1e9)) return have_ = false, nullptr;
            return &shown_;
        }
        if (!have_) {   // first seen, or back after the hold: no pose to blend from
            if (!Confirmed(*hand, newFrame, nowNs, k)) return nullptr;
            Accept(*hand, nowNs, true, k);
        } else if (newFrame) {
            const bool back = lostNs_ != 0;
            if (!back && Plausible(last_, *hand, (nowNs - acceptNs_) / 1e9, k)) {
                Accept(*hand, nowNs, false, k);   // (a misread frame before it is let go)
            } else {
                if (!back && !pending_) ++jumps;
                if (!Confirmed(*hand, true, nowNs, k)) return &shown_;   // held still meanwhile
                Accept(*hand, nowNs, PalmDistance(shown_, *hand) > k.snapM, k);
            }
        } else if (pending_) {
            return &shown_;
        }
        const double t = blendMs_ > 0 ? (nowNs - blendNs_) / 1e6 / blendMs_ : 1;
        shown_ = t >= 1 ? *hand : Lerp(from_, *hand, t);
        return &shown_;
    }
    // Drop the hand now (Room View, a controller in it, no head pose).
    void Reset() { have_ = false, pending_ = 0, lostNs_ = 0; }
    bool frozen() const { return have_ && (lostNs_ != 0 || pending_ > 0); }
    bool blending(int64_t nowNs) const { return have_ && !frozen() && nowNs - blendNs_ < int64_t(blendMs_ * 1e6); }
    int holds = 0, jumps = 0;   // losses held, frames refused as jumps (counters for the status line)

private:
    // Counts frames toward confirming a jump or a return: each must be plausible from the one before.
    // Frames that never settle are taken anyway after confirmMaxS, rather than holding a tracked hand still.
    bool Confirmed(const handcut::Hand &h, bool newFrame, int64_t nowNs, const Params &k) {
        if (!newFrame) return pending_ >= k.confirmFrames;
        if (!pending_) pendingNs_ = nowNs;
        pending_ = pending_ > 0 && Plausible(candidate_, h, (nowNs - candidateNs_) / 1e9, k) ? pending_ + 1 : 1;
        candidate_ = h, candidateNs_ = nowNs;
        return pending_ >= k.confirmFrames || nowNs - pendingNs_ > int64_t(k.confirmMaxS * 1e9);
    }
    void Accept(const handcut::Hand &h, int64_t nowNs, bool snap, const Params &k) {
        from_ = snap ? h : shown_;
        blendMs_ = snap ? 0 : std::clamp((nowNs - acceptNs_) / 1e6, k.blendMinMs, k.blendMaxMs);
        blendNs_ = acceptNs_ = nowNs;
        last_ = h;
        if (snap) shown_ = h;
        have_ = true, pending_ = 0, lostNs_ = 0;
    }
    handcut::Hand shown_{}, from_{}, last_{}, candidate_{};   // drawn, blended from, last taken, waiting
    bool have_ = false;
    uint64_t frame_ = ~uint64_t(0);
    int pending_ = 0;
    int64_t lostNs_ = 0, acceptNs_ = 0, blendNs_ = 0, candidateNs_ = 0, pendingNs_ = 0;
    double blendMs_ = 0;
};

}  // namespace handhold
