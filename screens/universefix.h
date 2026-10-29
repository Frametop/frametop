// Universe fix (DEE-206): panels placed while SteamVR had lost the room stay where you saw them
// when it finds the room again.
//
// Every panel sits in SteamVR's standing space. When the Frame loses tracking, vrserver switches
// to a temporary universe: its dummy one (424242, "Switch to the dummy chaperone universe") while
// tracking is gone, and once tracking is back in a new tracking world without relocalizing, a new
// universe with a default floor height ("Switch to a new chaperone universe (default floor height
// without bounds)": standing (0, 1.6, 0), yaw 0 in chaperone_info.vrchap). When it relocalizes, it
// restores the saved room ("Restoring universe ..."), and standing jumps: about 0.5 m in height
// on the Frame of 2026-10-27, since the guessed floor is 1.6 m under the head. A panel placed in
// the temporary universe (the session started, a reset, a drag) jumped with it.
//
// So each panel remembers the universe it was placed in. When the universe changes, the panels
// placed in the one we left move by the jump, if that one was temporary, so they stay where you
// saw them. Panels placed in a room stay put: SteamVR's restore puts them back where they were in
// it. A recenter keeps the universe, so panels follow it as before.
//
// The jump is the head's: standing changes in one tick while the head hardly moves, so it's the
// one step of the head around the switch that its motion doesn't explain. Only its turn about the
// vertical counts, since both standings are level.
//
// Header only, with no OpenVR calls: vr.cpp reads SteamVR and passes the values in.
#pragma once

#include "trackwatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <regex>
#include <string>
#include <vector>

namespace universefix {

using Pose = trackwatch::Pose;

constexpr uint64_t kDummyUniverse = 424242;

enum class Kind { Unknown, Room, Temporary };

inline const char *KindName(Kind k) {
    return k == Kind::Room ? "room" : k == Kind::Temporary ? "temporary" : "unknown";
}

// A universe's kind from chaperone_info.vrchap's text: temporary when it's the dummy universe or
// SteamVR made it with its default floor height (standing translation (0, 1.6, 0), yaw 0), a room
// when it has any other standing, unknown when the file doesn't list it.
inline Kind KindFromChaperone(const std::string &json, uint64_t id) {
    if (id == kDummyUniverse) return Kind::Temporary;
    // Each universe is one object in "universes"; none of their strings hold braces.
    const size_t list = json.find("\"universes\"");
    if (list == std::string::npos) return Kind::Unknown;
    static const std::regex idRe("\"universeID\"\\s*:\\s*\"?([0-9]+)\"?");
    static const std::regex standingRe(
        "\"standing\"\\s*:\\s*\\{[^}]*?\"translation\"\\s*:\\s*\\[\\s*([-+0-9.eE]+)\\s*,\\s*([-+0-9.eE]+)\\s*,"
        "\\s*([-+0-9.eE]+)\\s*\\][^}]*?\"yaw\"\\s*:\\s*([-+0-9.eE]+)");
    int depth = 0;
    size_t start = 0;
    for (size_t i = json.find('[', list); i != std::string::npos && i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{' && depth++ == 0) start = i;
        if (c == '}' && --depth == 0) {
            const std::string u = json.substr(start, i - start + 1);
            std::smatch m;
            if (!std::regex_search(u, m, idRe) || std::strtoull(m[1].str().c_str(), nullptr, 10) != id) continue;
            if (!std::regex_search(u, m, standingRe)) return Kind::Unknown;
            const double x = std::atof(m[1].str().c_str()), y = std::atof(m[2].str().c_str()),
                         z = std::atof(m[3].str().c_str()), yaw = std::atof(m[4].str().c_str());
            const bool guessed = std::fabs(x) < 1e-3 && std::fabs(y - 1.6) < 1e-3 && std::fabs(z) < 1e-3 &&
                                 std::fabs(yaw) < 1e-4;
            return guessed ? Kind::Temporary : Kind::Room;
        }
        if (c == ']' && depth == 0) break;
    }
    return Kind::Unknown;
}

inline Pose Identity() { return Pose{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}; }

// a * b, both rigid.
inline Pose Mul(const Pose &a, const Pose &b) {
    Pose r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    return r;
}

// The turn about the vertical (radians, positive to the left) closest to taking a's orientation
// to b's: the yaw part of R_b R_a^T, which holds up whichever way the head looks.
inline double TurnBetween(const Pose &a, const Pose &b) {
    double r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = b.m[i][0] * a.m[j][0] + b.m[i][1] * a.m[j][1] + b.m[i][2] * a.m[j][2];
    return std::atan2(r[0][2] - r[2][0], r[0][0] + r[2][2]);
}

// The move that turns by `turn` about the vertical and takes the point `from` to `to`.
inline Pose Jump(double turn, const double from[3], const double to[3]) {
    const double c = std::cos(turn), s = std::sin(turn);
    Pose j = Identity();
    j.m[0][0] = float(c), j.m[0][2] = float(s);
    j.m[2][0] = float(-s), j.m[2][2] = float(c);
    j.m[0][3] = float(to[0] - (c * from[0] + s * from[2]));
    j.m[1][3] = float(to[1] - from[1]);
    j.m[2][3] = float(to[2] - (-s * from[0] + c * from[2]));
    return j;
}

// A universe change, once its jump is known.
struct Switch {
    uint64_t from = 0, to = 0;
    bool measured = false;     // the head was tracked around it (else the jump is unknown)
    bool moved = false;        // ...and standing jumped (else `jump` is the identity)
    Pose jump = Identity();    // old standing -> new standing
    double metres = 0, height = 0, turnDeg = 0;
    long lag = 0;              // ticks the universe read changed after standing jumped (logged)
};

// Whether a panel placed in `placedIn` follows a switch: moves by its jump and counts as placed
// in the new universe. Only one placed in the universe left, and only if that was temporary.
inline bool Follows(uint64_t placedIn, const Switch &sw, Kind fromKind) {
    return placedIn != 0 && placedIn == sw.from && fromKind == Kind::Temporary;
}

class Tracker {
public:
    // Ticks either side of the switch whose head steps may hold the jump. The universe is read
    // after the poses, so the jump can land a tick either side of where the switch shows, or more
    // if SteamVR updates the universe property a little after standing (not seen yet: `lag` in
    // the log shows it).
    int window = 6;
    // A step this far off the head's motion is a jump; less is the head moving unevenly.
    double minJump = 0.02, minTurnDeg = 1.5;

    // Each tick, after the poses: the universe now (0: couldn't read it) and the head. True with
    // *out once a switch is `window` ticks old and its jump is worked out.
    bool Tick(long tick, uint64_t universe, bool headValid, const Pose &head, Switch *out) {
        history_.push_back({tick, headValid, head});
        while (history_.size() > size_t(4 * window + 4)) history_.pop_front();
        if (universe != 0 && universe != universe_) {
            if (universe_ != 0) {
                if (!pending_) sw_ = Switch{}, sw_.from = universe_;
                sw_.to = universe;   // (another switch before the last settled: one jump for both)
                pending_ = true, switchTick_ = tick;
            }
            universe_ = universe;
        }
        if (!pending_ || tick - switchTick_ < window) return false;
        pending_ = false;
        Measure(&sw_);
        *out = sw_;
        return true;
    }

private:
    struct Sample {
        long tick;
        bool valid;
        Pose head;
    };
    std::deque<Sample> history_;
    uint64_t universe_ = 0;
    bool pending_ = false;
    long switchTick_ = 0;
    Switch sw_;

    static void Position(const Pose &p, double out[3]) { out[0] = p.m[0][3], out[1] = p.m[1][3], out[2] = p.m[2][3]; }

    // The step with the most the head's motion doesn't explain, among the valid ones within
    // `window` ticks of the switch. The head's motion is the median step per tick there (one jump
    // can't sway it); with under three steps, none.
    void Measure(Switch *sw) const {
        struct Step {
            double from[3], to[3], turn, ticks;
            long at;
        };
        std::vector<Step> steps;
        const Sample *b = nullptr;
        for (const Sample &c : history_) {
            if (c.tick < switchTick_ - window - 1 || !c.valid) continue;
            if (b && c.tick - b->tick <= window) {   // (a longer gap: the head may have moved)
                Step s;
                Position(b->head, s.from), Position(c.head, s.to);
                s.turn = TurnBetween(b->head, c.head), s.ticks = double(c.tick - b->tick), s.at = c.tick;
                steps.push_back(s);
            }
            b = &c;
        }
        double rate[4] = {0, 0, 0, 0};   // per tick: x, y, z, turn
        if (steps.size() >= 3) {
            for (int k = 0; k < 4; ++k) {
                std::vector<double> v;
                for (const Step &s : steps) v.push_back((k < 3 ? s.to[k] - s.from[k] : s.turn) / s.ticks);
                std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                rate[k] = v[v.size() / 2];
            }
        }
        double best = -1;
        for (const Step &s : steps) {
            double from[3];
            for (int k = 0; k < 3; ++k) from[k] = s.from[k] + rate[k] * s.ticks;
            const double turn = s.turn - rate[3] * s.ticks;
            const double d = std::hypot(s.to[0] - from[0], s.to[1] - from[1], s.to[2] - from[2]);
            const double deg = turn * 180 / M_PI;
            const double score = d / minJump + std::fabs(deg) / minTurnDeg;
            if (score <= best) continue;
            best = score;
            sw->measured = true;
            sw->moved = d >= minJump || std::fabs(deg) >= minTurnDeg;
            sw->jump = sw->moved ? Jump(turn, from, s.to) : Identity();
            sw->metres = sw->moved ? d : 0, sw->height = sw->moved ? s.to[1] - from[1] : 0;
            sw->turnDeg = sw->moved ? deg : 0;
            sw->lag = sw->moved ? switchTick_ - s.at : 0;
        }
    }
};

}  // namespace universefix
