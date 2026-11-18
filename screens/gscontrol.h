// The gamescope apps (screens/vrgsapps.cpp), between the X side and the panels: what
// gsapps::Book says to do with each window, done (a panel made, its window tagged; dropped,
// untagged), and the laser and the keys on an app's panel as X's. Pure: the X side
// (gsforward::XSide: gslink.h's AsyncLink) and the panels (Panels: vrgsapps.cpp's, on OpenVR) come in,
// so its test (screens/tests/gscontrol-test.cpp) runs with fakes of both.
//
// The laser on a panel or a popup over it moves the X pointer there, and raises that window
// first: apps overlap on gamescope's one screen, and the X pointer goes to the top one. gamescope
// sends its pointer only while it has focused one of the apps' windows, and never focuses a tagged
// one (gsforward.h): the laser coming onto an app with none focused by us yet (or the one we
// focused gone or unmapped), and a press on another app's window, focus that app's window first. A press
// gives that window the typing (a dialog's own, not its app's), until a click somewhere that isn't
// an app (Blur). A dialog the window with the typing opens takes it, as a window manager gives it
// one (not a menu or a tooltip: override-redirect, a window manager never focuses those), also
// when its menu still has the typing (an item that opens a dialog: the app maps it first). When a
// dialog with the typing goes, the window it's for has it again (as a window manager does), else
// the one that had it before, else its app's; when a menu goes, the one that had it before (a
// dialog's menu: the dialog), else the one it's for, else its app's. Keys go only
// while the panel of the window with the typing is shown. A window that goes takes nothing held
// with it: a button pressed on it and keys down on it are let go of. One that unmaps is hidden,
// still tagged, and shown again if it maps within gsapps::kMapWaitMs (dropped otherwise).
// View in Theater lends an app's window (and its popups) to SteamVR's theater: untagged, so
// gamescope gives it its own overlay, which SteamVR takes into its theater (gstheater.h has when it
// comes back). Its panel hides meanwhile, and a popup it opens isn't tagged ours; Reclaim tags
// them all again and shows the panel if the window's mapped.
#pragma once

#include "gsapps.h"
#include "gsforward.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace gsapps {

// The panels, as vrgsapps.cpp makes them.
struct Panels {
    virtual ~Panels() = default;
    // A hidden panel for an app's window at `index`: its overlay's handle, gamescope (`pid`) its
    // renderer; 0 if it couldn't (the window keeps gamescope's own panel).
    virtual uint64_t Make(int index, const gsforward::Window &w, uint32_t pid) = 0;
    virtual void Show(int index, const gsforward::Window &w) = 0;   // it mapped: named, sized, placed
    virtual void Move(int index, const gsforward::Window &w) = 0;   // its size or place on X's screen
    virtual void Hide(int index) = 0;   // it unmapped: hidden until Show again (or Drop)
    virtual void Drop(int index) = 0;
    // A popup's overlay (number `sub` over the panel at `index`), as Make's.
    virtual uint64_t MakePopup(int index, int sub, const gsforward::Window &w, uint32_t pid) = 0;
    virtual void MovePopup(int index, int sub, const gsforward::Window &w) = 0;
    virtual void DropPopup(int index, int sub) = 0;
    // Whether the panel at `index` can be seen (and typed into) now.
    virtual bool Shown(int index) const = 0;
};

class Control {
public:
    Control(gsforward::XSide &x, Panels &panels) : x_(x), panels_(panels) {}

    // The app display opened (gamescope's pid), or went (the panels go; nothing to untag).
    void Opened(uint32_t pid) { pid_ = pid; }
    void Closed() {
        tagging_ = false;
        Do(book_.Clear());
        tagging_ = true;
        buttons_ = 0, raised_ = 0, pressedOn_ = 0, blurredFrom_ = 0, focused_ = 0, keys_ = Keys();
        typedBefore_.clear(), lent_.clear();
    }
    // Off (GSAPP_PANELS, or ft-screens going): buttons and keys held let go of, each window back to
    // gamescope's own panel.
    void Off() {
        LetGo();
        for (const auto &[code, win] : keys_.Down()) x_.Key(win, gsforward::XKeycode(code), 0, false);
        keys_ = Keys();
        Do(book_.Clear());
        raised_ = 0, pressedOn_ = 0, blurredFrom_ = 0, focused_ = 0;
        typedBefore_.clear(), lent_.clear();
    }
    void On(const gsforward::Event &e, int64_t nowMs) {
        Do(book_.On(e, nowMs));
        // a dialog for the window with the typing (or with it under the menus that have it): the
        // typing goes to it
        if (e.type == gsforward::Event::Mapped && !e.win.overrideRedirect && e.win.transientFor &&
            Typed() == e.win.transientFor && book_.PanelOf(e.win.id) >= 0)
            TypeIn(e.win.id);
    }
    void Tick(int64_t nowMs) { Do(book_.Tick(nowMs)); }

    // The laser at (x, y) on X's screen, on the panel at `index` (sub 0) or its popup `sub`.
    void Point(int index, int sub, int x, int y) {
        const uint32_t w = WindowAt(index, sub);
        if (!w) return;
        if (!focused_) FocusOn(book_.WindowOf(index));
        book_.Pointed(index);
        if (raised_ != w) x_.Raise(w), raised_ = w;
        x_.Pointer(x, y);
    }
    // A button (X's) there. A press gives that window the typing.
    void Press(int index, int sub, uint8_t button, bool down, int x, int y) {
        const uint32_t w = WindowAt(index, sub);
        if (!w) return;
        if (down) {
            if (sub) TypeIn(w);
            else keys_.Focus(w), FocusOn(w);
            pressedOn_ = w, blurredFrom_ = 0;
        }
        Point(index, sub, x, y);
        x_.Button(button, down);
        if (button < 32) buttons_ = down ? buttons_ | 1u << button : buttons_ & ~(1u << button);
    }
    void Scroll(int notches) { x_.Scroll(notches); }
    // The buttons held, let go (the laser left the app, or the app went).
    void LetGo() {
        for (uint8_t b = 1; b < 32; ++b)
            if (buttons_ & 1u << b) x_.Button(b, false);
        buttons_ = 0;
    }
    bool Holding() const { return buttons_ != 0; }
    // A click on something that isn't an app: the typing leaves the apps.
    void Blur() {
        if (keys_.Target()) blurredFrom_ = keys_.Target();
        keys_.Focus(0);
    }
    // An app has the typing, on a panel that's shown.
    bool Typing() const { return TargetShown(); }
    // A key (value 0 up, 1 down, 2 a repeat; `elsewhere`: something else has the typing now):
    // true if it went to an app.
    bool Key(uint32_t code, int value, bool elsewhere, gsforward::Mods now) {
        const uint32_t win = keys_.Route(code, value, elsewhere || (value && !TargetShown()));
        if (!win) return false;
        x_.Key(win, gsforward::XKeycode(code), gsforward::State(gsforward::Before(now, code, value != 0)), value != 0);
        return true;
    }
    void Resize(int index, int w, int h) {
        if (const uint32_t win = book_.WindowOf(index)) x_.Resize(win, w, h);
    }
    const Book &Windows() const { return book_; }

    // View in Theater: the app window at `index` and its popups untagged (gamescope's own overlay
    // has them, SteamVR's theater shows it), what's held on them let go of, its panel hidden. The
    // window lent, or 0 (no app there, or it's lent already).
    uint32_t Lend(int index) {
        const uint32_t win = book_.WindowOf(index);
        if (!win || lent_.count(win)) return 0;
        lent_.insert(win);
        for (const auto &[w, o] : overlays_)
            if (book_.PanelOf(w) == index) {
                LetGoOf(w), x_.Untag(w);
                if (raised_ == w) raised_ = 0;
                if (focused_ == w) focused_ = 0;   // (gamescope focuses it in the theater: again when it's back)
            }
        panels_.Hide(index);
        return win;
    }
    // A lent window back on its panel: it and its popups tagged ours again, the panel shown if it's
    // mapped. False if it isn't lent (it went meanwhile).
    bool Reclaim(uint32_t win) {
        if (!lent_.erase(win)) return false;
        const int index = book_.PanelOf(win);
        if (index < 0) return false;
        for (const auto &[w, o] : overlays_)
            if (book_.PanelOf(w) == index) x_.Tag(w, o);
        if (const gsforward::Window *r = book_.MainRect(index); r && book_.Mapped(index)) panels_.Show(index, *r);
        return true;
    }
    bool Lent(int index) const {
        const uint32_t win = book_.WindowOf(index);
        return win && lent_.count(win);
    }

private:
    gsforward::XSide &x_;
    Panels &panels_;
    Book book_;
    Keys keys_;
    uint32_t pid_ = 0;
    uint32_t raised_ = 0;     // the window raised last
    uint32_t buttons_ = 0;    // X buttons held (bit b: button b)
    uint32_t pressedOn_ = 0;  // the window the last press went to
    uint32_t focused_ = 0;    // the app window we had gamescope focus last (0: none, or it went or unmapped)
    // a popup that had the typing -> the app window that had it before (erased when the popup goes)
    std::map<uint32_t, uint32_t> typedBefore_;
    uint32_t blurredFrom_ = 0;   // the app window that had the typing at the last Blur (0 after a press)
    std::map<uint32_t, uint64_t> overlays_;   // each window with a panel or popup -> its overlay (its tag)
    std::set<uint32_t> lent_;                 // app windows lent to SteamVR's theater (untagged)
    bool tagging_ = true;     // (false: the display's gone, nobody to untag at)

    void FocusOn(uint32_t w) {
        if (!w || w == focused_) return;
        x_.Focus(w);
        focused_ = w;
    }
    uint32_t WindowAt(int index, int sub) const { return sub ? book_.PopupWindow(index, sub) : book_.WindowOf(index); }
    bool TargetShown() const {
        const int index = keys_.Target() ? book_.PanelOf(keys_.Target()) : -1;
        return index >= 0 && panels_.Shown(index);
    }
    // The window with the typing, past the menus that have it (each to the one it took it from).
    uint32_t Typed() const {
        uint32_t t = keys_.Target();
        for (int hops = 0; t && book_.Menu(t) && hops < 16; ++hops) {
            const auto b = typedBefore_.find(t);
            if (b == typedBefore_.end()) break;
            t = b->second;
        }
        return t;
    }
    // The typing to popup `w`, the app window it leaves remembered (off the apps since a Blur, with
    // no press on an app after it: the one that had it before the Blur).
    void TypeIn(uint32_t w) {
        if (const uint32_t t = keys_.Target() ? keys_.Target() : blurredFrom_; t && t != w) typedBefore_[w] = t;
        keys_.Focus(w);
    }
    // What's held on `win` let go of (its overlay's gone or hidden: no release comes from it).
    void LetGoOf(uint32_t win) {
        if (buttons_ && pressedOn_ == win) LetGo();
        for (const auto &[code, w] : keys_.Down())
            if (w == win) x_.Key(win, gsforward::XKeycode(code), 0, false);
        keys_.Released(win);
    }
    // `win`'s panel or popup went; for a popup's, `forWin` the window it's for, `owner` its app's
    // (0 for an app's own), `menu` whether it was override-redirect.
    void Gone(uint32_t win, uint32_t forWin, uint32_t owner, bool menu) {
        if (tagging_) {
            LetGoOf(win);
            x_.Untag(win);
        }
        overlays_.erase(win), lent_.erase(win);
        const bool typing = keys_.Target() == win;
        keys_.Gone(win);
        uint32_t before = 0;
        if (auto b = typedBefore_.find(win); b != typedBefore_.end()) before = b->second, typedBefore_.erase(b);
        for (auto &[popup, from] : typedBefore_)   // (a submenu's parent went first: the submenu goes back past it)
            if (from == win) from = before;
        for (auto b = typedBefore_.begin(); b != typedBefore_.end();) b = b->second ? std::next(b) : typedBefore_.erase(b);
        if (typing && owner) {   // a menu item picked, a dialog closed: back where it was
            auto known = [&](uint32_t w) { return w && w != win && book_.PanelOf(w) >= 0; };
            const uint32_t first = menu ? before : forWin, then = menu ? forWin : before;
            keys_.Focus(known(first) ? first : known(then) ? then : owner);
        }
        if (raised_ == win) raised_ = 0;
        if (focused_ == win) focused_ = 0;
        if (pressedOn_ == win) pressedOn_ = 0;
        if (blurredFrom_ == win) blurredFrom_ = 0;
    }
    void Do(const std::vector<Action> &actions) {
        for (const Action &a : actions) switch (a.type) {
                case Action::Make:
                    if (const uint64_t o = panels_.Make(a.index, a.win, pid_)) overlays_[a.win.id] = o, x_.Tag(a.win.id, o);
                    else Do(book_.Lose(a.win.id));
                    break;
                case Action::Show:
                    if (!lent_.count(a.win.id)) panels_.Show(a.index, a.win);   // (lent: Reclaim shows it)
                    break;
                case Action::Move: panels_.Move(a.index, a.win); break;
                case Action::Hide:
                    LetGoOf(a.win.id);
                    if (raised_ == a.win.id) raised_ = 0;   // (raised again when it's pointed at)
                    if (focused_ == a.win.id) focused_ = 0;   // (gamescope may have let go of it: again when it's pointed at)
                    panels_.Hide(a.index);
                    break;
                case Action::Drop: Gone(a.win.id, 0, 0, false), panels_.Drop(a.index); break;
                case Action::MakePopup:
                    if (const uint64_t o = panels_.MakePopup(a.index, a.sub, a.win, pid_)) {
                        overlays_[a.win.id] = o;
                        if (!lent_.count(book_.WindowOf(a.index))) x_.Tag(a.win.id, o);   // (lent: in the theater with its app)
                    } else {
                        Do(book_.Lose(a.win.id));
                    }
                    break;
                case Action::MovePopup: panels_.MovePopup(a.index, a.sub, a.win); break;
                case Action::DropPopup:
                    Gone(a.win.id, a.win.transientFor, book_.WindowOf(a.index), a.win.overrideRedirect);
                    panels_.DropPopup(a.index, a.sub);
                    break;
            }
    }
};

}  // namespace gsapps
