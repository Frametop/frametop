// Which bare hand's ray drives the pointer, which tracked hand each ray follows, and where a ray
// pinch clicks. The pointer helper's side of the hand ray ("Hand ray" in ft-pointer.cpp; the ray
// itself is hands/include/fh_ray.h). Header-only and free of OpenVR, so pointer/tests/rayowner-test.cpp
// can drive it with made-up timelines.
//
// Owner: one ray drives the pointer (one SteamVR laser). With none, the first ray to show takes it.
// The owner keeps it while its ray is up, and through a grace (0.5 s, POINTER_RAY_GRACE) after its
// ray goes down: the pointer holds the ray's last pose meanwhile, and the ray coming back carries on.
// Closing the fingers turns the hand, and a ray near a limit of fh_ray's gate drops for a moment;
// without the grace the other hand's ray took the laser at once and kept it (the laser "jumping
// between hands", live test 2026-10-09). The other hand takes over:
//   - when it pinches with its ray up (or able to come up, fh_ray's ShowForPinch): last used wins;
//   - when its ray shows while the owner's is down (a deliberate raise; a ray that was up all along
//     doesn't count);
//   - when the owner's ray has been down for the grace and its ray is up.
// The owner leaves at once when its side may not point (its controller in use, a game, the mouse) or
// its hand went near a panel (fh_ray's "near": ft-screens' poke and grab, no frozen beam meanwhile),
// and after the grace when no other ray is up.
//
// Pinch: a pinch on the owner's hand is a ray pinch (its ray brought back if it dropped within the
// grace). A pinch on the other hand is a ray pinch when that hand's ray is up or can come up: it
// takes the ray, and the click is where its own ray pointed (not where the old owner's did).
// Otherwise it isn't one (the helper's gaze-style hold).
//
// Slots: which of ft-hands' hands each side's ray follows. ft-hands calls each hand left or right
// per frame, and a call can flip for a few frames (or a pair swap): following the call, a ray's
// knuckle point jumped to the other hand and back. A ray keeps the hand it follows (by id) until
// ft-hands has called it the other side for flipHold (0.3 s), or it's gone; then it takes the hand
// called its side that the other ray doesn't follow.
//
// Aim lock: where a ray pinch clicks. ft-hands sees a pinch once the thumb and index tips are
// within begin_m, and closing them turns the hand (1.3-1.6 deg median in the first 150 ms after the
// begin, 2.4-3.5 at the 90th percentile, on recordings; more before it). So the press goes out
// along the ray as it was just before the fingers began to close: the newest frame, within lockBack
// (0.35 s) before the begin, with the tips still lockOpen (1 cm) further apart than the pinch's
// release distance (end_m, 3.5 cm); without a distance (no skeleton) or with fingers resting nearly
// closed, the frame lockLead (0.1 s) before the begin. Only frames with the ray up count. The rays
// here are fh_ray's filtered ones, what the beam showed.
//
// Times are seconds on one steady clock (CLOCK_MONOTONIC: ft-hands' capture times as seconds).
#pragma once

#include "../../hands/include/fh_ray.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace rayown {

inline const char *SideName(int s) { return s < 0 ? "none" : s ? "right" : "left"; }

struct Params {
    double grace = 0.5;      // the owner's ray down this long: it's gone
    double flipHold = 0.3;   // a hand called the other side this long moves to that side's ray
    double lockBack = 0.35;  // aim lock: how far back before a pinch's begin to look
    double lockLead = 0.10;  // aim lock: the frame this long before the begin, without a distance
    double lockOpen = 0.01;  // aim lock: the tips this much further apart than end_m: still open
};

// A change of owner: from, to (-1 none), why, and off (the old owner's side may not point: the
// pointer goes back at once, see rayLeave in ft-pointer.cpp).
struct Change {
    int from, to;
    std::string why;
    bool off = false;
};

class Owner {
public:
    explicit Owner(Params p = {}) : p_(p) {}
    void SetParams(const Params &p) { p_ = p; }
    int Side() const { return side_; }
    // The owner's ray is down, within the grace: the pointer holds its last pose.
    bool Waiting() const { return side_ >= 0 && downSince_ >= 0; }

    // Each tick. shown: each side's ray is up; may: each side may point at all (fh_ray's sideIsHand);
    // why: why each ray is down, for the log (fh_ray's hide reason).
    void Update(double t, const bool shown[2], const bool may[2], const std::string why[2]) {
        for (int s = 0; s < 2; ++s) {
            if (shown[s] && !was_[s]) shownAt_[s] = t;
            was_[s] = shown[s];
        }
        if (side_ < 0) {
            int pick = -1;
            for (int s = 0; s < 2; ++s)
                if (shown[s] && (pick < 0 || shownAt_[s] < shownAt_[pick])) pick = s;
            if (pick >= 0) Set(pick, "raised", false);
            return;
        }
        const int o = side_, other = 1 - o;
        if (!may[o]) {
            Set(-1, "off: its controller, a game, or the mouse", true);
            return;
        }
        if (!shown[o] && why[o] == "near") {
            Set(-1, "near a panel", false);
            return;
        }
        if (shown[o]) {
            if (downSince_ >= 0) {
                char b[96];
                std::snprintf(b, sizeof b, "hand ray %s: back after %.2f s", SideName(o), t - downSince_);
                log.push_back(b);
            }
            downSince_ = -1;
            return;
        }
        if (downSince_ < 0) {
            downSince_ = t, downWhy_ = why[o].empty() ? "pose" : why[o];
            log.push_back(std::string("hand ray ") + SideName(o) + ": down (" + downWhy_ + ")");
        }
        if (shown[other] && shownAt_[other] > downSince_) {
            Set(other, std::string(SideName(other)) + " raised while " + SideName(o) + "'s was down", false);
        } else if (t - downSince_ >= p_.grace - 1e-9) {
            char b[128];
            std::snprintf(b, sizeof b, "%s's down %.1f s (%s)%s%s", SideName(o), p_.grace, downWhy_.c_str(),
                          shown[other] ? ", up: " : "", shown[other] ? SideName(other) : "");
            Set(shown[other] ? other : -1, b, false);
        }
    }

    enum class Pinch { Ray, Take, None };
    // A pinch began on side s; canRay: its ray is up or may come up for it (fh_ray's CanShowForPinch).
    // Ray: the owner's, a ray pinch; Take: the other hand's, it takes the ray and it's a ray pinch;
    // None: not a ray pinch.
    Pinch Pinched(int s, bool canRay) {
        if (!canRay || s < 0 || s > 1) return Pinch::None;
        if (s == side_) {
            downSince_ = -1;  // brought back for the pinch
            return Pinch::Ray;
        }
        Set(s, std::string(SideName(s)) + " pinched", false);
        return Pinch::Take;
    }
    // The pointer went elsewhere (the mouse took it, hands turned off): no owner, at once.
    void Drop(const std::string &why) {
        if (side_ >= 0) Set(-1, why, false);
    }

    std::vector<Change> changes;   // since the caller last cleared it
    std::vector<std::string> log;  // the owner's ray going down and coming back, likewise

private:
    void Set(int to, const std::string &why, bool off) {
        if (to == side_) return;
        changes.push_back({side_, to, why, off});
        side_ = to, downSince_ = -1;
    }
    Params p_;
    int side_ = -1;
    double downSince_ = -1, shownAt_[2] = {-1e9, -1e9};
    bool was_[2] = {false, false};
    std::string downWhy_;
};

// One tracked hand this frame, as ft-hands calls it.
struct SeenHand {
    uint32_t id = 0;
    int side = 0;  // 0 left, 1 right
};

class Slots {
public:
    explicit Slots(Params p = {}) : p_(p) {}
    // This frame's hands: which one each side's ray follows (pick[s], an index into hands; -1 none).
    void Update(double t, const std::vector<SeenHand> &hands, int pick[2]) {
        pick[0] = pick[1] = -1;
        for (int s = 0; s < 2; ++s) {
            if (!bound_[s]) continue;
            int at = -1;
            for (size_t i = 0; i < hands.size(); ++i)
                if (hands[i].id == id_[s]) at = int(i);
            if (at < 0) {
                bound_[s] = false;  // gone
                continue;
            }
            if (hands[at].side == s) {
                flipSince_[s] = -1;
            } else {
                if (flipSince_[s] < 0) flipSince_[s] = t;
                if (t - flipSince_[s] >= p_.flipHold - 1e-9) {
                    bound_[s] = false;  // ft-hands has settled on the other side
                    continue;
                }
            }
            pick[s] = at;
        }
        for (int s = 0; s < 2; ++s) {
            if (pick[s] >= 0) continue;
            for (size_t i = 0; i < hands.size(); ++i)
                if (hands[i].side == s && int(i) != pick[1 - s]) {
                    pick[s] = int(i);
                    break;
                }
            bound_[s] = pick[s] >= 0;
            if (bound_[s]) id_[s] = hands[pick[s]].id, flipSince_[s] = -1;
        }
    }
    // The side whose ray follows hand id, or -1.
    int SideOf(uint32_t id) const {
        for (int s = 0; s < 2; ++s)
            if (bound_[s] && id_[s] == id) return s;
        return -1;
    }

private:
    Params p_;
    uint32_t id_[2] = {0, 0};
    bool bound_[2] = {false, false};
    double flipSince_[2] = {-1, -1};
};

// One side's recent rays, for the aim lock.
class AimHistory {
public:
    explicit AimHistory(Params p = {}) : p_(p) {}
    // A hands frame at capture time t: the ray (filtered), whether it's up, and the thumb to index
    // distance (m; < 0 unknown).
    void Add(double t, const fhray::Ray &ray, bool shown, double pinchD) {
        h_.push_back({t, ray, shown, pinchD});
        while (!h_.empty() && h_.front().t < t - 1.0) h_.pop_front();
    }
    // The ray to press along for a pinch that began at tBegin (see the top); endD: the pinch's release
    // distance (end_m). False: no frame with the ray up within lockBack. age: how long before the begin.
    bool Lock(double tBegin, double endD, fhray::Ray &out, double &age) const {
        const double openD = endD + p_.lockOpen;
        const S *open = nullptr, *lead = nullptr;
        for (auto it = h_.rbegin(); it != h_.rend(); ++it) {
            if (it->t > tBegin + 1e-6) continue;
            if (it->t < tBegin - p_.lockBack - 1e-6) break;
            if (!it->shown) continue;
            if (!open && it->d >= openD) open = &*it;
            if (!lead && it->t <= tBegin - p_.lockLead + 1e-6) lead = &*it;
        }
        const S *pick = open ? open : lead;
        if (!pick) {  // nothing that far back: the oldest up within the window
            for (const S &e : h_)
                if (e.shown && e.t <= tBegin + 1e-6 && e.t >= tBegin - p_.lockBack - 1e-6) {
                    pick = &e;
                    break;
                }
        }
        if (!pick) return false;
        out = pick->ray, age = tBegin - pick->t;
        return true;
    }
    void Clear() { h_.clear(); }

private:
    struct S {
        double t;
        fhray::Ray ray;
        bool shown;
        double d;
    };
    Params p_;
    std::deque<S> h_;
};

}  // namespace rayown
