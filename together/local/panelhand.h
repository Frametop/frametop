// The wearer's own hand on a panel someone else shares (docs/together.md, "Control and
// moving"). ft-screens tells the program that shows the panel what the pointer and the keyboard
// do on it, in the words it tells a remote display's (screens/remote.c):
//
//   move X Y        the pointer, in the picture's pixels
//   button CODE D   a mouse button (a Linux BTN_* code) down or up, where the pointer last was
//   scroll DX DY    the wheel, in notches to the right and down
//   key CODE D      a key, while the panel has the typing
//   blur            typing went elsewhere
//   leave           the pointer left the panel
//
// Here they become what ft-together's local socket takes (together/README.md): a pointer
// datagram for everyone in the room to see (proto/pointwire.h), and a line `control act ...`
// for the panel's owner (proto/control.h). Whether the wearer was given the panel isn't asked
// here: ft-together knows, and says no to an act for a panel that wasn't.
//
// What is kept here is what keeps the two ends in step:
//  - Moves go out sixty times a second at most. The last one waits and is sent by Tick, or
//    before the button or scroll that happens where it points.
//  - A pointer that rests on the panel is said again each second, so that whoever draws it can
//    tell one that rests from one whose program went; when it leaves, that is said once.
//  - A button or key goes down once and comes up only if it went down here. Keys come up when
//    typing goes elsewhere, buttons when the pointer leaves, and everything at the end.
//
// The rules only: no sockets and no clock here. Header only.
#pragma once

#include "../proto/control.h"
#include "../proto/pointwire.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace local {

class PanelHand {
public:
    static constexpr double kMoveS = 1.0 / 60;   // between two moves sent
    static constexpr double kStillS = 1.0;       // a resting pointer is said again this often
    static constexpr size_t kMostKeys = 8;       // keys held at once: its owner takes no more from one person (room/desk.h)

    using Out = std::vector<std::string>;        // datagrams for ft-together's local socket, in order

    explicit PanelHand(uint8_t panel) : panel_(panel) {}

    // The picture's size in the pixels ft-screens counts in. Until it is known nothing is sent.
    void Size(int width, int height) { width_ = width, height_ = height; }

    // One of ft-screens' words at `now` (seconds). False when it says nothing of the hand.
    bool Word(const std::string &msg, double now, Out &out) {
        double x = 0, y = 0;
        unsigned code = 0;
        int down = 0;
        if (std::sscanf(msg.c_str(), "move %lf %lf", &x, &y) == 2) Move(x, y, now, out);
        else if (std::sscanf(msg.c_str(), "button %u %d", &code, &down) == 2) Button(code, down != 0, now, out);
        else if (std::sscanf(msg.c_str(), "scroll %lf %lf", &x, &y) == 2) Scroll(x, y, now, out);
        else if (std::sscanf(msg.c_str(), "key %u %d", &code, &down) == 2) Key(code, down != 0, out);
        else if (msg == "blur") Blur(out);
        else if (msg == "leave") Leave(now, out);
        else return false;
        return true;
    }

    // Often: sends a move that waited, and says a resting pointer again.
    void Tick(double now, Out &out) {
        if (waits_ && now - moved_ >= kMoveS) Flush(now, out);
        else if (on_ && !waits_ && now - pointed_ >= kStillS) Point(now, out);
    }

    // Seconds until Tick has something to send; negative when it never will as things are.
    double Wait(double now) const {
        if (waits_) return std::max(0.0, kMoveS - (now - moved_));
        return on_ ? std::max(0.0, kStillS - (now - pointed_)) : -1;
    }

    // The program ends, or the panel goes: nothing stays down, and the pointer points at nothing.
    void End(double now, Out &out) {
        Blur(out);
        Leave(now, out);
    }

    bool on() const { return on_; }

private:
    // Pixel `v` of `size` as 0 to 65535 across the picture. (Not a number: 0.)
    static uint16_t Far(double v, int size) {
        v = v * 65536.0 / size;
        return !(v > 0) ? 0 : v >= proto::kActFar ? uint16_t(proto::kActFar) : uint16_t(v);
    }
    // Notches as 120ths, within what an act may say.
    static int Notches(double d) {
        if (!std::isfinite(d)) return 0;
        return int(std::lround(std::clamp(d * 120, -double(proto::kMostScroll), double(proto::kMostScroll))));
    }

    void Act(Out &out, const char *what, int a, int b) const {
        out.push_back("control act " + std::to_string(panel_) + " " + what + " " + std::to_string(a) + " " + std::to_string(b));
    }
    void Point(double now, Out &out) {
        proto::Point p;
        p.panel = on_ ? panel_ : proto::kNoPanel;
        p.x = x_, p.y = y_;
        p.flags = buttons_ ? proto::kPointPressed : 0;
        const std::vector<uint8_t> bytes = proto::Pack(p);
        out.emplace_back(bytes.begin(), bytes.end());
        pointed_ = now;
    }
    void Flush(double now, Out &out) {
        if (!waits_) return;
        waits_ = false, moved_ = now;
        Act(out, "move", x_, y_);
        Point(now, out);
    }

    void Move(double x, double y, double now, Out &out) {
        if (width_ <= 0 || height_ <= 0) return;
        x_ = Far(x, width_), y_ = Far(y, height_);
        on_ = waits_ = true;
        if (now - moved_ >= kMoveS) Flush(now, out);
    }
    void Button(unsigned code, bool down, double now, Out &out) {
        const int b = code == BTN_LEFT ? proto::kLeft : code == BTN_RIGHT ? proto::kRight : code == BTN_MIDDLE ? proto::kMiddle : 0;
        const uint8_t bit = uint8_t(1 << b);
        if (!b || down == bool(buttons_ & bit) || (down && !on_)) return;
        Flush(now, out);
        buttons_ = down ? buttons_ | bit : buttons_ & uint8_t(~bit);
        Act(out, "button", b, down);
        Point(now, out);
    }
    void Scroll(double dx, double dy, double now, Out &out) {
        const int right = Notches(dx), down = Notches(dy);
        if (!on_ || (!right && !down)) return;
        Flush(now, out);
        Act(out, "scroll", right, down);
    }
    void Key(unsigned code, bool down, Out &out) {
        if (code < 1 || code > unsigned(proto::kMostKey)) return;
        const auto at = std::find(keys_.begin(), keys_.end(), code);
        if (down ? at != keys_.end() || keys_.size() >= kMostKeys : at == keys_.end()) return;
        if (down) keys_.push_back(code);
        else keys_.erase(at);
        Act(out, "key", int(code), down);
    }
    void Blur(Out &out) {
        for (; !keys_.empty(); keys_.pop_back()) Act(out, "key", int(keys_.back()), 0);
    }
    void Leave(double now, Out &out) {
        if (!on_ && !buttons_) return;
        for (int b = proto::kMiddle; b >= proto::kLeft; --b)
            if (buttons_ & (1 << b)) Act(out, "button", b, 0);
        buttons_ = 0, on_ = waits_ = false;
        Point(now, out);
    }

    uint8_t panel_;
    int width_ = 0, height_ = 0;
    uint16_t x_ = 0, y_ = 0;          // where the pointer is, across and down the picture
    bool on_ = false;                 // it is on the panel
    bool waits_ = false;              // ...and where it is hasn't been sent yet
    double moved_ = -1e9, pointed_ = -1e9;   // when a move, and a pointer datagram, last went
    uint8_t buttons_ = 0;             // the buttons held, a bit each (1 << kLeft ...)
    std::vector<unsigned> keys_;      // the keys held, in the order they went down
};

}  // namespace local
