// gscontrol.h: each window's panel made and tagged, dropped and untagged; the laser raising the
// window it's on (a dialog's own); a press giving that window the typing, a click elsewhere taking
// it; keys only to a shown panel (PIN lock too); everything held let go of when it goes off.
//
//   g++ -std=c++17 -Iscreens screens/tests/gscontrol-test.cpp
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
static Event Ev(Event::Type type, const Window &w) {
    Event e;
    e.type = type, e.win = w;
    return e;
}

int main() {
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
        c.On(Ev(Event::Unmapped, Win(10, Kind::Main)), 0);
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
        CHECK(!c.Typing() && !c.Key(30, 1, false, {}));
        // a key down when it went keeps going to the app until it's up
        c.Press(kFirst + 1, 0, 1, true, 1, 1), c.Press(kFirst + 1, 0, 1, false, 1, 1);
        CHECK(c.Key(32, 1, false, {}));
        c.Blur();
        CHECK(c.Key(32, 0, false, {}) && x.Logged("key 2 40 0 up"));
        // its panel hidden (the dashboard closed, PIN locked): no typing
        c.Press(kFirst + 1, 0, 1, true, 1, 1), c.Press(kFirst + 1, 0, 1, false, 1, 1);
        p.shown.erase(kFirst + 1);
        CHECK(!c.Typing() && !c.Key(33, 1, false, {}));
        p.shown.insert(kFirst + 1);
        // a window that goes takes its typing
        c.On(Ev(Event::Gone, Win(2, Kind::Main)), 0);
        CHECK(!c.Typing());
        c.Resize(kFirst, 640, 480);
        CHECK(x.Logged("resize 1 640x480"));
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
