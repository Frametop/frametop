// The gamescope apps (vr.cpp's "gamescope apps"), between the X side and the panels: what
// gsapps::Book says to do with each window, done (a panel made, its window tagged; dropped,
// untagged), and the laser and the keys on an app's panel as X's. Pure: the X side
// (gsforward::XSide: gslink.h's AsyncLink) and the panels (Panels: vr.cpp's, on OpenVR) come in,
// so its test (screens/tests/gscontrol-test.cpp) runs with fakes of both.
//
// The laser on a panel or a popup over it moves the X pointer there, and raises that window
// first: apps overlap on gamescope's one screen, and the X pointer goes to the top one. A press
// gives that window the typing (a dialog's own, not its app's), until a click somewhere that isn't
// an app (Blur). A dialog the window with the typing opens takes it, as a window manager gives it
// one (not a menu or a tooltip: override-redirect, a window manager never focuses those). When a
// dialog with the typing goes, the window it's for has it again (as a window manager does), else
// the one that had it before, else its app's; when a menu goes, the one that had it before (a
// dialog's menu: the dialog), else the one it's for, else its app's. Keys go only
// while the panel of the window with the typing is shown. A window that goes takes nothing held
// with it: a button pressed on it and keys down on it are let go of.
#pragma once

#include "gsapps.h"
#include "gsforward.h"

#include <cstdint>
#include <map>
#include <vector>

namespace gsapps {

// The panels, as vr.cpp makes them.
struct Panels {
    virtual ~Panels() = default;
    // A hidden panel for an app's window at `index`: its overlay's handle, gamescope (`pid`) its
    // renderer; 0 if it couldn't (the window keeps gamescope's own panel).
    virtual uint64_t Make(int index, const gsforward::Window &w, uint32_t pid) = 0;
    virtual void Show(int index, const gsforward::Window &w) = 0;   // it mapped: named, sized, placed
    virtual void Move(int index, const gsforward::Window &w) = 0;   // its size or place on X's screen
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
        buttons_ = 0, raised_ = 0, pressedOn_ = 0, blurredFrom_ = 0, keys_ = Keys();
        typedBefore_.clear();
    }
    // Off (GSAPP_PANELS, or ft-screens going): buttons and keys held let go of, each window back to
    // gamescope's own panel.
    void Off() {
        LetGo();
        for (const auto &[code, win] : keys_.Down()) x_.Key(win, gsforward::XKeycode(code), 0, false);
        keys_ = Keys();
        Do(book_.Clear());
        raised_ = 0, pressedOn_ = 0, blurredFrom_ = 0;
        typedBefore_.clear();
    }
    void On(const gsforward::Event &e, int64_t nowMs) {
        Do(book_.On(e, nowMs));
        // a dialog for the window with the typing: the typing goes to it
        const uint32_t t = keys_.Target();
        if (e.type == gsforward::Event::Mapped && !e.win.overrideRedirect && e.win.transientFor && t &&
            t == e.win.transientFor && book_.PanelOf(e.win.id) >= 0)
            TypeIn(e.win.id);
    }
    void Tick(int64_t nowMs) { Do(book_.Tick(nowMs)); }

    // The laser at (x, y) on X's screen, on the panel at `index` (sub 0) or its popup `sub`.
    void Point(int index, int sub, int x, int y) {
        const uint32_t w = WindowAt(index, sub);
        if (!w) return;
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
            else keys_.Focus(w);
            pressedOn_ = w;
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

private:
    gsforward::XSide &x_;
    Panels &panels_;
    Book book_;
    Keys keys_;
    uint32_t pid_ = 0;
    uint32_t raised_ = 0;     // the window raised last
    uint32_t buttons_ = 0;    // X buttons held (bit b: button b)
    uint32_t pressedOn_ = 0;  // the window the last press went to
    // a popup that had the typing -> the app window that had it before (erased when the popup goes)
    std::map<uint32_t, uint32_t> typedBefore_;
    uint32_t blurredFrom_ = 0;   // the app window that had the typing at the last Blur
    bool tagging_ = true;     // (false: the display's gone, nobody to untag at)

    uint32_t WindowAt(int index, int sub) const { return sub ? book_.PopupWindow(index, sub) : book_.WindowOf(index); }
    bool TargetShown() const {
        const int index = keys_.Target() ? book_.PanelOf(keys_.Target()) : -1;
        return index >= 0 && panels_.Shown(index);
    }
    // The typing to popup `w`, the app window it leaves remembered (off the apps: the one that had
    // it before the Blur, which X still has focused).
    void TypeIn(uint32_t w) {
        if (const uint32_t t = keys_.Target() ? keys_.Target() : blurredFrom_; t && t != w) typedBefore_[w] = t;
        keys_.Focus(w);
    }
    // `win`'s panel or popup went; for a popup's, `forWin` the window it's for, `owner` its app's
    // (0 for an app's own), `menu` whether it was override-redirect.
    void Gone(uint32_t win, uint32_t forWin, uint32_t owner, bool menu) {
        if (tagging_) {
            if (buttons_ && pressedOn_ == win) LetGo();   // (its overlay's gone: no release comes from it)
            for (const auto &[code, w] : keys_.Down())
                if (w == win) x_.Key(win, gsforward::XKeycode(code), 0, false);
            x_.Untag(win);
        }
        const bool typing = keys_.Target() == win;
        keys_.Gone(win);
        uint32_t before = 0;
        if (auto b = typedBefore_.find(win); b != typedBefore_.end()) before = b->second, typedBefore_.erase(b);
        if (typing && owner) {   // a menu item picked, a dialog closed: back where it was
            auto known = [&](uint32_t w) { return w && w != win && book_.PanelOf(w) >= 0; };
            const uint32_t first = menu ? before : forWin, then = menu ? forWin : before;
            keys_.Focus(known(first) ? first : known(then) ? then : owner);
        }
        if (raised_ == win) raised_ = 0;
        if (pressedOn_ == win) pressedOn_ = 0;
        if (blurredFrom_ == win) blurredFrom_ = 0;
    }
    void Do(const std::vector<Action> &actions) {
        for (const Action &a : actions) switch (a.type) {
                case Action::Make:
                    if (const uint64_t o = panels_.Make(a.index, a.win, pid_)) x_.Tag(a.win.id, o);
                    else Do(book_.Lose(a.win.id));
                    break;
                case Action::Show: panels_.Show(a.index, a.win); break;
                case Action::Move: panels_.Move(a.index, a.win); break;
                case Action::Drop: Gone(a.win.id, 0, 0, false), panels_.Drop(a.index); break;
                case Action::MakePopup:
                    if (const uint64_t o = panels_.MakePopup(a.index, a.sub, a.win, pid_)) x_.Tag(a.win.id, o);
                    else Do(book_.Lose(a.win.id));
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
