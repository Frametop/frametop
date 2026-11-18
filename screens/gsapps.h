// The gamescope apps' bookkeeping (screens/vrgsapps.cpp's; gsforward.h has what they are):
// which app window has which panel, which popups go over which panel, and where a key goes. Pure:
// gsforward's events in, what to do with the panels out; vrgsapps.cpp does it (the overlays, the tags).
// Tested by screens/tests/gsapps-test.cpp.
//
// A window gets a panel when it's created (so it's tagged before it maps: an untagged window gets
// a panel of gamescope's own for a moment), shown when it maps, and dropped when it unmaps or
// goes. One that's created but not mapped within kMapWaitMs is dropped too (a window an app keeps
// for later, a menu made ahead of time); it gets a panel again if it maps. Past kMax panels, or
// kMaxPopups popups, a window keeps gamescope's own: SteamVR's overlays are few.
#pragma once

#include "gsforward.h"

#include <cstdint>
#include <map>
#include <vector>

namespace gsapps {

constexpr int kFirst = 199;           // panel indices kFirst.. (screen numbers 200..; remote displays are 100..115)
constexpr int kMax = 8;               // panels at most (SteamVR's 128 overlays are shared by everything)
constexpr int64_t kMapWaitMs = 5000;  // created or unmapped, not mapped (again) by then: dropped
constexpr int kMaxPopups = 8;         // popups' overlays at most, all apps' (more keep gamescope's)

struct Action {
    enum Type {
        Make,       // a panel for win at index, hidden (tag win)
        Show,       // win mapped: show its panel (win: its rectangle; Describe for its name)
        Move,       // win's rectangle changed
        Hide,       // win unmapped: its panel hidden, win still tagged (a remap shows it again at once)
        Drop,       // its panel goes (untag win)
        MakePopup,  // a popup over index's panel (tag win; sub: its number there)
        MovePopup,
        DropPopup,
    } type = Make;
    int index = -1;             // the panel's screen index
    int sub = 0;                // a popup's number on its panel
    gsforward::Window win;
};

class Book {
public:
    // What to do about one event at nowMs.
    std::vector<Action> On(const gsforward::Event &e, int64_t nowMs) {
        std::vector<Action> out;
        const uint32_t id = e.win.id;
        switch (e.type) {
            case gsforward::Event::Created:
                if (e.win.kind == gsforward::Kind::Main) MakeMain(e.win, nowMs, out);
                else if (e.win.kind == gsforward::Kind::Popup) MakePopup(e.win, nowMs, out);
                break;
            case gsforward::Event::Mapped: {
                auto m = mains_.find(id);
                // (read again at the map: a window can grow from a helper's size before it maps)
                if (m == mains_.end() && !popups_.count(id) && e.win.kind == gsforward::Kind::Main) {
                    MakeMain(e.win, nowMs, out);
                    m = mains_.find(id);
                }
                if (m != mains_.end()) {
                    m->second.mapped = true, m->second.win = e.win;
                    out.push_back({Action::Show, m->second.index, 0, e.win});
                } else if (popups_.count(id)) {
                    PopupWin &p = popups_[id];
                    p.mapped = true;
                    p.win.x = e.win.x, p.win.y = e.win.y, p.win.w = e.win.w, p.win.h = e.win.h;
                    out.push_back({Action::MovePopup, p.index, p.sub, p.win});
                } else if (e.win.kind == gsforward::Kind::Popup) {
                    MakePopup(e.win, nowMs, out);
                    if (auto p = popups_.find(id); p != popups_.end()) p->second.mapped = true;
                }
                break;
            }
            case gsforward::Event::Moved:
                if (auto m = mains_.find(id); m != mains_.end()) {
                    MainWin &w = m->second;
                    w.win.x = e.win.x, w.win.y = e.win.y, w.win.w = e.win.w, w.win.h = e.win.h;
                    if (w.mapped) out.push_back({Action::Move, w.index, 0, w.win});
                } else if (auto p = popups_.find(id); p != popups_.end()) {
                    PopupWin &q = p->second;
                    q.win.x = e.win.x, q.win.y = e.win.y, q.win.w = e.win.w, q.win.h = e.win.h;
                    out.push_back({Action::MovePopup, q.index, q.sub, q.win});
                }
                break;
            case gsforward::Event::Unmapped:
                // An app's window keeps its panel and its tag a while: one that maps again (a
                // toggled window, a Steam-launched app's splash) doesn't show gamescope's own panel
                // before ours is tagged again, and keeps its place. A popup goes (menus open anew).
                // One with a popup still mapped over it (a dialog left up, as an app minimized to
                // its tray does) goes now, as before: hidden, it would take the dialog with it.
                if (auto m = mains_.find(id); m != mains_.end() && m->second.mapped && !MappedPopupOn(m->second.index)) {
                    m->second.mapped = false, m->second.sinceMs = nowMs;
                    out.push_back({Action::Hide, m->second.index, 0, m->second.win});
                    if (lastPointed_ == id) lastPointed_ = 0;
                } else {
                    Forget(id, out);
                }
                break;
            case gsforward::Event::Gone:
                Forget(id, out);
                break;
            case gsforward::Event::Retarget:   // (Role::Steam's only: never on the app display)
                break;
        }
        return out;
    }

    // Windows created and never mapped, or unmapped and not mapped again, past kMapWaitMs: dropped
    // (popups too).
    std::vector<Action> Tick(int64_t nowMs) {
        std::vector<Action> out;
        std::vector<uint32_t> late;
        for (const auto &[id, m] : mains_)
            if (!m.mapped && nowMs - m.sinceMs > kMapWaitMs) late.push_back(id);
        for (const auto &[id, p] : popups_)
            if (!p.mapped && nowMs - p.createdMs > kMapWaitMs) late.push_back(id);
        for (uint32_t id : late) Forget(id, out);
        return out;
    }

    // Everything goes (the link closed, or the setting went off).
    std::vector<Action> Clear() {
        std::vector<Action> out;
        while (!popups_.empty()) Forget(popups_.begin()->first, out);
        while (!mains_.empty()) Forget(mains_.begin()->first, out);
        return out;
    }

    // The pointer was last on this panel's window (a menu opens over it: gsforward::PopupOwner).
    void Pointed(int index) {
        for (const auto &[id, m] : mains_)
            if (m.index == index) lastPointed_ = id;
    }
    uint32_t WindowOf(int index) const {
        for (const auto &[id, m] : mains_)
            if (m.index == index) return id;
        return 0;
    }
    // A popup's window from its panel and number (0: none).
    uint32_t PopupWindow(int index, int sub) const {
        for (const auto &[id, p] : popups_)
            if (p.index == index && p.sub == sub) return id;
        return 0;
    }
    // Whether the app window at `index` is mapped (its panel shown, unless it's lent: gscontrol.h).
    bool Mapped(int index) const {
        for (const auto &[id, m] : mains_)
            if (m.index == index) return m.mapped;
        return false;
    }
    const gsforward::Window *MainRect(int index) const {
        for (const auto &[id, m] : mains_)
            if (m.index == index) return &m.win;
        return nullptr;
    }
    const gsforward::Window *PopupRect(int index, int sub) const {
        const uint32_t id = PopupWindow(index, sub);
        return id ? &popups_.at(id).win : nullptr;
    }
    // The panel a window is on (a popup's: its app's), or -1.
    int PanelOf(uint32_t win) const {
        if (auto m = mains_.find(win); m != mains_.end()) return m->second.index;
        if (auto p = popups_.find(win); p != popups_.end()) return p->second.index;
        return -1;
    }
    // A popup that's override-redirect (a menu or a tooltip, not a dialog).
    bool Menu(uint32_t win) const {
        const auto p = popups_.find(win);
        return p != popups_.end() && p->second.win.overrideRedirect;
    }
    // A window that couldn't have its panel after all: what goes with it.
    std::vector<Action> Lose(uint32_t win) {
        std::vector<Action> out;
        Forget(win, out);
        return out;
    }
    static bool Ours(int index) { return index >= kFirst && index < kFirst + kMax; }
    size_t Panels() const { return mains_.size(); }

private:
    struct MainWin {
        int index;
        bool mapped = false;
        int64_t sinceMs = 0;   // when it was created, or unmapped last
        gsforward::Window win;
    };
    struct PopupWin {
        int index, sub;
        gsforward::Window win;
        bool mapped = false;
        int64_t createdMs = 0;
    };
    std::map<uint32_t, MainWin> mains_;
    std::map<uint32_t, PopupWin> popups_;
    uint32_t lastPointed_ = 0;
    int nextSub_ = 1;

    void MakeMain(const gsforward::Window &w, int64_t nowMs, std::vector<Action> &out) {
        if (mains_.count(w.id)) return;
        int index = -1;
        for (int i = kFirst; i < kFirst + kMax && index < 0; ++i) {
            bool used = false;
            for (const auto &[id, m] : mains_) used |= m.index == i;
            if (!used) index = i;
        }
        if (index < 0) return;   // full: gamescope's own panel has it
        mains_[w.id] = {index, false, nowMs, w};
        out.push_back({Action::Make, index, 0, w});
    }
    void MakePopup(const gsforward::Window &w, int64_t nowMs, std::vector<Action> &out) {
        // (no panel to go over, or too many: gamescope's)
        if (popups_.count(w.id) || mains_.empty() || int(popups_.size()) >= kMaxPopups) return;
        int index = -1;
        if (const auto tp = popups_.find(w.transientFor); w.transientFor && tp != popups_.end())
            index = tp->second.index;   // a dialog's dialog or a submenu: over the same app's
        else {
            std::vector<uint32_t> mapped;
            for (const auto &[id, m] : mains_)
                if (m.mapped) mapped.push_back(id);
            const uint32_t owner = gsforward::PopupOwner(w.transientFor, lastPointed_, mapped);
            if (!owner) return;
            index = mains_[owner].index;
        }
        const int sub = nextSub_++;
        popups_[w.id] = {index, sub, w, false, nowMs};
        out.push_back({Action::MakePopup, index, sub, w});
    }
    bool MappedPopupOn(int index) const {
        for (const auto &[pid, p] : popups_)
            if (p.index == index && p.mapped) return true;
        return false;
    }
    void Forget(uint32_t id, std::vector<Action> &out) {
        if (auto p = popups_.find(id); p != popups_.end()) {
            out.push_back({Action::DropPopup, p->second.index, p->second.sub, p->second.win});
            popups_.erase(p);
            return;
        }
        auto m = mains_.find(id);
        if (m == mains_.end()) return;
        const int index = m->second.index;
        // its popups first (they go over nothing now)
        for (auto p = popups_.begin(); p != popups_.end();) {
            if (p->second.index == index) out.push_back({Action::DropPopup, index, p->second.sub, p->second.win}), p = popups_.erase(p);
            else ++p;
        }
        out.push_back({Action::Drop, index, 0, m->second.win});
        if (lastPointed_ == id) lastPointed_ = 0;
        mains_.erase(m);
    }
};

// Where keys go: to the app window clicked last (Focus), until a click elsewhere (Focus(0)). A key
// goes up where it went down, wherever the typing has gone since.
class Keys {
public:
    void Focus(uint32_t win) { target_ = win; }
    uint32_t Target() const { return target_; }
    // The window for this key (value 0 up, 1 down, 2 a repeat), or 0: not an app's. `elsewhere`:
    // something else has the typing now (frunner's search), which a press doesn't go past.
    uint32_t Route(uint32_t code, int value, bool elsewhere) {
        auto d = down_.find(code);
        if (value == 0) {
            if (d == down_.end()) return 0;
            const uint32_t w = d->second;
            down_.erase(d);
            return w;
        }
        if (value == 2) return d != down_.end() && !elsewhere ? d->second : 0;
        if (elsewhere || !target_) return 0;
        down_[code] = target_;
        return target_;
    }
    // The window went: its keys are nobody's (their releases go on as no app's).
    void Gone(uint32_t win) {
        if (target_ == win) target_ = 0;
        Released(win);
    }
    // Its keys were let go of (it's hidden: the typing stays, to go on when it's shown again).
    void Released(uint32_t win) {
        for (auto d = down_.begin(); d != down_.end();) d = d->second == win ? down_.erase(d) : std::next(d);
    }
    // Keys down on apps now (to let go of at the end).
    const std::map<uint32_t, uint32_t> &Down() const { return down_; }

private:
    uint32_t target_ = 0;
    std::map<uint32_t, uint32_t> down_;   // key -> the window it went down on
};

// Which laser has the X pointer (gamescope's screen has one, all app panels share it): the one
// that moved it last keeps it while it moves (a move within kHoldMs of its last) or holds a
// button; another laser's moves wait until then, and a press takes it at once. Two lasers on two
// app panels don't flip it (and its raise) between them every frame.
class PointerOwner {
public:
    static constexpr int64_t kHoldMs = 300;
    // A move by `device`: whether it may move the pointer (and it's its now).
    bool Move(uint32_t device, int64_t nowMs) {
        if (owned_ && owner_ != device && (held_ || nowMs - lastMs_ < kHoldMs)) return false;
        if (!owned_ || owner_ != device) held_ = false;
        owned_ = true, owner_ = device, lastMs_ = nowMs;
        return true;
    }
    // `panel` and `sub` (0: the panel itself, else a popup over it): what was pressed on (its going
    // lets go: PanelGone).
    void Press(uint32_t device, bool down, int64_t nowMs, int panel = -1, int sub = 0) {
        if (down) owned_ = true, owner_ = device, held_ = true, panel_ = panel, sub_ = sub;
        else if (owned_ && owner_ == device) held_ = false;
        if (owned_ && owner_ == device) lastMs_ = nowMs;
    }
    // It left the app panels (or the app went): anyone's.
    void Left(uint32_t device) {
        if (owned_ && owner_ == device) owned_ = false, held_ = false;
    }
    // What was pressed on went or hid (a button that closes its window, a menu item its menu): no
    // release or leave comes from it, so the press is over. sub -1: the panel and its popups.
    void PanelGone(int panel, int sub = -1) {
        if (owned_ && held_ && panel_ == panel && (sub < 0 || sub_ == sub)) owned_ = false, held_ = false;
    }

private:
    bool owned_ = false, held_ = false;
    int panel_ = -1, sub_ = 0;
    uint32_t owner_ = 0;
    int64_t lastMs_ = 0;
};

}  // namespace gsapps
