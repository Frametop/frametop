// gscontrol.h: each window's panel made and tagged, dropped and untagged; the laser raising the
// window it's on (a dialog's own); a press giving that window the typing, a click elsewhere taking
// it; keys only to a shown panel (PIN lock too); everything held let go of when it goes off.
//
//   g++ -std=c++17 -Iscreens screens/tests/gscontrol-test.cpp screens/gsforward.cpp $(pkg-config --cflags --libs xcb xcb-xtest xcb-icccm)
#include "gscontrol.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace gsapps;
using gsforward::Event;
using gsforward::Kind;
using gsforward::Window;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

struct FakeX : gsforward::XSide {
    std::map<uint32_t, uint64_t> tags;
    std::vector<std::string> log;
    std::set<int> held;
    void Tag(uint32_t w, uint64_t o) override { tags[w] = o; }
    void Untag(uint32_t w) override { tags.erase(w), log.push_back("untag " + std::to_string(w)); }
    void Pointer(int x, int y) override { log.push_back("at " + std::to_string(x) + "," + std::to_string(y)); }
    void Button(uint8_t b, bool down) override {
        down ? (void)held.insert(b) : (void)held.erase(b);
        log.push_back(std::string(down ? "down " : "up ") + std::to_string(b));
    }
    void Scroll(int n) override { log.push_back("scroll " + std::to_string(n)); }
    void Key(uint32_t w, uint8_t k, uint16_t st, bool down) override {
        log.push_back("key " + std::to_string(w) + " " + std::to_string(k) + " " + std::to_string(st) + (down ? " down" : " up"));
    }
    void Raise(uint32_t w) override { log.push_back("raise " + std::to_string(w)); }
    void Resize(uint32_t w, int a, int b) override { log.push_back("resize " + std::to_string(w) + " " + std::to_string(a) + "x" + std::to_string(b)); }
    void Focus(uint32_t w) override { log.push_back("focus " + std::to_string(w)); }
    int Count(const std::string &s) const {
        int n = 0;
        for (const auto &l : log) n += l == s;
        return n;
    }
    // where `a` was logged before `b` (both there)
    bool Before(const std::string &a, const std::string &b) const {
        int i = -1, j = -1;
        for (int k = 0; k < int(log.size()); ++k) {
            if (i < 0 && log[k] == a) i = k;
            if (j < 0 && log[k] == b) j = k;
        }
        return i >= 0 && j >= 0 && i < j;
    }
    bool Logged(const std::string &s) const {
        for (const auto &l : log)
            if (l == s) return true;
        return false;
    }
};

struct FakePanels : Panels {
    std::set<int> made, shown;
    std::map<std::pair<int, int>, uint64_t> popups;
    bool refuse = false, refusePopups = false;
    uint32_t pid = 0;
    uint64_t Make(int index, const Window &, uint32_t p) override {
        if (refuse) return 0;
        pid = p;
        made.insert(index);
        return 1000 + uint64_t(index);
    }
    void Show(int index, const Window &) override { shown.insert(index); }
    void Move(int, const Window &) override {}
    void Hide(int index) override { shown.erase(index); }
    void Drop(int index) override { made.erase(index), shown.erase(index); }
    uint64_t MakePopup(int index, int sub, const Window &, uint32_t) override {
        if (refusePopups) return 0;
        return popups[{index, sub}] = 5000 + uint64_t(sub);
    }
    void MovePopup(int, int, const Window &) override {}
    void DropPopup(int index, int sub) override { popups.erase({index, sub}); }
    bool Shown(int index) const override { return shown.count(index) != 0; }
};

static Window Win(uint32_t id, Kind kind, int w = 800, int h = 600, uint32_t transientFor = 0) {
    Window win;
    win.id = id, win.kind = kind, win.w = w, win.h = h, win.transientFor = transientFor;
    return win;
}
static Window Menu(uint32_t id, int w, int h, uint32_t transientFor = 0) {
    Window win = Win(id, Kind::Popup, w, h, transientFor);
    win.overrideRedirect = true;
    return win;
}
static Event Ev(Event::Type type, const Window &w) {
    Event e;
    e.type = type, e.win = w;
    return e;
}

int main() {
    // --- View in Theater: an app's window and its popups lent to SteamVR's theater (untagged, its
    // panel hidden), and back
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(2, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(9, Kind::Popup, 300, 200, 1)), 0), c.On(Ev(Event::Mapped, Win(9, Kind::Popup, 300, 200, 1)), 0);
        const uint64_t mainTag = x.tags[1], popupTag = x.tags[9];
        CHECK(mainTag && popupTag && p.shown.count(kFirst));
        c.Press(kFirst, 0, 1, true, 5, 5);   // held on it as it goes: let go of
        CHECK(c.Lend(kFirst) == 1 && c.Lent(kFirst) && !c.Lent(kFirst + 1));
        CHECK(!x.tags.count(1) && !x.tags.count(9) && x.tags.count(2) && !p.shown.count(kFirst) && x.held.empty());
        CHECK(c.Lend(kFirst) == 0 && c.Lend(kFirst + 5) == 0);   // (lent already; nothing there)
        // a popup it opens meanwhile: still SteamVR's (gamescope's overlay has it)
        c.On(Ev(Event::Created, Win(10, Kind::Popup, 300, 200, 1)), 2);
        CHECK(!p.shown.count(kFirst) && !x.tags.count(1) && !x.tags.count(10));
        // back: all tagged ours again with the same overlays, its panel shown, and the laser hands it gamescope's focus again
        x.log.clear();
        CHECK(c.Reclaim(1) && !c.Lent(kFirst));
        CHECK(x.tags[1] == mainTag && x.tags[9] == popupTag && x.tags.count(10) && p.shown.count(kFirst));
        c.Point(kFirst, 0, 3, 3);
        CHECK(x.Before("focus 1", "at 3,3"));
        CHECK(!c.Reclaim(1));   // (once)
        // lent, unmapped and mapped again: not shown, not tagged; back unmapped: tagged, shown when it maps
        c.Lend(kFirst + 1);
        c.On(Ev(Event::Unmapped, Win(2, Kind::Main)), 3), c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 3);
        CHECK(!p.shown.count(kFirst + 1) && !x.tags.count(2) && c.Lent(kFirst + 1));
        c.On(Ev(Event::Unmapped, Win(2, Kind::Main)), 3);
        CHECK(c.Reclaim(2) && x.tags.count(2) && !p.shown.count(kFirst + 1));
        c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 4);
        CHECK(p.shown.count(kFirst + 1));
        // lent and gone (the game closed in the theater): nothing to take back
        c.Lend(kFirst);
        c.On(Ev(Event::Gone, Win(1, Kind::Main)), 5);
        CHECK(!c.Reclaim(1) && !x.tags.count(1));
        // a new window on the same panel isn't lent
        c.On(Ev(Event::Created, Win(3, Kind::Main)), 6), c.On(Ev(Event::Mapped, Win(3, Kind::Main)), 6);
        CHECK(c.Windows().PanelOf(3) == kFirst && !c.Lent(kFirst) && x.tags.count(3) && p.shown.count(kFirst));
        // off with one lent: nothing kept, nothing tagged
        CHECK(c.Lend(kFirst) == 3);
        c.Off();
        CHECK(!c.Lent(kFirst) && x.tags.empty() && c.Windows().Panels() == 0);
    }
    // --- gamescope's focus: its pointer goes only to a window it focused, and it never focuses a
    // tagged one (Slay the Spire 2's clicks went nowhere, 2026-11-18)
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(2, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 0);
        c.On(Ev(Event::Created, Menu(9, 100, 80, 1)), 0);   // app 1's menu
        // the laser onto an app first: that app focused, before the pointer moves
        c.Point(kFirst, 0, 10, 20);
        CHECK(x.Before("focus 1", "at 10,20"));
        // moving on it, and onto another app, focuses nothing more: gamescope's pointer stays on
        // its display, and X finds the window under it
        c.Point(kFirst, 0, 11, 20), c.Point(kFirst + 1, 0, 5, 5);
        CHECK(x.Count("focus 1") == 1 && !x.Logged("focus 2"));
        // a press on the other app focuses it (a window manager's click to focus), before the button
        x.log.clear();
        c.Press(kFirst + 1, 0, 1, true, 5, 5);
        CHECK(x.Before("focus 2", "down 1"));
        c.Press(kFirst + 1, 0, 1, false, 5, 5);
        c.Press(kFirst + 1, 0, 1, true, 6, 5), c.Press(kFirst + 1, 0, 1, false, 6, 5);
        CHECK(x.Count("focus 2") == 1);   // (once)
        // gamescope didn't focus it: moves over it ask nothing, the next press on it asks again,
        // before the button, and once
        x.log.clear();
        c.Unfocused(2);
        c.Point(kFirst + 1, 0, 7, 5), c.Point(kFirst, 0, 3, 3), c.Point(kFirst + 1, 0, 8, 5);
        CHECK(!x.Logged("focus 2"));
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Before("focus 2", "down 1") && x.Count("focus 2") == 1);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 1);   // (it worked that time: no more)
        // ...a window that isn't the one asked for changes nothing
        c.Unfocused(1), c.Unfocused(0);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 1 && !x.Logged("focus 1"));
        // refused, asked again at a press, and that one goes through: as often as that happens
        x.log.clear();
        c.Focused(2);
        for (int i = 0; i < 5; ++i) {
            c.Unfocused(2);
            c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
            c.Focused(2);
        }
        CHECK(x.Count("focus 2") == 5);
        // gamescope keeps refusing (another game in the theater has SteamVR's input): after
        // kFocusRefusals in a row a press asks no more, and each ask costs the panel its picture
        x.log.clear();
        for (int i = 0; i < Control::kFocusRefusals + 3; ++i) {
            c.Unfocused(2);
            c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        }
        CHECK(x.Count("focus 2") == Control::kFocusRefusals - 1 && x.Count("down 1") == Control::kFocusRefusals + 3);
        // a press on a menu doesn't take the focus from its app's window
        x.log.clear();
        c.Press(kFirst, 1, 1, true, 30, 30), c.Press(kFirst, 1, 1, false, 30, 30);
        CHECK(!x.Logged("focus 1") && !x.Logged("focus 9") && x.Logged("down 1"));
        // a window past kFocusRefusals isn't left for good: what made gamescope refuse can end (the
        // other game leaves the theater) with nothing said here, so once kFocusRetryMs have passed
        // a press asks again, once for each such wait while it's still refused. (After the menu's
        // check: the time that passes drops that menu, which never mapped.)
        x.log.clear();
        int64_t now = 5000;
        c.Tick(now);
        c.Unfocused(2);   // (the last refusal, at a time the test knows)
        c.Tick(now += Control::kFocusRetryMs - 1);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 0);
        c.Tick(now += 1);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 1);
        c.Unfocused(2);   // still refused: the next wait starts
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        c.Tick(now += Control::kFocusRetryMs - 1);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 1);
        c.Tick(now += 1);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 2);
        // ...and this one goes through: the window is asked for at once the next time it's refused
        c.Focused(2);
        c.Unfocused(2);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == 3 && x.Count("down 1") == 6);
        // refused past kFocusRefusals again, and the theater's game changes: no wait, the next
        // press asks (and only that changes it: the press before it didn't)
        for (int i = 0; i < Control::kFocusRefusals; ++i) {
            c.Unfocused(2);
            c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        }
        const int asked = x.Count("focus 2");
        c.Unfocused(2);
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == asked);
        c.TheaterChanged();
        c.Press(kFirst + 1, 0, 1, true, 8, 5), c.Press(kFirst + 1, 0, 1, false, 8, 5);
        CHECK(x.Count("focus 2") == asked + 1);
        c.Focused(2);
        // the window we focused unmaps: the laser coming back focuses again, once it maps
        x.log.clear();
        c.On(Ev(Event::Unmapped, Win(2, Kind::Main)), 1);
        c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 2);
        c.Point(kFirst + 1, 0, 7, 7);
        CHECK(x.Before("focus 2", "at 7,7"));
        // ...and when it goes, the next app pointed at
        x.log.clear();
        c.On(Ev(Event::Gone, Win(2, Kind::Main)), 3);
        c.Point(kFirst, 0, 8, 8);
        CHECK(x.Before("focus 1", "at 8,8"));
        // the display went and came back: none focused by us there yet
        c.Closed();
        x.log.clear();
        c.On(Ev(Event::Created, Win(3, Kind::Main)), 4), c.On(Ev(Event::Mapped, Win(3, Kind::Main)), 4);
        c.Point(kFirst, 0, 1, 1);
        CHECK(x.Logged("focus 3"));
    }
    // --- panels and tags
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.Opened(77);
        c.On(Ev(Event::Created, Win(10, Kind::Main)), 0);
        CHECK(p.made.count(kFirst) && x.tags[10] == 1000 + uint64_t(kFirst) && p.pid == 77);
        c.On(Ev(Event::Mapped, Win(10, Kind::Main)), 0);
        CHECK(p.shown.count(kFirst));
        // unmapped: hidden, still tagged (a remap shows it at once: no gamescope panel between)
        c.On(Ev(Event::Unmapped, Win(10, Kind::Main)), 0);
        CHECK(p.made.count(kFirst) && !p.shown.count(kFirst) && x.tags.count(10));
        c.On(Ev(Event::Mapped, Win(10, Kind::Main)), 1);
        CHECK(p.made.count(kFirst) && p.shown.count(kFirst) && x.tags[10] == 1000 + uint64_t(kFirst));
        c.On(Ev(Event::Gone, Win(10, Kind::Main)), 2);
        CHECK(!p.made.count(kFirst) && !x.tags.count(10));
        // a panel that couldn't be made: the window forgotten (its index free for the next)
        p.refuse = true;
        c.On(Ev(Event::Created, Win(11, Kind::Main)), 0);
        CHECK(c.Windows().Panels() == 0 && !x.tags.count(11));
        p.refuse = false;
        c.On(Ev(Event::Created, Win(12, Kind::Main)), 0);
        CHECK(c.Windows().WindowOf(kFirst) == 12);
        // the display went: panels gone, nothing untagged (nobody to tell)
        x.log.clear();
        c.Closed();
        CHECK(p.made.empty() && !x.Logged("untag 12"));
    }
    // --- the laser, a dialog, the typing
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(2, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(9, Kind::Popup, 300, 200, 1)), 0);   // app 1's dialog
        CHECK(p.popups.count({kFirst, 1}) && x.tags[9] == 5001);
        c.Point(kFirst + 1, 0, 10, 20);
        CHECK(x.Logged("raise 2") && x.Logged("at 10,20"));
        x.log.clear();
        c.Point(kFirst + 1, 0, 11, 20);
        CHECK(!x.Logged("raise 2"));   // (raised once)
        // a click on the dialog raises the dialog, not its app's window, and gives it the typing
        // (council, 2026-11-16: the main window was raised over it)
        x.log.clear();
        c.Press(kFirst, 1, 1, true, 50, 60);
        CHECK(x.Logged("raise 9") && !x.Logged("raise 1") && x.Logged("down 1"));
        c.Press(kFirst, 1, 1, false, 50, 60);
        CHECK(c.Typing());
        CHECK(c.TypingPanel() == kFirst);   // (the dialog's: its app's panel, where the gamepad pill goes)
        CHECK(c.Key(30, 1, false, {}) && x.Logged("key 9 38 0 down"));
        CHECK(c.Key(30, 0, false, {}));
        // Shift held: its own press unshifted, a letter shifted
        x.log.clear();
        c.Key(42, 1, false, gsforward::Mods{true, false, false, false});
        c.Key(30, 1, false, gsforward::Mods{true, false, false, false});
        CHECK(x.Logged("key 9 50 0 down") && x.Logged("key 9 38 1 down"));
        c.Key(30, 0, false, gsforward::Mods{true, false, false, false}), c.Key(42, 0, false, {});
        // frunner's search has the typing: a press doesn't go
        CHECK(!c.Key(31, 1, true, {}));
        // a click on Steam: the typing leaves the apps (council, 2026-11-16)
        c.Blur();
        CHECK(!c.Typing() && !c.Key(30, 1, false, {}) && c.TypingPanel() == -1);
        // a key down when it went keeps going to the app until it's up
        c.Press(kFirst + 1, 0, 1, true, 1, 1), c.Press(kFirst + 1, 0, 1, false, 1, 1);
        CHECK(c.Key(32, 1, false, {}));
        c.Blur();
        CHECK(c.Key(32, 0, false, {}) && x.Logged("key 2 40 0 up"));
        // its panel hidden (the dashboard closed, PIN locked): no typing
        c.Press(kFirst + 1, 0, 1, true, 1, 1), c.Press(kFirst + 1, 0, 1, false, 1, 1);
        CHECK(c.TypingPanel() == kFirst + 1);
        p.shown.erase(kFirst + 1);
        CHECK(!c.Typing() && !c.Key(33, 1, false, {}) && c.TypingPanel() == -1);
        p.shown.insert(kFirst + 1);
        // a window that goes takes its typing
        c.On(Ev(Event::Gone, Win(2, Kind::Main)), 0);
        CHECK(!c.Typing());
        c.Resize(kFirst, 640, 480);
        CHECK(x.Logged("resize 1 640x480"));
    }
    // --- a menu or dialog going (council pass 2, 2026-11-16): the typing back to its app, nothing left held
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.Press(kFirst, 0, 1, true, 1, 1), c.Press(kFirst, 0, 1, false, 1, 1);   // the editor clicked
        c.On(Ev(Event::Created, Win(7, Kind::Popup, 200, 100, 1)), 0), c.On(Ev(Event::Mapped, Win(7, Kind::Popup, 200, 100, 1)), 0);
        c.Press(kFirst, 1, 1, true, 5, 5), c.Press(kFirst, 1, 1, false, 5, 5);   // a menu item
        c.On(Ev(Event::Unmapped, Win(7, Kind::Popup)), 0);
        x.log.clear();
        CHECK(c.Typing() && c.Key(30, 1, false, {}) && x.Logged("key 1 38 0 down"));
        c.Key(30, 0, false, {});
        // a dialog that goes with the button still down on it, and a key down on it
        c.On(Ev(Event::Created, Win(8, Kind::Popup, 300, 200, 1)), 0), c.On(Ev(Event::Mapped, Win(8, Kind::Popup, 300, 200, 1)), 0);
        c.Press(kFirst, 2, 1, true, 5, 5);
        c.Key(28, 1, false, {});   // Enter, held
        x.log.clear();
        c.On(Ev(Event::Gone, Win(8, Kind::Popup)), 0);
        CHECK(!c.Holding() && x.held.empty() && x.Logged("key 8 36 0 up"));
        CHECK(!c.Key(28, 0, false, {}));   // (its own release later: let go of already)
        CHECK(c.Typing());
        // a key held on the app while it unmaps (and maps again): its release went
        c.Key(17, 1, false, {});
        x.log.clear();
        c.On(Ev(Event::Unmapped, Win(1, Kind::Main)), 0);
        CHECK(x.Logged("key 1 25 0 up") && !c.Typing());
        CHECK(!c.Key(17, 0, false, {}));   // (its own release later: let go of already)
        c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 1);
        CHECK(c.Typing());   // (the typing stayed with it)
    }
    // --- a dialog's own menu (council pass 3): the typing back to the dialog, not the app behind it
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.Press(kFirst, 0, 1, true, 1, 1), c.Press(kFirst, 0, 1, false, 1, 1);
        // a dialog the app opens takes the typing (as a window manager gives it)
        c.On(Ev(Event::Created, Win(2, Kind::Popup, 400, 300, 1)), 0), c.On(Ev(Event::Mapped, Win(2, Kind::Popup, 400, 300, 1)), 0);
        x.log.clear();
        CHECK(c.Key(30, 1, false, {}) && x.Logged("key 2 38 0 down"));
        c.Key(30, 0, false, {});
        // its right-click menu (override-redirect: for nobody), an item picked
        c.On(Ev(Event::Created, Menu(3, 150, 80)), 0), c.On(Ev(Event::Mapped, Menu(3, 150, 80)), 0);
        const int menuSub = 2;
        CHECK(c.Windows().PopupWindow(kFirst, menuSub) == 3);
        c.Press(kFirst, menuSub, 1, true, 5, 5), c.Press(kFirst, menuSub, 1, false, 5, 5);
        c.On(Ev(Event::Unmapped, Win(3, Kind::Popup)), 0);
        x.log.clear();
        CHECK(c.Key(31, 1, false, {}) && x.Logged("key 2 39 0 down"));
        c.Key(31, 0, false, {});
        // its dropdown (transient for it), the dialog clicked by the app first: back to the dialog
        c.On(Ev(Event::Created, Win(4, Kind::Popup, 150, 80, 2)), 0), c.On(Ev(Event::Mapped, Win(4, Kind::Popup, 150, 80, 2)), 0);
        c.On(Ev(Event::Gone, Win(4, Kind::Popup)), 0);
        x.log.clear();
        CHECK(c.Key(32, 1, false, {}) && x.Logged("key 2 40 0 down"));
        c.Key(32, 0, false, {});
        // the dialog closed: back to the app
        c.On(Ev(Event::Gone, Win(2, Kind::Popup)), 0);
        x.log.clear();
        CHECK(c.Key(33, 1, false, {}) && x.Logged("key 1 41 0 down"));
        c.Key(33, 0, false, {});
    }
    // --- council pass 4: a tooltip doesn't take the typing; a dialog closed gives it to the window
    // it's for, not the one clicked before it; a menu picked after a Blur goes back to the dialog
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.Press(kFirst, 0, 1, true, 1, 1), c.Press(kFirst, 0, 1, false, 1, 1);
        // a tooltip (override-redirect, transient for the app, as GTK's and Qt's are)
        c.On(Ev(Event::Created, Menu(5, 80, 40, 1)), 0), c.On(Ev(Event::Mapped, Menu(5, 80, 40, 1)), 0);
        x.log.clear();
        CHECK(c.Key(30, 1, false, {}) && x.Logged("key 1 38 0 down"));
        c.Key(30, 0, false, {});
        c.On(Ev(Event::Unmapped, Menu(5, 80, 40)), 0);
        // app 1's dialog; app 2 clicked; the dialog's OK clicked, it closes: back to app 1
        c.On(Ev(Event::Created, Win(2, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(2, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(3, Kind::Popup, 400, 300, 1)), 0), c.On(Ev(Event::Mapped, Win(3, Kind::Popup, 400, 300, 1)), 0);
        c.Press(kFirst + 1, 0, 1, true, 1, 1), c.Press(kFirst + 1, 0, 1, false, 1, 1);
        uint32_t dialogSub = 0;
        for (int s = 1; s < 10 && !dialogSub; ++s)
            if (c.Windows().PopupWindow(kFirst, s) == 3) dialogSub = uint32_t(s);
        CHECK(dialogSub != 0);
        c.Press(kFirst, int(dialogSub), 1, true, 5, 5), c.Press(kFirst, int(dialogSub), 1, false, 5, 5);
        c.On(Ev(Event::Gone, Win(3, Kind::Popup)), 0);
        x.log.clear();
        CHECK(c.Key(31, 1, false, {}) && x.Logged("key 1 39 0 down"));
        c.Key(31, 0, false, {});
        // a dialog again, its menu open; a click on a Steam panel (Blur); a menu item picked
        c.On(Ev(Event::Created, Win(6, Kind::Popup, 400, 300, 1)), 0), c.On(Ev(Event::Mapped, Win(6, Kind::Popup, 400, 300, 1)), 0);
        c.On(Ev(Event::Created, Menu(7, 150, 80)), 0), c.On(Ev(Event::Mapped, Menu(7, 150, 80)), 0);
        uint32_t menuSub = 0;
        for (int s = 1; s < 20 && !menuSub; ++s)
            if (c.Windows().PopupWindow(kFirst, s) == 7) menuSub = uint32_t(s);
        CHECK(menuSub != 0);
        c.Blur();
        c.Press(kFirst, int(menuSub), 1, true, 5, 5), c.Press(kFirst, int(menuSub), 1, false, 5, 5);
        c.On(Ev(Event::Unmapped, Menu(7, 150, 80)), 0);
        x.log.clear();
        CHECK(c.Key(32, 1, false, {}) && x.Logged("key 6 40 0 down"));
        c.Key(32, 0, false, {});
    }
    // --- council pass 5: a press on an app after a Blur ends what the Blur left: a menu picked
    // later goes back to its own app, not the one typed in before the Blur
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        for (uint32_t id : {1u, 2u, 3u}) c.On(Ev(Event::Created, Win(id, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(id, Kind::Main)), 0);
        const int i1 = c.Windows().PanelOf(1), i2 = c.Windows().PanelOf(2), i3 = c.Windows().PanelOf(3);
        c.Press(i1, 0, 1, true, 1, 1), c.Press(i1, 0, 1, false, 1, 1);
        c.Blur();
        c.Press(i2, 0, 1, true, 1, 1), c.Press(i2, 0, 1, false, 1, 1);
        c.On(Ev(Event::Gone, Win(2, Kind::Main)), 0);   // app 2 quits: nothing has the typing
        c.Point(i3, 0, 1, 1);
        c.On(Ev(Event::Created, Menu(9, 150, 80)), 0), c.On(Ev(Event::Mapped, Menu(9, 150, 80)), 0);
        uint32_t sub = 0;
        for (int s = 1; s < 10 && !sub; ++s)
            if (c.Windows().PopupWindow(i3, s) == 9) sub = uint32_t(s);
        CHECK(sub != 0);
        c.Press(i3, int(sub), 1, true, 5, 5), c.Press(i3, int(sub), 1, false, 5, 5);
        c.On(Ev(Event::Unmapped, Menu(9, 150, 80)), 0);
        x.log.clear();
        CHECK(c.Key(30, 1, false, {}) && x.Logged("key 3 38 0 down"));
        c.Key(30, 0, false, {});
    }
    // --- follow-ups: a submenu chain unmapping parent first; a dialog mapped before its menu
    // unmaps; a dialog's dialog over its own app's panel
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        for (uint32_t id : {1u, 2u}) c.On(Ev(Event::Created, Win(id, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(id, Kind::Main)), 0);
        const int i1 = c.Windows().PanelOf(1), i2 = c.Windows().PanelOf(2);
        auto subOf = [&](int index, uint32_t win) {
            for (int s = 1; s < 64; ++s)
                if (c.Windows().PopupWindow(index, s) == win) return s;
            return 0;
        };
        auto click = [&](int index, int sub) { c.Press(index, sub, 1, true, 5, 5), c.Press(index, sub, 1, false, 5, 5); };
        auto keyTo = [&](uint32_t win) {
            x.log.clear();
            const bool went = c.Key(30, 1, false, {}) && x.Logged("key " + std::to_string(win) + " 38 0 down");
            c.Key(30, 0, false, {});
            return went;
        };
        // N2d: dialog 6, its menu 7, the submenu 8; an item picked: 7 unmaps, then 8: back to the dialog
        c.On(Ev(Event::Created, Win(6, Kind::Popup, 400, 300, 1)), 0), c.On(Ev(Event::Mapped, Win(6, Kind::Popup, 400, 300, 1)), 0);
        click(i1, subOf(i1, 6));
        c.On(Ev(Event::Created, Menu(7, 150, 80)), 0), c.On(Ev(Event::Mapped, Menu(7, 150, 80)), 0);
        click(i1, subOf(i1, 7));
        c.On(Ev(Event::Created, Menu(8, 150, 80, 7)), 0), c.On(Ev(Event::Mapped, Menu(8, 150, 80, 7)), 0);
        CHECK(subOf(i1, 8) != 0);
        click(i1, subOf(i1, 8));
        c.On(Ev(Event::Unmapped, Menu(7, 150, 80)), 0);
        c.On(Ev(Event::Unmapped, Menu(8, 150, 80, 7)), 0);
        CHECK(keyTo(6));
        c.On(Ev(Event::Gone, Win(6, Kind::Popup, 400, 300, 1)), 0);
        CHECK(keyTo(1));
        // N6: app 1's menu clicked, its item opens dialog 3, mapped before the menu unmaps: the dialog types
        c.On(Ev(Event::Created, Menu(7, 150, 80)), 0), c.On(Ev(Event::Mapped, Menu(7, 150, 80)), 0);
        click(i1, subOf(i1, 7));
        c.On(Ev(Event::Created, Win(3, Kind::Popup, 400, 300, 1)), 0), c.On(Ev(Event::Mapped, Win(3, Kind::Popup, 400, 300, 1)), 0);
        c.On(Ev(Event::Unmapped, Menu(7, 150, 80)), 0);
        CHECK(keyTo(3));
        // N5: the dialog's own dialog, with the laser last on app 2: over app 1's panel
        c.Point(i2, 0, 1, 1);
        c.On(Ev(Event::Created, Win(4, Kind::Popup, 200, 100, 3)), 0), c.On(Ev(Event::Mapped, Win(4, Kind::Popup, 200, 100, 3)), 0);
        CHECK(c.Windows().PanelOf(4) == i1);
        CHECK(keyTo(4));
        c.On(Ev(Event::Gone, Win(4, Kind::Popup, 200, 100, 3)), 0);
        CHECK(keyTo(3));
        c.On(Ev(Event::Gone, Win(3, Kind::Popup, 400, 300, 1)), 0);
        CHECK(keyTo(1));
    }
    // --- off: buttons and keys held let go of, windows untagged
    {
        FakeX x;
        FakePanels p;
        Control c(x, p);
        c.On(Ev(Event::Created, Win(1, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
        c.Press(kFirst, 0, 1, true, 1, 1);
        c.Key(17, 1, false, {});   // W held in a game
        CHECK(c.Holding() && x.held.count(1));
        c.Off();
        CHECK(!c.Holding() && x.held.empty() && x.Logged("key 1 25 0 up") && x.Logged("untag 1") && p.made.empty());
        // popups that can't have overlays: forgotten
        p.refusePopups = true;
        c.On(Ev(Event::Created, Win(3, Kind::Main)), 0), c.On(Ev(Event::Mapped, Win(3, Kind::Main)), 0);
        c.On(Ev(Event::Created, Win(4, Kind::Popup, 100, 50)), 0);
        CHECK(c.Windows().PanelOf(4) == -1 && !x.tags.count(4));
    }
    if (failures) return std::printf("gscontrol-test: %d failed\n", failures), 1;
    std::printf("gscontrol-test: ok\n");
    return 0;
}
