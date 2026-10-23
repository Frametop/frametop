// Touch typing's geometry and poke states (src/touch.cpp), without SteamVR.
#include "framekeyboard/panel.hpp"
#include "framekeyboard/touch.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace framekeyboard;
using Kind = TouchEvent::Kind;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}
Transform at(double x, double y, double z) {
    return {{{1, 0, 0, x}, {0, 1, 0, y}, {0, 0, 1, z}}};
}
const Rect surface{100, 100, 1400, 400};

// A tip sampled every `step` seconds (vr.cpp reads it every 4 ms while it's near).
struct Tip {
    TouchPoke poke;
    double now = 10, step = .004, x = 800, y = 300;
    std::vector<Kind> kinds;
    TouchEvent at(std::optional<double> depth) {
        now += step;
        const auto e =
            poke.update(depth ? std::optional<PanelPoint>{{x, y, *depth}} : std::nullopt, surface, now);
        if (e.kind != Kind::None && e.kind != Kind::Hover && e.kind != Kind::Move) {
            kinds.push_back(e.kind);
        }
        return e;
    }
    // From `from` to `to` metres out at `speed` metres a second.
    void go(double from, double to, double speed) {
        const int n = std::max(1, static_cast<int>(std::fabs(to - from) / (speed * step)));
        for (int i = 1; i <= n; ++i) {
            at(from + (to - from) * i / n);
        }
    }
    void hold(double depth, double seconds, double jitter = 0) {
        for (int i = 0; i * step < seconds; ++i) {
            at(depth + (i % 2 ? jitter : -jitter));
        }
    }
    int count(Kind kind) const { return static_cast<int>(std::count(kinds.begin(), kinds.end(), kind)); }
};

void geometry() {
    // 1.6 m wide: 1000 pixels a metre. The centre is (800, 500) in OpenVR's bottom-up mouse
    // pixels, so y 300 once flipped and the popup margin taken off.
    auto p = locate_on_panel(at(0, 1, -1), 1.6, {0, 1, -.97});
    require(near(p.x, 800) && near(p.y, 300) && near(p.depth, .03), "the centre, 3 cm out");
    p = locate_on_panel(at(0, 1, -1), 1.6, {.1, 1.1, -1});
    require(near(p.x, 900) && near(p.y, 200) && near(p.depth, 0), "right and up: x grows, y shrinks");
    // Turned to face +x (its right is -z, its front +x): a point 2 cm along +x is 2 cm out.
    const Transform turned{{{0, 0, 1, 2}, {0, 1, 0, 1}, {-1, 0, 0, 0}}};
    p = locate_on_panel(turned, .8, {2.02, 1, -.1});
    require(near(p.depth, .02) && near(p.x, 1000), "a turned panel: 10 cm to its right is 200 px");
    // The tip: 5 mm past the component's origin along its -z, carried by the device's pose.
    const auto tip = tip_point(at(1, 2, 3), at(0, -.02, .02), .005);
    require(near(tip[0], 1) && near(tip[1], 1.98) && near(tip[2], 3.015), "tip point");
}

void poke() {
    Tip t;
    require(t.at(.08).kind == Kind::None, "too far out: nothing");
    require(t.at(.05).kind == Kind::Hover && t.poke.near(), "hovers within 6 cm");
    t.go(.05, .001, .2);
    require(!t.poke.pressed() && t.kinds.empty(), "not yet at the surface");
    t.x = 640;
    const auto e = t.at(-.0005);
    require(e.kind == Kind::Down && t.poke.pressed() && near(e.x, 640), "presses at the surface");
    t.go(-.0005, -.02, .2);
    require(t.poke.pressed(), "held while pushing through");
    t.go(-.02, -.013, .2);
    require(t.poke.pressed(), "held: 7 mm back from the deepest");
    t.go(-.013, -.011, .2);
    require(!t.poke.pressed() && t.count(Kind::Up) == 1, "up 8 mm back, still behind the surface");
    t.go(-.011, -.015, .2);
    require(!t.poke.pressed(), "4 mm in again: not a press");
    t.go(-.015, -.018, .2);
    require(t.poke.pressed() && t.count(Kind::Down) == 2, "6 mm in again: the next key");
    t.go(-.018, .03, .3);
    require(!t.poke.pressed() && t.count(Kind::Up) == 2 && t.poke.near(), "out in front: up, hovering");
}

void quick_taps() {
    // Ten fast taps, 2 cm out to 1.5 cm through at 0.6 m/s: ten keys, no more, no fewer.
    Tip t;
    t.go(.05, .02, .3);
    for (int i = 0; i < 10; ++i) {
        t.go(.02, -.015, .6);
        t.go(-.015, .02, .6);
    }
    require(t.count(Kind::Down) == 10 && t.count(Kind::Up) == 10, "ten quick taps, ten keys");
    // Shallow taps that come back only 1 cm, never out in front: still each a key.
    Tip s;
    s.go(.03, -.01, .3);
    for (int i = 0; i < 5; ++i) {
        s.go(-.01, -.001, .15);
        s.go(-.001, -.01, .15);
    }
    require(s.count(Kind::Down) == 6, "taps that stay behind the surface each press");
    // Read only once a frame (60 Hz): the same taps still each press once.
    Tip f;
    f.step = 1 / 60.0;
    f.go(.05, .02, .3);
    for (int i = 0; i < 5; ++i) {
        f.go(.02, -.015, .3);
        f.go(-.015, .02, .3);
    }
    require(f.count(Kind::Down) == 5 && f.count(Kind::Up) == 5, "at 60 Hz too");
}

void no_doubles() {
    // Held still through the surface with 1.5 mm of jitter: one press, no up.
    Tip t;
    t.go(.03, -.01, .3);
    t.hold(-.01, .3, .0015);
    require(t.count(Kind::Down) == 1 && t.count(Kind::Up) == 0, "jitter while held");
    // Lifted 9 mm and resting there with jitter: no second press.
    t.go(-.01, -.001, .3);
    require(t.count(Kind::Up) == 1, "lifted");
    t.hold(-.001, .3, .0015);
    require(t.count(Kind::Down) == 1, "jitter after the lift: no second press");
    // A bounce straight back in within the debounce: no second press.
    Tip b;
    b.go(.03, -.005, .5);
    b.go(-.005, -.014, .5);
    b.go(-.014, -.004, 1.5);
    b.go(-.004, -.012, 1.5);
    require(b.count(Kind::Down) == 1, "a bounce within 60 ms is one press");
}

void creeping() {
    // Hovering 1 cm out and drifting in at 1 cm/s: no press as it crosses.
    Tip t;
    t.go(.04, .01, .2);
    t.go(.01, -.002, .01);
    require(t.count(Kind::Down) == 0 && t.poke.near(), "a slow drift through doesn't press");
    // A push from there does.
    t.go(-.002, -.01, .2);
    require(t.count(Kind::Down) == 1, "a push from behind the surface after a drift presses");
}

void no_press_without_approach() {
    Tip t;
    // In at 5 mm (from the side, or creeping in): not armed, so the surface doesn't press.
    t.at(.005);
    t.go(.005, -.001, .2);
    require(t.count(Kind::Down) == 0 && !t.poke.near(), "through without arming: no press");
    // From behind, then out in front: hover first, and a press from there.
    Tip b;
    b.at(-.02);
    b.go(-.02, -.001, .2);
    require(b.kinds.empty() && !b.poke.near(), "coming forward from behind");
    b.go(-.001, .02, .2);
    require(b.poke.near(), "out in front");
    b.go(.02, -.002, .2);
    require(b.count(Kind::Down) == 1, "a press from the front");
    // Off the case, behind, then slid over it: nothing.
    Tip o;
    o.x = 50;
    o.at(-.01);
    o.x = 800;
    o.at(-.01);
    require(o.kinds.empty(), "slid over it behind the surface");
}

void releases() {
    Tip t;
    t.go(.03, -.002, .3);
    require(t.poke.pressed(), "pressed");
    t.x += TouchPoke::drift - 1;
    t.at(-.002);
    require(t.poke.pressed(), "a little slide: held");
    t.x += 2;
    t.at(-.002);
    require(!t.poke.pressed() && t.count(Kind::Up) == 1, "slid too far from where it pressed: up");
    Tip e;
    e.x = 1500 - 10;
    e.go(.03, -.002, .3);
    e.x = 1500 + TouchPoke::slide + 1;
    e.at(-.002);
    require(!e.poke.pressed() && e.count(Kind::Up) == 1, "slid off the case: up");
    Tip d;
    d.go(.03, -.002, .3);
    d.at(-.13);
    require(!d.poke.pressed() && d.count(Kind::Up) == 1 && !d.poke.near(), "pushed far through: up");
    Tip l;
    l.go(.03, -.002, .3);
    require(l.at(std::nullopt).kind == Kind::Cancel && !l.poke.near(), "lost while pressed: cancel");
    l.go(.05, .03, .3);
    require(l.at(std::nullopt).kind == Kind::Leave, "lost while hovering: leave");
    require(l.at(std::nullopt).kind == Kind::None, "lost again: nothing");
    l.go(.05, .03, .3);
    l.x = 50;
    require(l.at(.03).kind == Kind::Leave, "hover off the case: leave");
}
} // namespace

int main() {
    try {
        geometry();
        poke();
        quick_taps();
        no_doubles();
        creeping();
        no_press_without_approach();
        releases();
    } catch (const std::exception& error) {
        std::cerr << "touch typing: " << error.what() << '\n';
        return 1;
    }
    std::cout << "touch typing: ok\n";
    return 0;
}
