// Unit tests for typing on a real keyboard (pointer/helper/typing.h): made-up keys and hands at
// ft-hands' 30 Hz, the head at eye height facing -z. The hands sit where the 2026-10-10 headset
// session and the hand recorder's desk take had them (docs/hands-typing.md).
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/typing-test.cpp).
#include "pointer/helper/typing.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace typing;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

namespace {

constexpr double kFrame = 1 / 30.0;
constexpr double kEye = 1.2;

// Palms: on the keyboard (today's first flag: 0.31 m under the eyes, 0.30 ahead, 0.17 apart),
// on the mouse (the desk take: 0.39 under, 0.27 ahead, 0.39 from the other hand), raised to point
// (0.15 under, 0.40 ahead, palm to the side), in the lap.
Hand Keys(int side) { return {true, {side ? 0.085 : -0.085, kEye - 0.31, -0.30}, 0.97}; }
Hand Mouse() { return {true, {0.30, kEye - 0.39, -0.27}, 0.85}; }
Hand Pointing(int side) { return {true, {side ? 0.18 : -0.18, kEye - 0.15, -0.40}, 0.3}; }
Hand Lap(int side) { return {true, {side ? 0.15 : -0.15, kEye - 0.55, -0.05}, 0.5}; }
Hand Unseen() { return {}; }

struct World {
    Typing ty;
    double t = 1000;
    Head head{{0, kEye, 0}, {0, 0, -1}};
    Hand hands[2] = {Unseen(), Unseen()};
    std::string log;
    explicit World(Params p = {}) : ty(p) {}
    void Step() {
        ty.Frame(t, head, hands);
        for (const std::string &l : ty.log) log += l + "\n";
        ty.log.clear();
        t += kFrame;
    }
    // Runs secs of frames; each: called before each frame; keyEvery: a key that often (0: none).
    void Run(double secs, double keyEvery = 0, const std::function<void()> &each = {}) {
        const double end = t + secs - 1e-9;
        double nextKey = t;
        while (t < end) {
            if (keyEvery > 0 && t >= nextKey) ty.Key(t), nextKey += keyEvery;
            if (each) each();
            Step();
        }
    }
    bool Blocked(int s) const { return ty.Blocked(s, t); }
};

// No hands seen: a key holds every hand back for POINTER_PINCH_TYPING (2 s), then nothing.
void TestKeysAlone() {
    World w;
    CHECK(!w.Blocked(0) && !w.Blocked(1), "no keys: nothing held back");
    w.ty.Key(w.t);
    w.Run(1.9);
    CHECK(w.Blocked(0) && w.Blocked(1), "1.9 s after a key: held back");
    CHECK(std::string(w.ty.Why(1, w.t)) == "typing", "why: %s", w.ty.Why(1, w.t));
    w.Run(0.2);
    CHECK(!w.Blocked(0) && !w.Blocked(1), "2.1 s after: not");
}

// The live complaint (14:57:10-30): typing with pauses of a few seconds, the hands on the keys all
// along. The old 1 s guard let the pinches in the pauses through; on the keyboard they stay held back.
void TestPausesKeepTheHandsParked() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(0.1);
    CHECK(!w.Blocked(1), "hands on the keys, no key yet (and none for a while): free");
    w.Run(3, 0.2);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1), "typing: both hands on the keyboard");
    bool always = true;
    w.Run(10, 0, [&] { always = always && w.Blocked(0) && w.Blocked(1); });
    CHECK(always, "a 10 s pause with the hands on the keys: held back throughout");
    CHECK(std::string(w.ty.Why(0, w.t)) == "typing: the hand is on the keyboard", "why: %s", w.ty.Why(0, w.t));
    CHECK(w.log.find("left hand on the keyboard") != std::string::npos && w.log.find("right hand on the keyboard") != std::string::npos,
          "logged: %s", w.log.c_str());
    // small movements on the keys (reaching for a key, 3 cm up and forward) don't free it
    w.Run(2, 0.25, [&] {
        const double k = std::sin(w.t * 9);
        w.hands[1].palm.y = Keys(1).palm.y + 0.03 * k, w.hands[1].palm.z = Keys(1).palm.z - 0.04 * std::fabs(k);
    });
    CHECK(w.ty.Parked(1), "typing movements keep it on the keyboard");
    // a single wild frame (a misread hand 15 cm up, palm flipped) doesn't free it either
    w.hands[1] = Keys(1), w.hands[1].palm.y += 0.15, w.hands[1].down = -0.5;
    w.Step();
    w.hands[1] = Keys(1);
    w.Run(1);
    CHECK(w.ty.Parked(1), "one misread frame: still on the keyboard");
}

// Lifting a hand off the keyboard: a deliberate pinch 0.5 s after it began to rise works, even with
// the other hand still typing; the typing hand stays held back.
void TestRaiseFreesAtOnce() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(2, 0.2);
    const double from = w.t;
    const Hand a = Keys(1), b = Pointing(1);
    w.Run(0.5, 0.2, [&] {
        const double f = std::clamp((w.t - from) / 0.25, 0.0, 1.0);   // up in 0.25 s
        w.hands[1].palm = {a.palm.x + (b.palm.x - a.palm.x) * f, a.palm.y + (b.palm.y - a.palm.y) * f, a.palm.z + (b.palm.z - a.palm.z) * f};
        w.hands[1].down = a.down + (b.down - a.down) * f;
    });
    CHECK(!w.Blocked(1), "raised 0.5 s ago, keys still coming from the other hand: free");
    CHECK(w.Blocked(0), "the typing hand: held back");
    CHECK(w.log.find("right hand off the keyboard (raised") != std::string::npos, "logged: %s", w.log.c_str());
    // the freed hand stays free while it's up, keys or not
    w.Run(3, 0.2);
    CHECK(!w.Blocked(1) && w.Blocked(0), "up while the other types: free");
    // when it was freed: within 0.2 s of starting up (8 cm of a 16 cm raise in 0.25 s, then 0.1 s)
    World v;
    v.hands[0] = Keys(0), v.hands[1] = Keys(1);
    v.Run(2, 0.2);
    const double start = v.t;
    double freedAt = -1;
    v.Run(1, 0.2, [&] {
        const double f = std::clamp((v.t - start) / 0.25, 0.0, 1.0);
        v.hands[1].palm.y = a.palm.y + (b.palm.y - a.palm.y) * f;
        if (freedAt < 0 && !v.Blocked(1)) freedAt = v.t;
    });
    CHECK(freedAt > 0 && freedAt - start <= 0.3, "free %.2f s after starting up", freedAt - start);
}

// Back down on the keys: on the keyboard again once keys come, or the posture holds.
void TestBackDown() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(1, 0.2);
    w.hands[1] = Pointing(1);
    w.Run(0.5);
    CHECK(!w.ty.Parked(1) && w.ty.Free(1), "raised: free");
    w.hands[1] = Keys(1);
    w.Run(0.2, 0.1);
    CHECK(w.ty.Parked(1) && w.Blocked(1), "back down while typing: on the keyboard");
    // up again, and down after the keys stopped: the posture puts it back within 0.3 s
    w.hands[1] = Pointing(1);
    w.Run(3);
    CHECK(!w.Blocked(1), "up, no keys for 3 s: free");
    w.hands[1] = Keys(1);
    w.Run(0.2);
    CHECK(!w.Blocked(1), "just back down, no key: not yet");
    w.Run(0.3);
    CHECK(w.ty.Posture() && w.Blocked(1), "both hands in the posture 0.3 s, a key this session: held back");
}

// The posture before the first key: only within POINTER_TYPING_RECENT of a key (a keyboard in use).
void TestPosture() {
    World never;
    never.hands[0] = Keys(0), never.hands[1] = Keys(1);
    never.Run(5);
    CHECK(!never.Blocked(0) && !never.Blocked(1), "no key ever: the posture alone holds nothing back");
    World w;
    w.ty.Key(w.t);
    w.Run(60);
    CHECK(!w.Blocked(1), "a key a minute ago, hands away: free");
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(0.2);
    CHECK(!w.Blocked(1), "hands just down: not yet");
    w.Run(0.2);
    CHECK(w.Blocked(0) && w.Blocked(1), "0.4 s in the posture: the first keystroke's pinch is held back");
    CHECK(w.log.find("keyboard posture") != std::string::npos, "logged: %s", w.log.c_str());
    // today's second flag: hands lower and nearer (0.41 under the eyes, 0.20 ahead, 0.15 apart)
    World low;
    low.ty.Key(low.t);
    low.Run(5);
    low.hands[0] = {true, {-0.075, kEye - 0.41, -0.20}, 0.87}, low.hands[1] = {true, {0.075, kEye - 0.42, -0.20}, 0.96};
    low.Run(0.4);
    CHECK(low.Blocked(0) && low.Blocked(1), "lower hands, nearer: the posture too");
    // POINTER_TYPING_RECENT=0: always; POINTER_TYPING_POSTURE=0: keys only
    Params always;
    always.recent = 0;
    World a(always);
    a.hands[0] = Keys(0), a.hands[1] = Keys(1);
    a.Run(0.4);
    CHECK(a.Blocked(1), "recent 0: the posture without any key");
    Params off;
    off.posture = 0;
    World o(off);
    o.ty.Key(o.t);
    o.Run(5);
    o.hands[0] = Keys(0), o.hands[1] = Keys(1);
    o.Run(2);
    CHECK(!o.Blocked(1), "posture 0: keys only");
}

// What isn't the posture: one hand low alone (a gaze pinch with the elbow resting, the other hand in
// the lap), a hand on the mouse beside the keyboard hand, both hands up pointing.
void TestNotThePosture() {
    World w;
    w.ty.Key(w.t);
    w.Run(5);
    w.hands[0] = Lap(0), w.hands[1] = {true, {0.15, kEye - 0.40, -0.30}, 0.9};
    w.Run(2);
    CHECK(!w.Blocked(1), "one hand low alone: free (%s)", w.ty.Why(1, w.t));
    w.hands[0] = Keys(0), w.hands[1] = Mouse();
    w.Run(2);
    CHECK(!w.Blocked(0) && !w.Blocked(1), "keyboard hand and mouse hand, no key: free");
    w.hands[0] = Pointing(0), w.hands[1] = Pointing(1);
    w.Run(2);
    CHECK(!w.Blocked(0) && !w.Blocked(1), "both up: free");
}

// A mouse user with one hand on the keyboard: a key (a shortcut) puts both on the keyboard, the mouse
// hand too (its pinches are the mouse's buttons); lifting the mouse hand frees it.
void TestMouseUser() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Mouse();
    w.Run(1);
    w.ty.Key(w.t);
    w.Run(0.1);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1), "a shortcut: the keyboard hand and the mouse hand are on the keyboard");
    w.Run(20, 0, [&] { w.hands[1].palm.x = Mouse().palm.x + 0.05 * std::sin(w.t); });   // mousing
    CHECK(w.Blocked(1), "mousing 20 s on: still held back");
    w.hands[1] = Pointing(1);
    w.Run(0.3);
    CHECK(!w.Blocked(1) && w.Blocked(0), "the mouse hand raised: free; the keyboard hand: not");
}

// Lost hands: on the keyboard, it stays there out of sight (looking up at a screen), up to
// POINTER_TYPING_KEEP; back raised, it's free. A free hand unseen 3 s is forgotten.
void TestLost() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(1, 0.2);
    w.hands[1] = Unseen();
    w.Run(2);
    w.hands[1] = Keys(1);
    w.Step();
    CHECK(w.ty.Parked(1), "unseen 2 s, back where it typed: still on the keyboard");
    // the 2026-10-28 complaint: looked up for 16 s, then down at the hands
    w.hands[0] = Unseen(), w.hands[1] = Unseen();
    w.Run(16);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1) && w.Blocked(1), "unseen 16 s: still on the keyboard");
    CHECK(w.log.find("forgotten") == std::string::npos, "not forgotten: %s", w.log.c_str());
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(1);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1), "seen again where they typed: on the keyboard");
    // past POINTER_TYPING_KEEP: forgotten
    w.hands[1] = Unseen();
    w.Run(301);
    CHECK(!w.ty.Parked(1) && !w.Blocked(1), "unseen past keep: forgotten");
    CHECK(w.log.find("right hand forgotten (unseen)") != std::string::npos, "logged: %s", w.log.c_str());
    // back up where it was raised: free
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(0.5, 0.2);
    w.hands[1] = Unseen();
    w.Run(10);
    w.hands[1] = Pointing(1);
    w.Run(0.2);
    CHECK(!w.Blocked(1), "lost on the keys, found raised: free");
    CHECK(w.log.find("right hand off the keyboard (raised") != std::string::npos, "left the keyboard: %s", w.log.c_str());
    // a free hand: forgotten after 3 s, as before
    w.hands[1] = Unseen();
    w.Run(3.2);
    CHECK(!w.ty.Free(1), "free and unseen 3 s: forgotten");
    // keep 0: as forget
    Params p;
    p.keep = 0;
    World v(p);
    v.hands[0] = Keys(0), v.hands[1] = Keys(1);
    v.Run(1, 0.2);
    v.hands[1] = Unseen();
    v.Run(3.2);
    CHECK(!v.ty.Parked(1), "POINTER_TYPING_KEEP=0: forgotten after 3 s");
}

// The mouse in use, no keys for a long time (the 2026-10-28 log): the hand on the mouse is at the
// desk, so it raises no ray; lifted, it's free; back down, at the desk again; long after the mouse,
// a hand coming down there is free.
void TestMouseParks() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Mouse();
    w.Run(1);
    CHECK(!w.ty.Parked(0) && !w.ty.Parked(1), "no keys, no mouse: free");
    w.Run(3, 0, [&] {
        w.ty.Mouse(w.t);
        w.hands[1].palm.x = Mouse().palm.x + 0.05 * std::sin(w.t);
    });
    CHECK(w.ty.Parked(1) && w.Blocked(1), "mousing: the mouse hand is at the desk");
    CHECK(w.ty.Parked(0), "the other hand resting by the keys: at the desk too");
    CHECK(std::string(w.ty.Why(1, w.t)) == "typing: the hand is at the desk (the mouse in use)", "why: %s", w.ty.Why(1, w.t));
    CHECK(w.log.find("right hand at the desk (the mouse in use)") != std::string::npos, "logged: %s", w.log.c_str());
    // the mouse still for a minute: still parked (the leave test, not the mouse's time, frees it)
    w.Run(60);
    CHECK(w.ty.Parked(1), "the mouse idle 60 s, the hand on it: still at the desk");
    // looking up and back down with the hand on the still mouse
    w.hands[0] = Unseen(), w.hands[1] = Unseen();
    w.Run(20);
    w.hands[0] = Keys(0), w.hands[1] = Mouse();
    w.Run(1);
    CHECK(w.ty.Parked(1) && w.Blocked(1), "looked up 20 s and back down: still at the desk");
    // lifted to point: free
    w.hands[1] = Pointing(1);
    w.Run(0.3);
    CHECK(!w.Blocked(1), "the mouse hand raised: free");
    // back down on the mouse, used: at the desk again
    w.hands[1] = Mouse();
    w.Run(0.5, 0, [&] { w.ty.Mouse(w.t); });
    CHECK(w.ty.Parked(1), "back on the mouse: at the desk");
    // a hand that comes down long after the mouse was used: free
    World v;
    v.ty.Mouse(v.t);
    v.Run(11);
    v.hands[1] = Mouse();
    v.Run(1);
    CHECK(!v.ty.Parked(1), "the mouse used 11 s before the hand came down: free");
    // the mouse used while the hands were out of sight: parked when they come into view
    World u;
    u.Run(5, 0, [&] { u.ty.Mouse(u.t); });
    u.hands[1] = Mouse();
    u.Run(0.2);
    CHECK(u.ty.Parked(1), "mousing unseen, then seen on the mouse: at the desk");
}

// A gaze pinch from a hand resting low (TestNotThePosture's), after a mouse move: no pinch while the
// mouse is in use, then pinches again; no ray until it's lifted. Keys make it a hand on the keyboard.
void TestDeskPinchesAfterTheMouse() {
    World w;
    w.ty.Mouse(w.t);
    w.hands[1] = {true, {0.15, kEye - 0.40, -0.30}, 0.9};
    w.Run(2);
    CHECK(w.ty.Parked(1) && w.ty.Why(1, w.t), "low, 2 s after the mouse: no pinch");
    CHECK(!w.ty.OnKeyboard(1), "at the desk, not on the keyboard (a gesture it holds carries on)");
    w.Run(9);
    CHECK(!w.ty.Why(1, w.t), "the mouse still 11 s: pinches again (%s)", w.ty.Why(1, w.t));
    CHECK(w.Blocked(1), "...but no ray until it's lifted");
    w.ty.Key(w.t);
    w.Run(0.2);
    CHECK(std::string(w.ty.Why(1, w.t)) == "typing: the hand is on the keyboard", "a key: on the keyboard (%s)", w.ty.Why(1, w.t));
    CHECK(w.ty.OnKeyboard(1), "a key: on the keyboard");
    w.Run(15);
    CHECK(w.ty.Why(1, w.t), "on the keyboard, keys long gone: still held back");
}

// The mouse doesn't park a hand that's pointing or holding a gesture, or one up in the air.
void TestMouseLeavesPointingAlone() {
    World w;
    w.hands[0] = Lap(0), w.hands[1] = Pointing(1);
    w.hands[1].busy = true;
    w.Run(2, 0, [&] { w.ty.Mouse(w.t); });
    CHECK(!w.Blocked(1), "the pointing hand while the other mouses: free (%s)", w.ty.Why(1, w.t));
    // a hand holding a gesture down in the zone
    World g;
    g.hands[1] = Keys(1);
    g.hands[1].busy = true;
    g.Run(1, 0, [&] { g.ty.Mouse(g.t); });
    CHECK(!g.ty.Parked(1), "holding a gesture in the zone while the mouse moves: not parked");
    w.hands[1].busy = false;
    w.Run(1, 0, [&] { w.ty.Mouse(w.t); });
    CHECK(!w.Blocked(1), "raised, not in the zone: free");
    CHECK(!w.ty.Parked(0), "in the lap (out of the zone): free");
    // POINTER_TYPING_MOUSE=0: off
    Params p;
    p.mouse = 0;
    World v(p);
    v.hands[1] = Mouse();
    v.Run(2, 0, [&] { v.ty.Mouse(v.t); });
    CHECK(!v.ty.Parked(1), "POINTER_TYPING_MOUSE=0: the mouse parks nothing");
}

// The desk model: a hand it puts at the keyboard or mouse (P 0.8 or more for 0.3 s) coming down in the
// zone is at the desk with no key or mouse for minutes: no ray or pokes, but its pinches work (a gaze
// pinch from a resting hand). Under the threshold, too briefly, out of the zone or busy: not parked.
// Lifted, it leaves as any parked hand.
void TestDeskModelParks() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Mouse();
    w.hands[0].desk = w.hands[1].desk = 0.95;
    w.Run(0.2);
    CHECK(!w.ty.Parked(1), "0.2 s: not yet");
    w.Run(0.2);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1), "0.4 s at the desk by the model: parked");
    CHECK(w.Blocked(1), "no ray or pokes");
    CHECK(w.ty.Why(1, w.t) == nullptr, "pinches work with the mouse still: %s", w.ty.Why(1, w.t));
    CHECK(w.log.find("right hand at the desk (the desk model, 0.95)") != std::string::npos, "logged: %s", w.log.c_str());
    w.ty.Mouse(w.t);
    CHECK(w.ty.Why(1, w.t) != nullptr, "the mouse in use: pinches held back as for the mouse's own park");
    w.hands[1].palm.y += 0.12;
    w.Run(0.3);
    CHECK(!w.ty.Parked(1) && w.ty.Free(1), "lifted: free");
    // under the threshold, flickering, out of the zone, or in use: never parked
    World u;
    u.hands[1] = Mouse(), u.hands[1].desk = 0.7;
    u.Run(1);
    CHECK(!u.ty.Parked(1), "P 0.7 under 0.8: not parked");
    World f;
    f.hands[1] = Mouse();
    int k = 0;
    f.Run(2, 0, [&] { f.hands[1].desk = (k++ % 6) < 5 ? 0.9 : 0.5; });
    CHECK(!f.ty.Parked(1), "never 0.3 s running: not parked");
    World l;
    l.hands[0] = Lap(0), l.hands[0].desk = 0.99;
    l.Run(1);
    CHECK(!l.ty.Parked(0), "the model alone doesn't park a hand out of the zone");
    World b;
    b.hands[1] = Keys(1), b.hands[1].desk = 0.99, b.hands[1].busy = true;
    b.Run(1);
    CHECK(!b.ty.Parked(1), "a hand holding a gesture: not parked");
    World n;
    n.hands[1] = Keys(1);
    n.Run(2);
    CHECK(!n.ty.Parked(1), "no model (-1): not parked without keys or the mouse");
    // exactly the threshold parks
    World e;
    e.hands[1] = Mouse(), e.hands[1].desk = 0.8;
    e.Run(0.4);
    CHECK(e.ty.Parked(1), "P 0.8: parked");
    // a hand lost for a frame starts its 0.3 s over
    World g;
    g.hands[1] = Mouse(), g.hands[1].desk = 0.95;
    g.Run(0.2);
    Hand seen = g.hands[1];
    g.hands[1] = Unseen();
    g.Step();
    g.hands[1] = seen;
    g.Run(0.2);
    CHECK(!g.ty.Parked(1), "0.2 s after a lost frame: not yet");
    g.Run(0.15);
    CHECK(g.ty.Parked(1), "0.35 s after it: parked");
    // a hand that leaves the zone for a frame starts over too
    World z;
    z.hands[1] = Mouse(), z.hands[1].desk = 0.95;
    z.Run(0.2);
    z.hands[1].palm.y += 0.3;
    z.Step();
    z.hands[1].palm.y -= 0.3;
    z.Run(0.2);
    CHECK(!z.ty.Parked(1), "0.2 s after leaving the zone: not yet");
    // lifted, then put down again (no mouse in use): the model parks it again
    World r;
    r.hands[1] = Mouse(), r.hands[1].desk = 0.95;
    r.Run(0.4);
    r.hands[1].palm.y += 0.12, r.hands[1].desk = 0.1;
    r.Run(0.3);
    CHECK(!r.ty.Parked(1), "lifted: not parked");
    r.hands[1].palm.y -= 0.12, r.hands[1].desk = 0.95;
    r.Run(0.4);
    CHECK(r.ty.Parked(1), "back at the desk: parked again");
    size_t times = 0;
    for (size_t at = r.log.find("(the desk model"); at != std::string::npos; at = r.log.find("(the desk model", at + 1)) ++times;
    CHECK(times == 2, "parked by the model twice: %s", r.log.c_str());
}

// Pointing with one hand (its ray up) and pressing Enter with the other: the pointing hand isn't held
// back. The same hand idle where it was (not pointing): the key holds it back for 2 s.
void TestPointAndEnter() {
    World w;
    w.hands[0] = Lap(0), w.hands[1] = Pointing(1);
    w.hands[1].busy = true;
    w.Run(1);
    w.ty.Key(w.t);
    w.Run(0.5);
    CHECK(!w.Blocked(1), "the pointing hand: free (%s)", w.ty.Why(1, w.t));
    CHECK(w.Blocked(0), "the hand that pressed the key: held back");
    World v;
    v.hands[0] = Lap(0), v.hands[1] = Pointing(1);
    v.Run(1);
    v.ty.Key(v.t);
    v.Run(0.5);
    CHECK(v.Blocked(1), "a raised hand that isn't in use: held back by the key");
    // a hand on the keys whose ray came up (today, 14:57:37): on the keyboard all the same
    World k;
    k.hands[0] = Keys(0), k.hands[1] = Keys(1);
    k.hands[1].busy = true;
    k.Run(1, 0.2);
    CHECK(k.ty.Parked(1) && k.Blocked(1), "a typing hand with its ray up: on the keyboard");
}

// A hand holding a gesture (busy: a three-finger scroll, live 2026-10-18 16:4x) out of the zone is
// free; brought down onto the keys while the other hand types, it's on the keyboard, so the helper
// ends what it held (Parked).
void TestHoldOntoTheKeys() {
    World w;
    w.hands[0] = Pointing(0), w.hands[1] = Keys(1);
    w.hands[0].busy = true;
    w.Run(1, 0.2);
    CHECK(!w.ty.Parked(0) && !w.Blocked(0), "holding, raised, the other hand typing: free (%s)", w.ty.Why(0, w.t));
    w.hands[0] = Keys(0);
    w.hands[0].busy = true;
    w.Run(0.3, 0.2);
    CHECK(w.ty.Parked(0), "still holding, down on the keys: on the keyboard");
    CHECK(w.log.find("left hand on the keyboard") != std::string::npos, "logged: %s", w.log.c_str());
}

// A palm turned up (the palm menu's pose) leaves the keyboard too.
void TestTurned() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(1, 0.2);
    w.hands[1].down = -0.8;
    w.Run(0.2);
    CHECK(!w.Blocked(1), "palm turned up: free");
    CHECK(w.log.find("palm turned") != std::string::npos, "logged: %s", w.log.c_str());
}

void TestOff() {
    Params p;
    p.hold = 0;
    World w(p);
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(2, 0.2);
    CHECK(!w.Blocked(0) && !w.Blocked(1) && !w.ty.Parked(0), "POINTER_PINCH_TYPING=0: no guard");
}

// Times below zero (a replay's, relative to a flag): the same.
void TestNegativeClock() {
    World w;
    w.t = -16;
    w.ty.Key(w.t - 60);
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(0.4);
    CHECK(w.ty.Posture() && w.Blocked(1), "the posture at t < 0");
    w.Run(1, 0.2);
    w.hands[1] = Pointing(1);
    w.Run(0.3);
    CHECK(!w.Blocked(1), "raised at t < 0: free");
}

// The head turned (facing +x): ahead counts along its heading.
void TestHeading() {
    World w;
    w.head.fwd = {1, -0.5, 0};
    w.hands[0] = {true, {0.30, kEye - 0.31, -0.085}, 0.97}, w.hands[1] = {true, {0.30, kEye - 0.31, 0.085}, 0.97};
    w.Run(1, 0.2);
    CHECK(w.ty.Parked(0) && w.ty.Parked(1), "facing +x, the keyboard 0.3 m that way: on it");
    World v;
    v.head.fwd = {-1, 0, 0};
    v.hands[0] = {true, {0.30, kEye - 0.31, -0.085}, 0.97}, v.hands[1] = {true, {0.30, kEye - 0.31, 0.085}, 0.97};
    v.Run(1, 0.2);
    CHECK(!v.ty.Parked(0) && !v.ty.Parked(1), "behind the head: not");
}

}  // namespace

int main() {
    TestKeysAlone();
    TestPausesKeepTheHandsParked();
    TestRaiseFreesAtOnce();
    TestBackDown();
    TestPosture();
    TestNotThePosture();
    TestMouseUser();
    TestLost();
    TestMouseParks();
    TestDeskPinchesAfterTheMouse();
    TestMouseLeavesPointingAlone();
    TestDeskModelParks();
    TestPointAndEnter();
    TestHoldOntoTheKeys();
    TestTurned();
    TestOff();
    TestHeading();
    TestNegativeClock();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("typing-test: ok\n");
    return 0;
}
