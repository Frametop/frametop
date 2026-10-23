// Touch typing's geometry and poke states (src/touch.cpp), without SteamVR.
#include "framekeyboard/panel.hpp"
#include "framekeyboard/touch.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

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
// A point straight out from the panel's middle row, `depth` in front, at pixel x.
PanelPoint pt(double depth, double x = 800, double y = 300) {
    return {x, y, depth};
}

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
    TouchPoke t;
    require(t.update(pt(.08), surface).kind == Kind::None, "too far out: nothing");
    require(t.update(pt(.05), surface).kind == Kind::Hover && t.near(), "hovers within 6 cm");
    require(t.update(pt(.02), surface).kind == Kind::Hover, "still hovering");
    auto e = t.update(pt(0, 640, 250), surface);
    require(e.kind == Kind::Down && t.pressed() && near(e.x, 640) && near(e.y, 250),
            "presses at the surface");
    require(t.update(pt(-.01), surface).kind == Kind::Move, "held behind the surface");
    require(t.update(pt(.006), surface).kind == Kind::Move, "held until the release travel");
    require(t.update(pt(.013), surface).kind == Kind::Up && !t.pressed() && t.near(),
            "released out front");
    require(t.update(pt(0), surface).kind == Kind::Down, "the release's travel arms the next press");
    require(t.update(pt(.02), surface).kind == Kind::Up, "and up again");
}

void no_press_without_approach() {
    TouchPoke t;
    // In at 5 mm (from the side, or creeping in): not armed, so the surface doesn't press.
    require(t.update(pt(.005), surface).kind == Kind::Hover, "hover at 5 mm");
    require(t.update(pt(-.001), surface).kind == Kind::Leave && !t.near(),
            "through without arming: no press");
    require(t.update(pt(-.001), surface).kind == Kind::None, "behind: nothing");
    // From behind, then out in front: hover first, and a press from there.
    t.reset();
    require(t.update(pt(-.02), surface).kind == Kind::None, "behind the panel");
    require(t.update(pt(-.001), surface).kind == Kind::None, "coming forward from behind");
    require(t.update(pt(.02), surface).kind == Kind::Hover, "out in front");
    require(t.update(pt(-.002), surface).kind == Kind::Down, "a press from the front");
    // Off the case, behind, then slid over it: nothing.
    t.reset();
    require(t.update(pt(-.01, 50), surface).kind == Kind::None, "off the case");
    require(t.update(pt(-.01, 800), surface).kind == Kind::None, "slid over it behind the surface");
}

void releases() {
    TouchPoke t;
    t.update(pt(.03), surface);
    t.update(pt(0), surface);
    require(t.pressed(), "pressed");
    require(t.update(pt(-.01, 1500 + TouchPoke::slide - 1), surface).kind == Kind::Move,
            "a little off the edge: held");
    require(t.update(pt(-.01, 1500 + TouchPoke::slide + 1), surface).kind == Kind::Up && !t.near(),
            "slid off the case: up");
    t.update(pt(.03), surface);
    t.update(pt(0), surface);
    require(t.update(pt(-.13), surface).kind == Kind::Up && !t.near(), "pushed far through: up");
    require(t.update(pt(-.05), surface).kind == Kind::None, "and no hover behind");
    t.update(pt(.03), surface);
    t.update(pt(0), surface);
    require(t.update(std::nullopt, surface).kind == Kind::Cancel && !t.near(),
            "lost while pressed: cancel");
    t.update(pt(.03), surface);
    require(t.update(std::nullopt, surface).kind == Kind::Leave, "lost while hovering: leave");
    require(t.update(std::nullopt, surface).kind == Kind::None, "lost again: nothing");
    t.update(pt(.03), surface);
    require(t.update(pt(.03, 50), surface).kind == Kind::Leave, "hover off the case: leave");
}
} // namespace

int main() {
    try {
        geometry();
        poke();
        no_press_without_approach();
        releases();
    } catch (const std::exception& error) {
        std::cerr << "touch typing: " << error.what() << '\n';
        return 1;
    }
    std::cout << "touch typing: ok\n";
    return 0;
}
