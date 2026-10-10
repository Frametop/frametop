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

// Lost hands: through a short loss it stays on the keyboard; unseen 3 s, forgotten.
void TestLost() {
    World w;
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(1, 0.2);
    w.hands[1] = Unseen();
    w.Run(2);
    w.hands[1] = Keys(1);
    w.Step();
    CHECK(w.ty.Parked(1), "unseen 2 s, back where it typed: still on the keyboard");
    w.hands[1] = Unseen();
    w.Run(3.2);
    CHECK(!w.ty.Parked(1) && !w.Blocked(1), "unseen 3 s: forgotten");
    // back up where it was raised: free
    w.hands[0] = Keys(0), w.hands[1] = Keys(1);
    w.Run(0.5, 0.2);
    w.hands[1] = Unseen();
    w.Run(1);
    w.hands[1] = Pointing(1);
    w.Run(0.2);
    CHECK(!w.Blocked(1), "lost on the keys, found raised: free");
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
    TestPointAndEnter();
    TestTurned();
    TestOff();
    TestHeading();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("typing-test: ok\n");
    return 0;
}
