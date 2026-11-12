// The wearer's pointer on someone else's panel while their eyes move it and they don't share
// where they look (gaze.h, `quiet`). A pointer that only moves would tell the room, and the
// panel's owner, where they look, all the time. So while it is quiet:
//
//  - Where they point isn't told to the room, except while they hold a button down there. When
//    the button comes up the room is told the pointer points at nothing, as it is when it
//    leaves the panel.
//  - A move isn't given to the panel's owner until something is done there: the last one waits,
//    and goes before the button or the scroll that happens where it points. While a button is
//    held the moves go: that is a drag, and they chose to make it.
//
// Keys always go: they say nothing of the eyes. A button counts as held for kHeldS after the
// last word of it (an act on its panel, or a pointer that says it is pressed, which comes each
// second while it rests), so one whose release never came doesn't open the pointer for good.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/control.h"
#include "../proto/pointwire.h"

#include <vector>

namespace local {

class Hush {
public:
    static constexpr double kHeldS = 3.0;

    // Whether the wearer's pointer datagram `point` goes to the room at `now` (seconds), and as
    // what: one that may not say where it is says it is nowhere, once.
    bool Point(proto::Point &point, bool quiet, double now) {
        const bool pressed = point.flags & proto::kPointPressed;
        if (pressed && point.panel == panel_ && buttons_) heard_ = now;
        if (quiet && !pressed && point.panel != proto::kNoPanel) {
            if (!shown_) return false;
            point.panel = proto::kNoPanel, point.x = point.y = 0;
        }
        shown_ = point.panel != proto::kNoPanel;
        return true;
    }

    // The wearer's act at `now`: what goes to the panel's owner for it, in order, is added to
    // `out`.
    void Act(const proto::Act &act, bool quiet, double now, std::vector<proto::Act> &out) {
        if (act.what != proto::kActMove && act.what != proto::kActButton && act.what != proto::kActScroll) return out.push_back(act);
        if (buttons_ && (act.panel != panel_ || now - heard_ > kHeldS)) buttons_ = 0;
        if (act.what == proto::kActMove) {
            waits_ = quiet && !buttons_;
            if (waits_) return void(move_ = act);
        } else {
            if (waits_ && move_.panel == act.panel) out.push_back(move_);
            waits_ = false;
            if (act.what == proto::kActButton && act.a >= proto::kLeft && act.a <= proto::kMiddle) {
                const unsigned bit = 1u << act.a;
                buttons_ = act.b ? buttons_ | bit : buttons_ & ~bit;
                panel_ = act.panel;
            }
        }
        if (buttons_ && act.panel == panel_) heard_ = now;
        out.push_back(act);
    }

private:
    bool shown_ = false;     // the room was last told the pointer is on a panel
    bool waits_ = false;     // a move that wasn't given waits
    proto::Act move_;        // ...this one
    unsigned buttons_ = 0;   // the buttons held, a bit each (1 << kLeft ...)
    uint8_t panel_ = 0;      // ...on this panel
    double heard_ = -1e18;   // ...as last heard of then
};

}  // namespace local
