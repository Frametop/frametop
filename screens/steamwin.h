// Steam's own windows as panels of ours (user, 2026-11-17: "We need the dashboard Steam window to
// use frame-top displays, not GameScope anymore", then "a prototype of the whole steamos with the
// exception of the horizontal dashboard/launcher using Frametop windows instead of gamescope"; a
// spike, STEAM_DASH_PANEL, off by default).
//
// Steam's dashboard windows aren't gamescope's apps: they're Steam's own CEF windows on gamescope's
// first Xwayland (`:0`), each tagged with STEAM_GAMESCOPE_VROVERLAY_TARGET naming Steam's overlay for
// it, the same forwarding the gamescope apps use (gsforward.h). Tagged with one of ours instead,
// gamescope puts its frames on that one. What each window becomes (Classify, by its title):
//
//   - tiles: the big dashboard window ("SteamVR") and Steam's left menu (frame.menu), panels tiled on
//     the dashboard like any other;
//   - followers: Steam's popups (the footer, tooltips, the keyboard, notifications, the volume),
//     bare overlays of ours placed where Steam puts its own, moved with the tile they sit on
//     (steamplace.h);
//   - Steam's: the bar and the launcher it opens (barpopup) stay Steam's, as does anything unknown.
//
// Tracker is one window's policy: when to tag it ours, when to give it back to Steam (switched off,
// ft-screens going, and on opening when a tag of ours was left behind), and what to do when Steam
// tags it again itself: tag it ours again, unless Steam keeps doing it (kMaxRetakes within
// kRetakeWindowMs), then give it back and say so. Fleet keeps one per window and routes X's news to
// them. Pure: vrsteamwin.cpp does what they say on X and OpenVR. Header only; tested by
// screens/tests/steamwin-test.cpp.
#pragma once

#include "gsforward.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace steamwin {

constexpr const char *kTitle = "SteamVR";               // Steam's dashboard window's name
constexpr const char *kPrefix = "SteamVR - ";            // and the start of its others'
constexpr const char *kSteamPrefix = "valve.steam.gamepadui.";
constexpr int kMaxRetakes = 5;
constexpr int64_t kRetakeWindowMs = 10000;
constexpr int kTiles = 2;                                // tile slots: 0 the dashboard window, 1 the menu

// What a window of Steam's is to us.
struct Spec {
    enum Role { Steams, Tile, Follow } role = Steams;
    int slot = -1;            // a tile's: 0 the dashboard window, 1 the menu
    std::string overlay;      // Steam's overlay's key for it ("" for Steam's own ones)
    std::string tileKey;      // a tile's tile-memory key
    bool operator==(const Spec &o) const { return role == o.role && slot == o.slot && overlay == o.overlay; }
};

// Steam's overlay's key, from a window's title: "SteamVR - <key> (<n>)" (the keyboard and the
// notifications named in words: their keys are fixed).
inline std::string OverlayKey(const std::string &title) {
    if (title == kTitle) return std::string(kSteamPrefix) + "main";
    if (title == "SteamVR - Keyboard") return std::string(kSteamPrefix) + "keyboard";
    if (title == "SteamVR - Notifications") return std::string(kSteamPrefix) + "notifications";
    const std::string p = kPrefix;
    if (title.compare(0, p.size(), p) != 0) return "";
    std::string k = title.substr(p.size());
    const size_t paren = k.rfind(" (");
    if (paren != std::string::npos) k.resize(paren);
    return k.compare(0, std::string(kSteamPrefix).size(), kSteamPrefix) == 0 ? k : "";
}

inline Spec Classify(const std::string &title) {
    Spec s;
    s.overlay = OverlayKey(title);
    if (s.overlay.empty()) return s;   // (Steam's 1x1 helper page, anything else)
    const std::string name = s.overlay.substr(std::string(kSteamPrefix).size());
    auto starts = [&](const char *p) { return name.compare(0, std::string(p).size(), p) == 0; };
    if (name == "main") s.role = Spec::Tile, s.slot = 0, s.tileKey = "steam:dashboard";
    else if (starts("frame.menu")) s.role = Spec::Tile, s.slot = 1, s.tileKey = "steam:menu";
    else if (name == "bar" || starts("barpopup")) s.role = Spec::Steams, s.overlay.clear();
    else s.role = Spec::Follow;
    return s;
}

struct Action {
    enum Type {
        Make,     // a panel (a tile) or an overlay (a follower) for it, hidden; its overlay to Made
        Tag,      // the window's target to `overlay`: ours, or Steam's back
        Show,     // it's mapped and ours: shown, `win` its size
        Move,     // its size changed
        Hide,     // unmapped: hidden
        Drop,     // ours goes (Drop, Move and Hide: `overlay` is ours)
        GaveUp,   // Steam kept tagging it back: given back to Steam (the switch stays on, idle)
    } type = Make;
    gsforward::Window win;
    uint64_t overlay = 0;
    Spec spec;
};

class Tracker {
public:
    explicit Tracker(Spec spec = {}) : spec_(std::move(spec)) {}

    // Steam's overlay for it found (0: not, or gone). Nothing is taken without it: there'd be
    // nothing to give the window back to. Off, a tag of ours left behind goes back now: at start
    // the windows map before Steam's overlays are looked up (2026-11-18: an ft-screens killed by a
    // restart left Steam's page blank, with SteamVR's controls for it over the tiles).
    std::vector<Action> SetSteams(uint64_t steams) {
        std::vector<Action> out;
        steams_ = steams;
        if (on_ && !gaveUp_ && mapped_) Take(&out);
        else if (!made_ && mapped_ && Stale(win_.target)) {
            out.push_back(A(Action::Tag, steams_));
            win_.target = steams_;
        }
        return out;
    }
    // Its display went: ours goes (nothing to tag back on a display that's gone).
    std::vector<Action> Closed() {
        std::vector<Action> out;
        if (made_) out.push_back(A(Action::Drop, made_));
        mapped_ = false, made_ = 0, retakes_.clear(), gaveUp_ = false;
        return out;
    }

    // STEAM_DASH_PANEL. Off: the window back to Steam, ours gone.
    std::vector<Action> Switch(bool on) {
        std::vector<Action> out;
        if (on == on_) return out;
        on_ = on, gaveUp_ = false, retakes_.clear();
        if (on && mapped_) Take(&out);
        else GiveBack(&out);
        return out;
    }

    // What Make asked for: its overlay (0: none, the window stays Steam's).
    std::vector<Action> Made(uint64_t overlay) {
        std::vector<Action> out;
        if (!overlay || !on_ || !win_.id) return out;
        made_ = overlay;
        out.push_back(A(Action::Tag, made_));
        if (mapped_) out.push_back(A(Action::Show));
        return out;
    }

    // X's news of its window.
    std::vector<Action> On(const gsforward::Event &e, int64_t nowMs) {
        std::vector<Action> out;
        if (win_.id && e.win.id != win_.id) return out;
        switch (e.type) {
            case gsforward::Event::Created: break;   // (named when it maps)
            case gsforward::Event::Mapped:
                win_ = e.win;
                mapped_ = true;
                if (on_ && !gaveUp_) {
                    if (!made_) Take(&out);
                    else out.push_back(A(Action::Show));
                } else if (Stale(e.win.target)) {
                    out.push_back(A(Action::Tag, steams_));   // a tag of ours left behind
                    win_.target = steams_;
                }
                break;
            case gsforward::Event::Moved:
                win_.x = e.win.x, win_.y = e.win.y, win_.w = e.win.w, win_.h = e.win.h;
                if (made_) out.push_back(A(Action::Move, made_));
                break;
            case gsforward::Event::Unmapped:
                mapped_ = false;
                if (made_) out.push_back(A(Action::Hide, made_));
                break;
            case gsforward::Event::Gone:
                if (made_) out.push_back(A(Action::Drop, made_));
                mapped_ = false, made_ = 0;
                break;
            case gsforward::Event::Retarget:
                if (!made_ || e.win.target == made_) break;
                ++retaken_;
                retakes_.push_back(nowMs);
                while (!retakes_.empty() && nowMs - retakes_.front() > kRetakeWindowMs) retakes_.pop_front();
                if (int(retakes_.size()) >= kMaxRetakes) {
                    GiveBack(&out);
                    gaveUp_ = true;
                    out.push_back(A(Action::GaveUp));
                } else {
                    out.push_back(A(Action::Tag, made_));
                }
                break;
        }
        return out;
    }

    // ft-screens going: the window back to Steam (nothing to drop: they all go).
    std::vector<Action> Shutdown() {
        std::vector<Action> out;
        if (made_ && win_.id && steams_) out.push_back(A(Action::Tag, steams_));
        made_ = 0;
        return out;
    }

    // Its panel gone some other way (a tile closed from Frametop): the window back to Steam.
    std::vector<Action> Lost() {
        std::vector<Action> out;
        if (made_ && win_.id && steams_) out.push_back(A(Action::Tag, steams_));
        made_ = 0, gaveUp_ = true;   // (until switched off and on: it'd only be taken straight back)
        return out;
    }

    const Spec &What() const { return spec_; }
    const gsforward::Window &Win() const { return win_; }
    uint32_t Window() const { return win_.id; }
    bool Mapped() const { return mapped_; }
    uint64_t Panel() const { return made_; }
    uint64_t Steams() const { return steams_; }
    int Retaken() const { return retaken_; }   // times Steam tagged it back (this run)

private:
    Spec spec_;
    gsforward::Window win_;
    bool mapped_ = false, on_ = false, gaveUp_ = false;
    uint64_t made_ = 0, steams_ = 0;
    std::deque<int64_t> retakes_;
    int retaken_ = 0;

    Action A(Action::Type t, uint64_t overlay = 0) const { return {t, win_, overlay, spec_}; }
    // A target that's neither Steam's nor none: a tag of ours from an ft-screens that went.
    bool Stale(uint64_t target) const { return steams_ && target && target != steams_; }
    void Take(std::vector<Action> *out) {
        if (win_.id && !made_ && steams_) out->push_back(A(Action::Make));
    }
    void GiveBack(std::vector<Action> *out) {
        if (!made_) return;
        if (win_.id && steams_) out->push_back(A(Action::Tag, steams_));
        out->push_back(A(Action::Drop, made_));
        made_ = 0;
    }
};

// Every window of Steam's we know: a Tracker each, by window id. Steam's own ones (the bar, the
// launcher) aren't kept.
class Fleet {
public:
    std::vector<Action> Switch(bool on) {
        on_ = on;
        std::vector<Action> out;
        for (auto &[id, t] : wins_) Add(&out, t.Switch(on));
        return out;
    }
    bool On() const { return on_; }

    std::vector<Action> On(const gsforward::Event &e, int64_t nowMs) {
        std::vector<Action> out;
        auto it = wins_.find(e.win.id);
        if (e.type == gsforward::Event::Mapped) {
            const Spec spec = Classify(e.win.title);
            if (it != wins_.end() && !(it->second.What() == spec)) {   // (renamed: something else now)
                Add(&out, it->second.Closed());
                wins_.erase(it), it = wins_.end();
            }
            if (spec.role == Spec::Steams) return out;
            if (it == wins_.end()) {
                if (spec.role == Spec::Tile) {   // (a second one in a tile's slot: the newest is it)
                    for (auto o = wins_.begin(); o != wins_.end();) {
                        if (o->second.What().role == Spec::Tile && o->second.What().slot == spec.slot)
                            Add(&out, o->second.Closed()), o = wins_.erase(o);
                        else
                            ++o;
                    }
                }
                it = wins_.emplace(e.win.id, Tracker(spec)).first;
                it->second.Switch(on_);   // (not mapped yet: nothing to do)
            }
        }
        if (it == wins_.end()) return out;
        Add(&out, it->second.On(e, nowMs));
        if (e.type == gsforward::Event::Gone) wins_.erase(it);
        return out;
    }

    std::vector<Action> SetSteams(uint32_t win, uint64_t steams) {
        const auto it = wins_.find(win);
        return it == wins_.end() ? std::vector<Action>{} : it->second.SetSteams(steams);
    }
    std::vector<Action> Made(uint32_t win, uint64_t overlay) {
        const auto it = wins_.find(win);
        return it == wins_.end() ? std::vector<Action>{} : it->second.Made(overlay);
    }
    std::vector<Action> Lost(uint32_t win) {
        const auto it = wins_.find(win);
        return it == wins_.end() ? std::vector<Action>{} : it->second.Lost();
    }
    // The display went: all of ours go, nothing kept.
    std::vector<Action> Closed() {
        std::vector<Action> out;
        for (auto &[id, t] : wins_) Add(&out, t.Closed());
        wins_.clear();
        return out;
    }
    std::vector<Action> Shutdown() {
        std::vector<Action> out;
        for (auto &[id, t] : wins_) Add(&out, t.Shutdown());
        return out;
    }

    const std::map<uint32_t, Tracker> &Windows() const { return wins_; }
    const Tracker *Find(uint32_t win) const {
        const auto it = wins_.find(win);
        return it == wins_.end() ? nullptr : &it->second;
    }
    // The tile in `slot`, or none.
    const Tracker *TileIn(int slot) const {
        for (const auto &[id, t] : wins_)
            if (t.What().role == Spec::Tile && t.What().slot == slot) return &t;
        return nullptr;
    }

private:
    std::map<uint32_t, Tracker> wins_;
    bool on_ = false;
    static void Add(std::vector<Action> *out, std::vector<Action> more) {
        for (Action &a : more) out->push_back(std::move(a));
    }
};

}  // namespace steamwin
