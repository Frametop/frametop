// Handoff: which hand works with a controller and which bare, decided per side, automatically.
// The pointer helper's arbiter for hand interaction (frame-hands/notes/hand-interaction.md,
// section 1). Header-only and free of OpenVR, so pointer/tests/handoff-test.cpp can drive it
// with made-up timelines.
//
// Per side the state is controller or hand (or off: no hand tracking, and no controller in use).
// A controller used recently wins its side; otherwise the bare hand does. One controller and one
// bare hand at once is fine.
//
// Goes to controller at once on any of:
//   - touch: a touch sensor of that side's controller is touched (the input relay reads them on
//     vrserver's web socket and sends "ctl <side> touch 0|1"; the strongest signal: a controller
//     on a desk reads no touch);
//   - press: a button of it went down ("ctl <side> press");
//   - moving: faster than moveSpeed (0.15 m/s) or moveSpin (1 rad/s) for moveFor (100 ms) in a
//     row, tracked normally (a knock on the desk is one or two samples);
//   - a tracked hand within nearGrip (12 cm) of it, moving with it over togetherFor (200 ms): held
//     by the ring, no skin on a sensor.
// Goes back to hand only when all of these hold: no touch for touchHold (1.5 s), no press for
// pressHold (3 s), not moving for stillHold (3 s), and no tracked hand within nearGrip of it.
// Or at once when that side has no controller (disconnected, gone from the web socket), it's
// charging (docked), or it has been out of tracking for lostFor (1 s) while that side's hand is
// seen more than lostAway (30 cm) from where it was last tracked.
// Without the web socket ("ctl ws 0"), touch and press are unknown: a controller whose position
// spreads more than heldStillStd (1 mm) over heldStillWindow (1 s) counts as touched (held; one
// resting on a desk spreads less). Probe P5 (pointer/probe/ctlnoise) measures the resting noise.
//
// NearController(p): a bare hand at p within nearGrip of any tracked controller is reaching for
// it or holding it: its gestures are ignored whatever its side (ft-hands' left/right call is
// wrong about 2.5% of the time, so this goes by position).
//
// Positions are metres in one room frame (the standing universe); times are seconds on one
// steady clock. A controller's side is its SteamVR hand role.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace handoff {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

enum class Owner { Off, Hand, Controller };
inline const char *Name(Owner o) { return o == Owner::Hand ? "hand" : o == Owner::Controller ? "controller" : "off"; }
inline const char *SideName(int s) { return s ? "right" : "left"; }

struct Params {
    double touchHold = 1.5, pressHold = 3.0, stillHold = 3.0;
    double moveSpeed = 0.15, moveSpin = 1.0, moveFor = 0.1;
    double nearGrip = 0.12;
    double togetherFor = 0.2, togetherDiff = 0.1, togetherHandSpeed = 0.1;
    double lostFor = 1.0, lostAway = 0.30;
    double heldStillStd = 0.001, heldStillWindow = 1.0;
};

// A real controller, this frame.
struct Controller {
    uint32_t id = 0;         // its device index
    int side = -1;           // 0 left, 1 right (its hand role), -1 none: left out
    bool connected = true;
    bool charging = false;
    bool tracked = false;    // a valid pose, tracked normally (Running_OK)
    bool lost = false;       // out of range, or no valid pose
    V3 pos;                  // where a hand holds it (room)
    V3 vel;                  // m/s
    double spin = 0;         // rad/s
};

// A tracked bare hand, this frame (room frame).
struct Hand {
    int side = -1;           // ft-hands' call: 0 left, 1 right
    V3 pos;                  // the palm's centre
    V3 vel;                  // room-frame velocity, m/s
    bool velValid = false;
};

class Handoff {
public:
    explicit Handoff(Params p = {}) : p_(p) {}
    const Params &params() const { return p_; }
    void SetParams(const Params &p) { p_ = p; }

    // ---- from the input relay ----
    void Touch(int side, bool on, double t) {
        if (side < 0 || side > 1) return;
        S &s = side_[side];
        if (on) s.lastTouch = t;
        else if (s.touch) s.lastTouch = t;  // touched until now
        s.gone = false;  // the web socket sees it again
        s.touch = on;
    }
    void Press(int side, double t) {
        if (side < 0 || side > 1) return;
        side_[side].lastPress = t;
        side_[side].pressNew = true;
        side_[side].gone = false;
    }
    // The side's controller left the web socket's list (turned off): no controller there until the
    // relay reports it again (its touch state, or a press), whatever OpenVR still lists.
    void Gone(int side, double t) {
        if (side < 0 || side > 1) return;
        S &s = side_[side];
        if (s.touch) s.lastTouch = t;
        s.touch = false;
        s.gone = true;
    }
    void Socket(bool up, double t) {
        if (up == ws_) return;
        ws_ = up;
        if (!up)
            for (S &s : side_) {
                if (s.touch) s.lastTouch = t;
                s.touch = false;
            }
        log.push_back(up ? "handoff: SteamVR's web socket is up: touch sensors and buttons say who holds a controller"
                         : "handoff: no web socket: touch and buttons unknown, a controller held still (position "
                           "spread over 1 s) counts as held");
    }
    bool SocketUp() const { return ws_; }

    // ---- each frame ----
    // handsLive: hand tracking publishes (a side with no controller in use is "hand", else "off").
    void Update(double t, const std::vector<Controller> &ctls, const std::vector<Hand> &hands, bool handsLive) {
        handsLive_ = handsLive;
        for (int k = 0; k < 2; ++k) {
            S &s = side_[k];
            const Owner before = s.owner;
            bool any = false, allCharging = true, lostAway = false, near = false, together = false;
            std::string why;
            for (const Controller &c : ctls) {
                if (c.side != k || !c.connected) continue;
                any = true;
                allCharging = allCharging && c.charging;
                D &d = dev_[c.id];
                d.seen = t;
                // moving, in a row, tracked normally
                const bool fast = c.tracked && (Len(c.vel) > p_.moveSpeed || c.spin > p_.moveSpin);
                if (!fast) {
                    d.movingSince = -1;
                } else {
                    if (d.movingSince < 0) d.movingSince = t;
                    if (t - d.movingSince >= p_.moveFor - 1e-9) {
                        if (s.lastMove < d.movingSince) {
                            char b[64];
                            std::snprintf(b, sizeof b, "moving %.2f m/s %.1f rad/s", Len(c.vel), c.spin);
                            why = b;
                        }
                        s.lastMove = t;
                    }
                }
                // tracking, and where it was last seen
                if (c.tracked) d.last = c.pos, d.lostSince = -1, d.hasLast = true;
                else if (c.lost && d.lostSince < 0) d.lostSince = t;
                if (c.lost && d.lostSince >= 0 && t - d.lostSince >= p_.lostFor && d.hasLast) {
                    bool away = false, seen = false;
                    for (const Hand &h : hands)
                        if (h.side == k) seen = true, away = away || Len(Sub(h.pos, d.last)) > p_.lostAway;
                    lostAway = lostAway || (seen && away);
                }
                // a hand on it, and moving with it
                if (c.tracked)
                    for (const Hand &h : hands) {
                        if (Len(Sub(h.pos, c.pos)) > p_.nearGrip) continue;
                        near = true;
                        if (h.velValid && Len(h.vel) > p_.togetherHandSpeed && Len(Sub(h.vel, c.vel)) < p_.togetherDiff) {
                            if (d.togetherSince < 0) d.togetherSince = t;
                        } else {
                            d.togetherSince = -1;
                        }
                        if (d.togetherSince >= 0 && t - d.togetherSince >= p_.togetherFor - 1e-9) together = true;
                    }
                else
                    d.togetherSince = -1;
                // held still, without the web socket
                if (!ws_ && c.tracked) {
                    d.window.push_back({t, c.pos});
                    while (!d.window.empty() && t - d.window.front().first > p_.heldStillWindow) d.window.erase(d.window.begin());
                    if (d.window.size() >= 5 && Spread(d.window) > p_.heldStillStd) {
                        if (s.lastHeldStill < t - 0.5) why = why.empty() ? "held (position spread, no web socket)" : why;
                        s.lastHeldStill = t;
                    }
                } else {
                    d.window.clear();
                }
            }
            if (near) s.lastNear = t;
            if (together) s.lastMove = t;
            const bool noController = !any || s.gone;

            // to controller at once
            bool toCtl = false;
            if (!noController && !allCharging) {
                if (s.touch) toCtl = true, why = why.empty() ? "touch" : why;
                if (s.pressNew) toCtl = true, why = "press";
                if (s.lastMove == t) toCtl = true;
                if (together) toCtl = true, why = "a hand moving with it";
                if (!ws_ && s.lastHeldStill == t) toCtl = true;
            }
            s.pressNew = false;

            Owner now = s.owner == Owner::Controller ? Owner::Controller : Owner::Hand;
            if (toCtl) {
                now = Owner::Controller;
            } else if (s.owner == Owner::Controller) {
                std::string back;
                if (noController) back = "no controller";
                else if (allCharging) back = "charging";
                else if (lostAway) back = "lost tracking, the hand is away from it";
                else if (!s.touch && t - s.lastTouch >= p_.touchHold && t - s.lastPress >= p_.pressHold &&
                         t - s.lastMove >= p_.stillHold && s.lastNear != t &&
                         (ws_ || t - s.lastHeldStill >= p_.touchHold)) {
                    char b[96];
                    std::snprintf(b, sizeof b, "no touch %.1f s, still %.1f s", std::fmin(t - s.lastTouch, 99.0),
                                  std::fmin(t - s.lastMove, 99.0));
                    back = b;
                }
                if (!back.empty()) now = Owner::Hand, why = back;
            }
            s.owner = now;
            if (now != before && !(before == Owner::Off && now == Owner::Hand)) {
                char b[200];
                std::snprintf(b, sizeof b, "handoff %s: %s -> %s (%s)", SideName(k), Name(before), Name(now), why.c_str());
                log.push_back(b);
            }
        }
        // forget controllers gone a while
        for (auto it = dev_.begin(); it != dev_.end();)
            it = t - it->second.seen > 5 ? dev_.erase(it) : std::next(it);
        controllers_ = ctls;
    }

    // What the side is now: controller, else hand while hand tracking publishes, else off.
    Owner State(int side) const {
        if (side < 0 || side > 1) return Owner::Off;
        if (side_[side].owner == Owner::Controller) return Owner::Controller;
        return handsLive_ ? Owner::Hand : Owner::Off;
    }
    bool IsController(int side) const { return State(side) == Owner::Controller; }
    // When the side's controller last had a button pressed (-1e9: never).
    double LastPress(int side) const { return side < 0 || side > 1 ? -1e9 : side_[side].lastPress; }

    // A bare hand at p is reaching for or holding a tracked controller.
    bool NearController(V3 p) const {
        for (const Controller &c : controllers_)
            if (c.connected && c.tracked && c.side >= 0 && Len(Sub(p, c.pos)) <= p_.nearGrip) return true;
        return false;
    }

    std::vector<std::string> log;  // transitions and notes since the caller last cleared it

private:
    struct S {
        Owner owner = Owner::Off;
        bool touch = false, gone = false, pressNew = false;
        double lastTouch = -1e9, lastPress = -1e9, lastMove = -1e9, lastNear = -1e9, lastHeldStill = -1e9;
    };
    struct D {
        double seen = 0, movingSince = -1, lostSince = -1, togetherSince = -1;
        V3 last;
        bool hasLast = false;
        std::vector<std::pair<double, V3>> window;
    };
    static double Spread(const std::vector<std::pair<double, V3>> &w) {
        double m[3] = {}, q[3] = {};
        for (const auto &e : w) {
            const double v[3] = {e.second.x, e.second.y, e.second.z};
            for (int k = 0; k < 3; ++k) m[k] += v[k], q[k] += v[k] * v[k];
        }
        double best = 0;
        for (int k = 0; k < 3; ++k) {
            const double mean = m[k] / double(w.size());
            best = std::fmax(best, std::sqrt(std::fmax(0.0, q[k] / double(w.size()) - mean * mean)));
        }
        return best;
    }
    Params p_;
    S side_[2];
    std::map<uint32_t, D> dev_;
    std::vector<Controller> controllers_;
    bool ws_ = false, handsLive_ = false;
};

}  // namespace handoff
