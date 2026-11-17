// gsapps.h: app windows to panels (made at create, shown at map, dropped at unmap, the never
// mapped after a while), their popups over the right panel, and keys going up where they went down.
//
//   g++ -std=c++17 -Iscreens screens/tests/gsapps-test.cpp
#include "gsapps.h"

#include <cstdio>

using namespace gsapps;
using gsforward::Event;
using gsforward::Kind;
using gsforward::Window;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

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
static bool One(const std::vector<Action> &a, Action::Type type, int index) {
    return a.size() == 1 && a[0].type == type && a[0].index == index;
}

static void Panels() {
    Book b;
    CHECK(One(b.On(Ev(Event::Created, Win(10, Kind::Main)), 0), Action::Make, kFirst));
    CHECK(b.On(Ev(Event::Created, Win(11, Kind::Ignore)), 0).empty());
    CHECK(One(b.On(Ev(Event::Mapped, Win(10, Kind::Main)), 10), Action::Show, kFirst));
    CHECK(b.WindowOf(kFirst) == 10);
    Window moved = Win(10, Kind::Main, 1024, 768);
    auto a = b.On(Ev(Event::Moved, moved), 20);
    CHECK(One(a, Action::Move, kFirst) && a[0].win.w == 1024);
    CHECK(b.MainRect(kFirst) && b.MainRect(kFirst)->h == 768);
    // a helper that grows before it maps gets one at the map
    a = b.On(Ev(Event::Mapped, Win(11, Kind::Main)), 30);
    CHECK(a.size() == 2 && a[0].type == Action::Make && a[0].index == kFirst + 1 && a[1].type == Action::Show);
    // unmapped: hidden, its panel (and tag) kept; mapped again: shown at once, nothing made anew
    CHECK(One(b.On(Ev(Event::Unmapped, Win(10, Kind::Main)), 40), Action::Hide, kFirst));
    CHECK(b.WindowOf(kFirst) == 10 && b.Panels() == 2);
    CHECK(One(b.On(Ev(Event::Mapped, Win(10, Kind::Main)), 45), Action::Show, kFirst));
    // unmapped and not mapped again: dropped after a while, and its index free again
    CHECK(One(b.On(Ev(Event::Unmapped, Win(10, Kind::Main)), 50), Action::Hide, kFirst));
    CHECK(b.Tick(50 + kMapWaitMs).empty());
    CHECK(One(b.Tick(51 + kMapWaitMs), Action::Drop, kFirst));
    CHECK(b.On(Ev(Event::Gone, Win(10, Kind::Main)), 52 + kMapWaitMs).empty());
    // unmapped, then gone: dropped at once
    CHECK(One(b.On(Ev(Event::Unmapped, Win(11, Kind::Main)), 60), Action::Hide, kFirst + 1));
    CHECK(One(b.On(Ev(Event::Gone, Win(11, Kind::Main)), 61), Action::Drop, kFirst + 1));
    const int64_t t = 100 + kMapWaitMs;
    CHECK(One(b.On(Ev(Event::Created, Win(12, Kind::Main)), t), Action::Make, kFirst));
    // created, never mapped: dropped after a while
    CHECK(b.Tick(t + kMapWaitMs).empty());
    CHECK(One(b.Tick(t + 1 + kMapWaitMs), Action::Drop, kFirst));
    // kMax at most
    Book full;
    for (int i = 0; i < kMax; ++i) CHECK(One(full.On(Ev(Event::Created, Win(100 + i, Kind::Main)), 0), Action::Make, kFirst + i));
    CHECK(full.On(Ev(Event::Created, Win(200, Kind::Main)), 0).empty());
    CHECK(full.Clear().size() == size_t(kMax) && full.Panels() == 0);
    CHECK(Book::Ours(kFirst) && Book::Ours(kFirst + kMax - 1) && !Book::Ours(kFirst + kMax) && !Book::Ours(100));
}

static void Popups() {
    Book b;
    b.On(Ev(Event::Created, Win(1, Kind::Main)), 0), b.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
    b.On(Ev(Event::Created, Win(2, Kind::Main)), 0), b.On(Ev(Event::Mapped, Win(2, Kind::Main)), 0);
    CHECK(b.On(Ev(Event::Created, Win(9, Kind::Popup, 100, 50)), 0).size() == 1);   // the first panel's
    CHECK(b.PopupWindow(kFirst, 1) == 9);
    b.Pointed(kFirst + 1);
    auto a = b.On(Ev(Event::Created, Win(8, Kind::Popup, 100, 50)), 0);   // a menu: where the pointer was
    CHECK(a.size() == 1 && a[0].type == Action::MakePopup && a[0].index == kFirst + 1 && a[0].sub == 2);
    a = b.On(Ev(Event::Created, Win(7, Kind::Popup, 300, 200, 1)), 0);    // a dialog: its window's
    CHECK(a.size() == 1 && a[0].index == kFirst);
    Window at = Win(8, Kind::Popup, 120, 60);
    at.x = 5, at.y = 6;
    a = b.On(Ev(Event::Moved, at), 0);
    CHECK(One(a, Action::MovePopup, kFirst + 1) && a[0].win.x == 5);
    CHECK(b.PopupRect(kFirst + 1, 2) && b.PopupRect(kFirst + 1, 2)->w == 120);
    // its window goes: its popups first
    a = b.On(Ev(Event::Gone, Win(2, Kind::Main)), 0);
    CHECK(a.size() == 2 && a[0].type == Action::DropPopup && a[0].sub == 2 && a[1].type == Action::Drop);
    CHECK(b.PopupWindow(kFirst + 1, 2) == 0);
    CHECK(One(b.On(Ev(Event::Unmapped, Win(9, Kind::Popup)), 0), Action::DropPopup, kFirst));
    // popups made ahead of time and never mapped go after a while; kMaxPopups at most (council,
    // 2026-11-16: hidden menus used up SteamVR's overlays)
    Book many;
    many.On(Ev(Event::Created, Win(1, Kind::Main)), 0), many.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
    for (int i = 0; i < kMaxPopups; ++i) CHECK(many.On(Ev(Event::Created, Win(50 + i, Kind::Popup, 100, 50)), 0).size() == 1);
    CHECK(many.On(Ev(Event::Created, Win(90, Kind::Popup, 100, 50)), 0).empty());
    CHECK(many.On(Ev(Event::Mapped, Win(50, Kind::Popup, 100, 50)), 10).size() == 1);   // this one maps: it stays
    a = many.Tick(1 + kMapWaitMs);
    CHECK(int(a.size()) == kMaxPopups - 1 && a[0].type == Action::DropPopup);
    CHECK(many.PopupWindow(kFirst, 1) == 50 && many.PanelOf(50) == kFirst && many.PanelOf(51) == -1);
    // a window that unmaps with its dialog still up goes at once (council pass 7: hidden, it took the
    // dialog with it for kMapWaitMs); with nothing over it, it's only hidden
    Book d;
    d.On(Ev(Event::Created, Win(1, Kind::Main)), 0), d.On(Ev(Event::Mapped, Win(1, Kind::Main)), 0);
    d.On(Ev(Event::Created, Win(7, Kind::Popup, 300, 200, 1)), 0), d.On(Ev(Event::Mapped, Win(7, Kind::Popup, 300, 200, 1)), 0);
    a = d.On(Ev(Event::Unmapped, Win(1, Kind::Main)), 10);
    CHECK(a.size() == 2 && a[0].type == Action::DropPopup && a[1].type == Action::Drop && d.PanelOf(7) == -1);
    a = many.On(Ev(Event::Mapped, Win(51, Kind::Popup, 100, 50)), 2 + kMapWaitMs);   // mapped later: made then
    CHECK(a.size() == 1 && a[0].type == Action::MakePopup);
    CHECK(many.Tick(100 + 2 * kMapWaitMs).empty());   // (mapped ones stay)
    // a window whose panel couldn't be made: forgotten, its index free
    a = many.Lose(1);
    CHECK(a.size() == 3 && a.back().type == Action::Drop && many.Panels() == 0);
    // no panel to go over: none
    Book none;
    CHECK(none.On(Ev(Event::Created, Win(3, Kind::Popup, 50, 50)), 0).empty());
}

static void Typing() {
    Keys k;
    CHECK(k.Route(30, 1, false) == 0);   // no app has the typing
    k.Focus(5);
    CHECK(k.Route(30, 1, false) == 5);
    CHECK(k.Route(30, 2, false) == 5);
    CHECK(k.Route(31, 1, true) == 0);    // frunner's search has it
    k.Focus(0);                          // clicked elsewhere, A still down: its release is the app's
    CHECK(k.Route(30, 2, false) == 5);
    CHECK(k.Route(30, 0, false) == 5);
    CHECK(k.Route(30, 0, false) == 0);
    CHECK(k.Route(32, 1, false) == 0);
    k.Focus(6);
    CHECK(k.Route(33, 1, false) == 6);
    k.Gone(6);
    CHECK(k.Target() == 0 && k.Route(33, 0, false) == 0 && k.Down().empty());
}

// Two lasers on app panels: the one moving keeps the X pointer, a press takes it (follow-up)
static void Lasers() {
    PointerOwner o;
    CHECK(o.Move(1, 0));
    CHECK(!o.Move(2, 10));              // laser 1 still moving
    CHECK(o.Move(1, 20));
    CHECK(o.Move(2, 20 + PointerOwner::kHoldMs + 1));   // laser 1 still: laser 2 takes it
    CHECK(!o.Move(1, 400));
    o.Press(1, true, 410);              // a press takes it
    CHECK(o.Move(1, 420) && !o.Move(2, 2000));   // held: laser 2 waits however long
    o.Press(2, false, 2010);            // (another laser's release doesn't end laser 1's hold)
    CHECK(!o.Move(2, 2020));
    o.Press(1, false, 2030);
    CHECK(!o.Move(2, 2100) && o.Move(2, 2030 + PointerOwner::kHoldMs + 1));
    o.Left(2);                          // it left the app panels: anyone's
    CHECK(o.Move(1, 2400));
    // the panel pressed on goes (a button that closes its window): no release comes, the press is over
    PointerOwner g;
    g.Press(1, true, 0, 4, 0);
    g.PanelGone(4, 2);                  // a menu over it went: still held
    CHECK(!g.Move(2, 1000));
    g.PanelGone(5);                     // another panel: still held
    CHECK(!g.Move(2, 1100));
    g.PanelGone(4);                     // its own
    CHECK(g.Move(2, 1200));
    g.Press(2, true, 1300, 4, 3);       // on a menu, which closes on the press
    g.PanelGone(4, 3);
    CHECK(g.Move(1, 1300 + PointerOwner::kHoldMs + 1));
}

int main() {
    Panels();
    Popups();
    Typing();
    Lasers();
    if (failures) return std::printf("gsapps-test: %d failed\n", failures), 1;
    std::printf("gsapps-test: ok\n");
    return 0;
}
