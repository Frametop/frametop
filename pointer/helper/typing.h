// Typing on a real keyboard: which bare hands may not click, poke or point now. The pointer helper's
// side of "Typing" in ft-pointer.cpp. Header-only and free of OpenVR, so pointer/tests/typing-test.cpp
// can drive it with made-up keys and hands.
//
// Typing touches thumb to index like a pinch, curls the hand like a grip, and a hand on the keys sits
// where the hand ray's "raised" test begins (2026-10-10 headset session: knuckles 0.29-0.31 m under
// the eyes, the ray's limit 0.30; docs/hands-typing.md has the numbers). A key alone held pinches
// back for 1 s, and typing pauses are longer than that. So:
//
// Keys: the input relay says "typing" on a key (at most 4 times a second). For hold (2 s,
// POINTER_PINCH_TYPING) after a key, every hand is held back except one that has left the keyboard
// (below).
//
// On the keyboard (parked): a hand in the keyboard zone while keys come (or in the posture, below) is
// on the keyboard, and stays there, held back, however long the keys stop, until it leaves: its palm
// rises raise (0.08 m, POINTER_TYPING_RAISE) above where it typed, or turns (its normal's downward
// part under turned), for leave (0.1 s). Then it's free at once, keys or not: a pinch right after
// lifting a hand works. It's on the keyboard again once it's back down in the zone while typing. A
// hand unseen for forget (3 s) is forgotten. A hand in use out of the zone (its ray up, or holding a
// gesture: Hand::busy) is free too: pointing with one hand and pressing Enter with the other. A hand
// on the mouse is on the keyboard too (beside it, palm down, at the same height): its pinches are the
// mouse's buttons.
//   The zone: the palm at least below (0.20 m, POINTER_TYPING_BELOW) under the eyes, aheadMin to
// aheadMax (0.05-0.45 m) ahead of them across the floor (along the head's heading), the palm facing
// down (palmDown). Typing hands measured 0.29-0.42 m under the eyes and 0.18-0.34 m ahead, palms down
// 0.84-1.00; deliberate pointing 0.0-0.30 m under the eyes, 0.33-0.46 m ahead. Where it typed: the
// palm's height, followed with restTau while it stays within raise / 2 of it.
//
// The posture (before the first key): both hands in the zone, level within level (0.08 m) and at most
// gap (0.32 m) apart, for posture (0.3 s, POINTER_TYPING_POSTURE; 0: keys only), count as typing, so
// the first keystroke's pinch is held back too. Typing hands were 0.15-0.22 m apart and level within
// 1-5 cm; a hand on the mouse 0.41 m from the other. Only within recent (600 s, POINTER_TYPING_RECENT;
// 0: always) of a key: a keyboard in use. Deliberate gaze pinches with the elbows resting can sit as
// low as typing, and both hands resting on a desk would otherwise hold them back.
//
// Times are seconds on one steady clock (CLOCK_MONOTONIC). Positions are the standing universe's
// (+y up), metres.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace typing {

struct V3 {
    double x = 0, y = 0, z = 0;
};

struct Params {
    double hold = 2.0;        // POINTER_PINCH_TYPING: keys hold every hand back this long (0: no guard at all)
    double raise = 0.08;      // POINTER_TYPING_RAISE: a hand leaves the keyboard this far above where it typed
    double turned = 0.2;      // ...or with its palm turned (the normal's downward part under this)
    double leave = 0.1;       // ...for this long
    double below = 0.20;      // POINTER_TYPING_BELOW: the zone: the palm at least this far under the eyes
    double aheadMin = 0.05, aheadMax = 0.45;  // ...this far ahead of them
    double palmDown = 0.6;    // ...facing down at least this much
    double posture = 0.3;     // POINTER_TYPING_POSTURE: both hands in the posture this long is typing (0: off)
    double gap = 0.32, level = 0.08;  // the posture: palms this close, this level
    double recent = 600;      // POINTER_TYPING_RECENT: the posture only this long after a key (0: always)
    double forget = 3.0;      // a hand unseen this long is forgotten
    double restTau = 0.5;     // where it typed follows the palm this slowly
};

// One side's hand in a frame: seen, its palm's centre, and how far its palm faces down (the palm
// normal's -y: 1 straight down).
struct Hand {
    bool seen = false;
    V3 palm;
    double down = 0;
    bool busy = false;  // its ray is up or it holds a gesture: in use elsewhere, if it's out of the zone
};

// The head in that frame: where the eyes are, and where it faces (its -z; only the heading counts).
struct Head {
    V3 pos, fwd{0, 0, -1};
};

class Typing {
public:
    explicit Typing(Params p = {}) : p_(p) {}
    void SetParams(const Params &p) { p_ = p; }
    const Params &params() const { return p_; }

    // A key on a keyboard at t.
    void Key(double t) { lastKey_ = t; }
    double LastKey() const { return lastKey_; }

    // Each hands frame (t: its capture time).
    void Frame(double t, const Head &head, const Hand hands[2]) {
        if (p_.hold <= 0) {
            for (Side &s : side_) s = Side{};
            postureSince_ = -1, posture_ = false;
            return;
        }
        double fx = head.fwd.x, fz = head.fwd.z;
        const double fl = std::hypot(fx, fz);
        if (fl > 1e-6) fx /= fl, fz /= fl;
        else fx = 0, fz = -1;
        bool in[2] = {false, false};
        for (int k = 0; k < 2; ++k) {
            const Hand &h = hands[k];
            if (!h.seen) continue;
            const double below = head.pos.y - h.palm.y;
            const double ahead = (h.palm.x - head.pos.x) * fx + (h.palm.z - head.pos.z) * fz;
            in[k] = below >= p_.below && ahead >= p_.aheadMin && ahead <= p_.aheadMax && h.down >= p_.palmDown;
        }
        // the posture
        bool pose = in[0] && in[1] && std::fabs(hands[0].palm.y - hands[1].palm.y) <= p_.level &&
                    Dist(hands[0].palm, hands[1].palm) <= p_.gap;
        pose = pose && p_.posture > 0 && (p_.recent <= 0 || (lastKey_ > -1e17 && t - lastKey_ < p_.recent));
        if (!pose) postureSince_ = -1, posture_ = false;
        else if (postureSince_ < 0) postureSince_ = t;
        if (pose && !posture_ && t - postureSince_ >= p_.posture - 1e-9) {
            posture_ = true;
            log.push_back("typing: both hands in the keyboard posture");
        }
        const bool typing = Keys(t) || posture_;
        for (int k = 0; k < 2; ++k) {
            Side &s = side_[k];
            const Hand &h = hands[k];
            if (!h.seen) {
                if ((s.parked || s.free) && t - s.seen > p_.forget) {
                    if (s.parked) log.push_back(std::string("typing: ") + Name(k) + " hand forgotten (unseen)");
                    s = Side{};
                }
                continue;
            }
            const double dt = s.seen > -1e17 ? std::clamp(t - s.seen, 0.0, 1.0) : 0;
            s.seen = t;
            if (s.parked) {
                const double rose = h.palm.y - s.restY;
                const bool up = rose >= p_.raise, turned = h.down < p_.turned;
                if (up || turned) {
                    if (s.leaveSince < 0) s.leaveSince = t;
                    if (t - s.leaveSince >= p_.leave - 1e-9) {
                        s.parked = false, s.free = true, s.leaveSince = -1;
                        char b[96];
                        if (up) std::snprintf(b, sizeof b, "typing: %s hand off the keyboard (raised %.2f m)", Name(k), rose);
                        else std::snprintf(b, sizeof b, "typing: %s hand off the keyboard (palm turned)", Name(k));
                        log.push_back(b);
                    }
                } else {
                    s.leaveSince = -1;
                    if (std::fabs(rose) < p_.raise / 2)
                        s.restY += rose * (1 - std::exp(-dt / std::max(1e-3, p_.restTau)));
                }
                continue;
            }
            // back down where it typed (or in the zone, if it never typed here) while typing: on the keyboard
            const bool down = in[k] && (!s.known || h.palm.y - s.restY < p_.raise / 2);
            // pointing or holding a gesture out of the zone when keys come: not typing (point, then Enter)
            if (h.busy && !in[k] && !s.free) s.free = true;
            if (typing && down) {
                s.parked = true, s.free = false, s.leaveSince = -1;
                if (!s.known || std::fabs(h.palm.y - s.restY) >= p_.raise / 2) s.restY = h.palm.y;
                s.known = true;
                log.push_back(std::string("typing: ") + Name(k) + " hand on the keyboard");
            }
        }
    }

    // Typing by the keys: within hold of the last one.
    bool Keys(double t) const { return p_.hold > 0 && lastKey_ > -1e17 && t - lastKey_ < p_.hold; }
    bool Posture() const { return posture_; }
    bool Parked(int s) const { return side_[s].parked; }
    bool Free(int s) const { return side_[s].free; }

    // Why side s may not click, poke or point at t, or nullptr: its hand is on the keyboard, or keys
    // came and it hasn't left the keyboard since.
    const char *Why(int s, double t) const {
        if (p_.hold <= 0 || s < 0 || s > 1) return nullptr;
        if (side_[s].parked) return "typing: the hand is on the keyboard";
        if (side_[s].free) return nullptr;
        if (Keys(t) || posture_) return "typing";
        return nullptr;
    }
    bool Blocked(int s, double t) const { return Why(s, t) != nullptr; }

    std::vector<std::string> log;  // changes, since the caller last cleared it

private:
    struct Side {
        bool parked = false;  // on the keyboard
        bool free = false;    // left it since (keys don't hold it back)
        bool known = false;   // restY is where it typed
        double restY = 0, seen = -1e18, leaveSince = -1;
    };
    static double Dist(const V3 &a, const V3 &b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
    static const char *Name(int k) { return k ? "right" : "left"; }
    Params p_;
    Side side_[2];
    double lastKey_ = -1e18, postureSince_ = -1;
    bool posture_ = false;
};

}  // namespace typing
