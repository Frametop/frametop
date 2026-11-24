// The pointer's ray on Full Keyboard's panel (org.framekeyboard.panel), forwarded to it.
//
// Outside SteamVR's dashboard Full Keyboard takes no SteamVR laser events (its own controller
// laser only), so the 3D mouse's and a hand's ray went through it to the panel behind (user,
// 2026-11-21: "you can't even interact with it unless you open the steam menu"). Its hosted
// protocol takes a forwarded pointer instead, as ft-kbhost already gives it for fingertips: so
// while the ray is on its panel, the pointer helper says where, as ft-screens says a fingertip's
// point ("kbpoke SIDE VERB [X Y Z]" to the input relay, the point in the standing universe), and a
// press there goes to it and not to SteamVR's laser. The same in and out of the dashboard.
//
// Forward is that policy, pure (pointer/tests/kbray-test.cpp): each call returns the line to send
// to the relay, or "" for none. ft-kbhost (keyboard/host/touch.py) puts the point on the panel,
// turns its own pointers off while one is forwarded, and lets go 0.6 s after the last word and a
// press 3 s after it: so a ray resting on it says so again every kKeepS.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace kbray {

constexpr const char *kPanel = "org.framekeyboard.panel";
constexpr double kKeepS = 0.25;    // a still ray says where it is again this often
constexpr double kStillM = 0.0005; // ...and a move under this isn't news

struct Point {
    double x = 0, y = 0, z = 0;
};

class Forward {
public:
    // Each frame: whether the ray is on the panel and where, for the hand `side` ("left"/"right").
    // While pressed it stays the press's, on the panel or off it (the keyboard cancels a press
    // outside its keys, as on a focus leave), until Release.
    std::string Aim(double now, bool on, const Point &p, const char *side) {
        if (pressed_) {
            if (on && Far(p)) at_ = p;
            else if (now - sentAt_ < kKeepS) return "";
            return Line(now, "move", true);
        }
        if (!on) {
            if (!hovering_) return "";
            hovering_ = false;
            return Line(now, "end", false);
        }
        if (hovering_ && side_ == side && !Far(p) && now - sentAt_ < kKeepS) return "";
        if (hovering_ && side_ != side) {   // (another hand's ray now: the old one's hover ends first)
            const std::string end = Line(now, "end", false);
            side_ = side, at_ = p;
            return end + "\n" + Line(now, "hover", true);
        }
        hovering_ = true, side_ = side, at_ = p;
        return Line(now, "hover", true);
    }

    // A press: the keyboard's when the ray is on its panel (then *line is set, and SteamVR's laser
    // isn't pressed); false otherwise.
    bool Press(double now, bool on, const Point &p, const char *side, std::string *line) {
        if (!on) return false;
        if (hovering_ && side_ != side) *line = Line(now, "end", false) + "\n";
        else line->clear();
        hovering_ = pressed_ = true, side_ = side, at_ = p;
        *line += Line(now, "press", true);
        return true;
    }

    // The press's release, if it was the keyboard's (then *line is set).
    bool Release(double now, std::string *line) {
        if (!pressed_) return false;
        pressed_ = false;
        *line = Line(now, "up", true);
        return true;
    }

    bool Pressed() const { return pressed_; }

private:
    bool hovering_ = false, pressed_ = false;
    std::string side_ = "right";
    Point at_;
    double sentAt_ = -1e9;

    bool Far(const Point &p) const {
        return std::fabs(p.x - at_.x) > kStillM || std::fabs(p.y - at_.y) > kStillM || std::fabs(p.z - at_.z) > kStillM;
    }
    std::string Line(double now, const char *verb, bool point) {
        sentAt_ = now;
        std::string s = "kbpoke " + side_ + " " + verb;
        if (point) {
            char b[96];   // (the relay takes up to 3 digits before the point and 6 after)
            std::snprintf(b, sizeof b, " %.4f %.4f %.4f", Clamp(at_.x), Clamp(at_.y), Clamp(at_.z));
            s += b;
        }
        return s;
    }
    static double Clamp(double v) { return std::isfinite(v) ? std::fmax(-999.0, std::fmin(999.0, v)) : 0.0; }
};

}  // namespace kbray
