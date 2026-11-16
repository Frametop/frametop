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
    // unmapped: gone, and its index free again
    CHECK(One(b.On(Ev(Event::Unmapped, Win(10, Kind::Main)), 40), Action::Drop, kFirst));
    CHECK(b.On(Ev(Event::Gone, Win(10, Kind::Main)), 41).empty());
    CHECK(One(b.On(Ev(Event::Created, Win(12, Kind::Main)), 50), Action::Make, kFirst));
    // created, never mapped: dropped after a while
    CHECK(b.Tick(50 + kMapWaitMs).empty());
    CHECK(One(b.Tick(51 + kMapWaitMs), Action::Drop, kFirst));
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

int main() {
    Panels();
    Popups();
    Typing();
    if (failures) return std::printf("gsapps-test: %d failed\n", failures), 1;
    std::printf("gsapps-test: ok\n");
    return 0;
}
