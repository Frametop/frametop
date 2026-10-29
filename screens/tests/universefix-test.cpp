// The universe fix (screens/universefix.h): which universes are temporary, the jump at a switch,
// and which panels follow it.
#include "../universefix.h"

#include <cstdio>

using namespace universefix;

static int failed = 0;

static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A head pose: at (x, y, z), looking yaw degrees left of -z and pitch degrees up.
static Pose head(double x, double y, double z, double yawDeg, double pitchDeg = 0) {
    const double y0 = yawDeg * M_PI / 180, p0 = pitchDeg * M_PI / 180;
    const double cy = std::cos(y0), sy = std::sin(y0), cp = std::cos(p0), sp = std::sin(p0);
    // R = Ry(yaw) Rx(pitch)
    Pose r{{{float(cy), float(sy * sp), float(sy * cp), float(x)},
            {0, float(cp), float(-sp), float(y)},
            {float(-sy), float(cy * sp), float(cy * cp), float(z)}}};
    return r;
}

static double apart(const Pose &a, const Pose &b) {
    double worst = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) worst = std::max(worst, double(std::fabs(a.m[i][j] - b.m[i][j])));
    return worst;
}

// Where p is seen from the head h: h^-1 p.
static Pose seen(const Pose &h, const Pose &p) {
    Pose inv{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) inv.m[i][j] = h.m[j][i];
    for (int i = 0; i < 3; ++i) inv.m[i][3] = -(inv.m[i][0] * h.m[0][3] + inv.m[i][1] * h.m[1][3] + inv.m[i][2] * h.m[2][3]);
    return Mul(inv, p);
}

static const char *kChaperone = R"({
   "jsonid" : "chaperone_info",
   "universes" : [
      {
         "collision_bounds" : [ [ [ -1, 0, -1 ], [ 1, 0, -1 ] ] ],
         "play_area" : [ 2, 2 ],
         "seated" : { "translation" : [ 0, 0, 0 ], "yaw" : 0 },
         "standing" : { "translation" : [ 0, 1.60000002, 0 ], "yaw" : 0 },
         "time" : "Wed Oct 28 20:58:57 2026",
         "universeID" : "4396560858472634643"
      },
      {
         "collision_bounds" : [ [ [ -0.5, 0, -2 ], [ 0.5, 0, -2 ] ] ],
         "play_area" : [ 3.1, 2.4 ],
         "seated" : { "translation" : [ -0.279505819, 0.175504774, 1.01056802 ], "yaw" : 1.09098911 },
         "standing" : { "translation" : [ -0.370687932, 1.41664124, -0.449202031 ], "yaw" : -1.11575031 },
         "time" : "Tue Oct 20 16:46:15 2026",
         "universeID" : "7619347082598966940"
      }
   ],
   "version" : 5
})";

constexpr uint64_t kRoom = 7619347082598966940ull, kGuess = 4396560858472634643ull;

// A head walking forward and turning, with standing jumping by `j` from tick `jumpAt` (the head's
// pose in the new standing is j times the old one), and the universe read as changed from tick
// `switchAt`. Runs `ticks` ticks; returns how many switches came out, the last in *out.
static int run(Tracker &t, const Pose &j, long jumpAt, long switchAt, long ticks, Switch *out,
               double turnPerTick = 0.5, long lostFrom = -1, long lostTo = -1, Pose *headBefore = nullptr,
               Pose *headAfter = nullptr) {
    int n = 0;
    for (long k = 0; k < ticks; ++k) {
        Pose h = head(0.1 + 0.008 * k, 1.07 + 0.001 * k, -0.2 - 0.006 * k, 10 + turnPerTick * k, -20);
        if (headBefore && k == jumpAt - 1) *headBefore = h;
        if (k >= jumpAt) h = Mul(j, h);
        if (headAfter && k == jumpAt) *headAfter = h;
        const bool valid = k < lostFrom || k > lostTo;
        Switch sw;
        if (t.Tick(k, k < switchAt ? kGuess : kRoom, valid, h, &sw)) *out = sw, ++n;
    }
    return n;
}

int main() {
    // which universes are temporary
    const std::string chap = kChaperone;
    check("the default floor height universe is temporary", KindFromChaperone(chap, kGuess) == Kind::Temporary);
    check("a set-up room is a room", KindFromChaperone(chap, kRoom) == Kind::Room);
    check("the dummy universe is temporary, file or not", KindFromChaperone("", kDummyUniverse) == Kind::Temporary);
    check("one the file doesn't list is unknown", KindFromChaperone(chap, 12345) == Kind::Unknown);
    check("no file is unknown", KindFromChaperone("", kRoom) == Kind::Unknown);
    check("a guessed floor moved by a recenter is a room",
          KindFromChaperone(R"({"universes":[{"standing":{"translation":[0.2,1.6,0],"yaw":0},"universeID":"5"}]})", 5) ==
              Kind::Room);
    const auto standing = [](const char *t) {
        return KindFromChaperone(std::string(R"({"universes":[{"universeID":"5","standing":)") + t + "}]}", 5);
    };
    check("a floor 2 mm off the guess is a room", standing(R"({"translation":[0,1.602,0],"yaw":0})") == Kind::Room);
    check("...or 2 mm aside", standing(R"({"translation":[0.002,1.6,0],"yaw":0})") == Kind::Room &&
                                  standing(R"({"translation":[0,1.6,-0.002],"yaw":0})") == Kind::Room);
    check("the guess turned 0.0002 rad is a room", standing(R"({"translation":[0,1.6,0],"yaw":0.0002})") == Kind::Room);
    check("the guess off by under 1 mm is temporary",
          standing(R"({"translation":[0.0005,1.6005,-0.0005],"yaw":0.00005})") == Kind::Temporary);
    check("the guess as SteamVR writes it (float noise) is temporary",
          standing(R"({"translation":[0.0000001,1.60000002,-0.0000001],"yaw":0.000001})") == Kind::Temporary);
    check("ids written as numbers count too",
          KindFromChaperone(R"({"universes":[{"universeID":5,"standing":{"translation":[0,1.6,0],"yaw":0}}]})", 5) ==
              Kind::Temporary);

    // the jump: up 0.53 m (a 1.6 m guessed floor against a real one), over and turned 20 degrees
    const double from[3] = {0, 0, 0}, to[3] = {0.37, 0.53, -0.45};
    const Pose j = Jump(20 * M_PI / 180, from, to);
    {
        Tracker t;
        Switch sw;
        Pose before{}, after{};
        const int n = run(t, j, 40, 40, 60, &sw, 0.5, -1, -1, &before, &after);
        check("one switch comes out", n == 1 && sw.from == kGuess && sw.to == kRoom);
        check("its jump is measured and moved", sw.measured && sw.moved);
        check("the jump is the standing's, within a few mm", apart(sw.jump, j) < 0.005);
        std::printf("      measured %.3f m (height %+.3f m), turn %+.1f deg\n", sw.metres, sw.height, sw.turnDeg);
        // A panel 1.5 m ahead of the head, placed in the old standing just before the jump:
        // moved by the jump, it's where it was as seen from the head just after.
        const Pose panel = Mul(before, head(0, 0, -1.5, 0));
        const Pose moved = Mul(sw.jump, panel);
        check("a panel moved by it stays where it was seen", apart(seen(after, moved), seen(before, panel)) < 0.02);
        check("left alone it would have jumped", apart(seen(after, panel), seen(before, panel)) > 0.3);
    }
    {
        Tracker t;
        Switch sw;
        run(t, j, 41, 40, 60, &sw);
        check("the jump a tick after the switch shows is found", sw.moved && apart(sw.jump, j) < 0.005);
        Tracker u;
        run(u, j, 39, 40, 60, &sw);
        check("...and a tick before", sw.moved && apart(sw.jump, j) < 0.005);
        Tracker v;
        run(v, j, 35, 40, 60, &sw);
        check("...and when the universe shows 5 ticks late, with its lag", sw.moved && apart(sw.jump, j) < 0.005 && sw.lag == 5);
        Tracker w;
        run(w, j, 34, 40, 60, &sw);
        check("...6 ticks late, the window's edge", sw.moved && apart(sw.jump, j) < 0.005 && sw.lag == 6);
        Tracker x;
        run(x, j, 33, 40, 60, &sw);
        check("a jump 7 ticks before the switch isn't taken for its own", sw.measured && !sw.moved);
        Tracker y;
        run(y, j, 46, 40, 60, &sw);
        check("...but one 6 ticks after it is, with a negative lag", sw.moved && apart(sw.jump, j) < 0.005 && sw.lag == -6);
        Tracker z;
        run(z, j, 47, 40, 60, &sw);
        check("...and one 7 ticks after it isn't", sw.measured && !sw.moved);
    }
    {
        // the head seen for only 2 steps around it: no motion to take off, the jump still found
        Tracker t;
        Switch sw;
        int n = 0;
        for (long k = 0; k < 60; ++k) {
            Pose h = head(0.1, 1.07, -0.2, 10, -20);
            if (k >= 40) h = Mul(j, h);
            n += t.Tick(k, k < 40 ? kGuess : kRoom, k >= 39 && k <= 41, h, &sw);
        }
        check("under three steps of head: the jump still measured", n == 1 && sw.moved && apart(sw.jump, j) < 0.005);
    }
    {
        Tracker t;
        Switch sw;
        run(t, Identity(), 40, 40, 60, &sw);
        check("a switch with no jump (standing didn't change) moves nothing", sw.measured && !sw.moved);
        Tracker u;
        run(u, Identity(), 40, 40, 60, &sw, 3.5);
        check("a fast head turn isn't a jump", sw.measured && !sw.moved);
    }
    {
        Tracker t;
        Switch sw;
        run(t, j, 40, 40, 60, &sw, 0.5, 30, 50);
        check("no head pose around the switch: not measured", !sw.measured && !sw.moved);
    }
    {
        Tracker t;
        Switch sw;
        const Pose h = head(0, 1, 0, 0);
        int n = 0;
        for (long k = 0; k < 20; ++k) {
            const uint64_t u = k < 5 ? kRoom : k < 7 ? kDummyUniverse : kGuess;
            n += t.Tick(k, u, true, h, &sw);
        }
        check("two switches before the first settles: one, from the first to the last",
              n == 1 && sw.from == kRoom && sw.to == kGuess);
        bool none = !t.Tick(20, 0, true, h, &sw);   // (and the same one after it: no switch either)
        for (long k = 21; k < 40; ++k) none = none && !t.Tick(k, kGuess, true, h, &sw);
        check("an unread universe (0) isn't a switch", none);
    }

    // which panels follow
    Switch back;
    back.from = kGuess, back.to = kRoom;
    check("placed in the temporary universe: follows the restore", Follows(kGuess, back, Kind::Temporary));
    check("placed in the room: stays", !Follows(kRoom, back, Kind::Temporary));
    check("placed before the universe was known: stays", !Follows(0, back, Kind::Temporary));
    Switch away;
    away.from = kRoom, away.to = kGuess;
    check("placed in the room, the room lost: stays", !Follows(kRoom, away, Kind::Room));
    check("leaving a universe of unknown kind: stays", !Follows(kGuess, back, Kind::Unknown));

    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
