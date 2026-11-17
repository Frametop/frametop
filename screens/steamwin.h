// Steam's dashboard window as a panel of ours (user, 2026-11-17: "We need the dashboard Steam window
// to use frame-top displays, not GameScope anymore"; a spike, STEAM_DASH_PANEL, off by default).
//
// Steam's big dashboard window isn't gamescope's: it's Steam's own CEF window, "SteamVR", on
// gamescope's first Xwayland (`:0`), which Steam tags with STEAM_GAMESCOPE_VROVERLAY_TARGET naming
// its own overlay (valve.steam.gamepadui.main), the same forwarding the gamescope apps use
// (gsforward.h). Tagged with one of ours instead, gamescope puts its frames on that one: a panel
// like any other, tiled on the dashboard. Steam's bar, menu, keyboard and popups stay Steam's.
//
// Tracker is the policy: which window it is, when to tag it ours, when to give it back to Steam
// (switched off, ft-screens going, and on opening when a tag of ours was left behind), and what
// to do when Steam tags it again itself: tag it ours again, unless Steam keeps doing it
// (kMaxRetakes within kRetakeWindowMs), then give it back and say so. Pure: vrsteamwin.cpp does
// what it says on X and OpenVR. Header only; tested by screens/tests/steamwin-test.cpp.
#pragma once

#include "gsforward.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace steamwin {

constexpr const char *kTitle = "SteamVR";              // Steam's dashboard window's name
constexpr const char *kSteamOverlay = "valve.steam.gamepadui.main";   // the overlay Steam tags it with
constexpr int kMaxRetakes = 5;
constexpr int64_t kRetakeWindowMs = 10000;

struct Action {
    enum Type {
        Make,     // a panel for it (hidden); its overlay goes to Tracker::Made
        Tag,      // the window's target to `overlay`: ours, or Steam's back
        Show,     // it's mapped and ours: the panel shows it, `win` its size
        Move,     // its size changed
        Hide,     // unmapped: the panel hidden
        Drop,     // the panel goes
        GaveUp,   // Steam kept tagging it back: given back to Steam (the switch stays on, idle)
    } type = Make;
    gsforward::Window win;
    uint64_t overlay = 0;
};

class Tracker {
public:
    // Steam's display opened, or Steam's overlay found or made again: `steams` the overlay Steam
    // tags its window with (0: not found). Nothing is taken without it: there'd be nothing to give
    // the window back to.
    std::vector<Action> SetSteams(uint64_t steams) {
        std::vector<Action> out;
        steams_ = steams;
        if (on_ && !gaveUp_ && mapped_) Take(&out);
        return out;
    }
    // It went: the panel goes (nothing to tag back on a display that's gone).
    std::vector<Action> Closed() {
        std::vector<Action> out;
        if (made_) out.push_back({Action::Drop, win_});
        win_ = {}, mapped_ = false, made_ = 0, retakes_.clear(), gaveUp_ = false;
        return out;
    }

    // STEAM_DASH_PANEL. Off: the window back to Steam, the panel gone.
    std::vector<Action> Switch(bool on) {
        std::vector<Action> out;
        if (on == on_) return out;
        on_ = on, gaveUp_ = false, retakes_.clear();
        if (on && mapped_) Take(&out);
        else GiveBack(&out);
        return out;
    }
    bool On() const { return on_; }

    // The panel Make asked for: its overlay (0: none, the window stays Steam's).
    std::vector<Action> Made(uint64_t overlay) {
        std::vector<Action> out;
        if (!overlay || !on_ || !win_.id) return out;
        made_ = overlay;
        out.push_back({Action::Tag, win_, made_});
        if (mapped_) out.push_back({Action::Show, win_});
        return out;
    }

    std::vector<Action> On(const gsforward::Event &e, int64_t nowMs) {
        std::vector<Action> out;
        const bool its = win_.id && e.win.id == win_.id;
        switch (e.type) {
            case gsforward::Event::Created: break;   // (named when it maps)
            case gsforward::Event::Mapped:
                if (e.win.title != kTitle || e.win.kind != gsforward::Kind::Main) break;
                if (!its) {
                    if (win_.id) Forget(&out);   // (a second one: the newest is Steam's now)
                    win_ = e.win;
                }
                win_.w = e.win.w, win_.h = e.win.h, win_.x = e.win.x, win_.y = e.win.y;
                mapped_ = true;
                if (on_ && !gaveUp_) {
                    if (!made_) Take(&out);
                    else out.push_back({Action::Show, win_});
                } else if (Stale(e.win.target)) {
                    out.push_back({Action::Tag, win_, steams_});   // a tag of ours left behind
                }
                break;
            case gsforward::Event::Moved:
                if (!its) break;
                win_.x = e.win.x, win_.y = e.win.y, win_.w = e.win.w, win_.h = e.win.h;
                if (made_) out.push_back({Action::Move, win_});
                break;
            case gsforward::Event::Unmapped:
                if (!its) break;
                mapped_ = false;
                if (made_) out.push_back({Action::Hide, win_});
                break;
            case gsforward::Event::Gone:
                if (its) Forget(&out);
                break;
            case gsforward::Event::Retarget:
                if (!its || !made_ || e.win.target == made_) break;
                ++retaken_;
                retakes_.push_back(nowMs);
                while (!retakes_.empty() && nowMs - retakes_.front() > kRetakeWindowMs) retakes_.pop_front();
                if (int(retakes_.size()) >= kMaxRetakes) {
                    GiveBack(&out);
                    gaveUp_ = true;
                    out.push_back({Action::GaveUp, win_});
                } else {
                    out.push_back({Action::Tag, win_, made_});
                }
                break;
        }
        return out;
    }

    // ft-screens going: the window back to Steam (no panel to drop: they all go).
    std::vector<Action> Shutdown() {
        std::vector<Action> out;
        if (made_ && win_.id && steams_) out.push_back({Action::Tag, win_, steams_});
        made_ = 0;
        return out;
    }

    uint32_t Window() const { return win_.id; }
    uint64_t Panel() const { return made_; }
    int Retaken() const { return retaken_; }   // times Steam tagged it back (this run)

private:
    gsforward::Window win_;
    bool mapped_ = false, on_ = false, gaveUp_ = false;
    uint64_t made_ = 0, steams_ = 0;
    std::deque<int64_t> retakes_;
    int retaken_ = 0;

    // A target that's neither Steam's nor none: a tag of ours from an ft-screens that went.
    bool Stale(uint64_t target) const { return steams_ && target && target != steams_; }
    void Take(std::vector<Action> *out) {
        if (win_.id && !made_ && steams_) out->push_back({Action::Make, win_});
    }
    void GiveBack(std::vector<Action> *out) {
        if (!made_) return;
        if (win_.id && steams_) out->push_back({Action::Tag, win_, steams_});
        out->push_back({Action::Drop, win_});
        made_ = 0;
    }
    void Forget(std::vector<Action> *out) {
        if (made_) out->push_back({Action::Drop, win_});
        win_ = {}, mapped_ = false, made_ = 0;
    }
};

}  // namespace steamwin
