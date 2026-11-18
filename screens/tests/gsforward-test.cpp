// gsforward.h: the policy (which windows get panels, which panel a popup goes over, keys and
// buttons as X's), and Link against a real X server: Xvfb, standing in for gamescope's app
// display (it has no forwarding, so Link opens it without asking for that). Skips the X half
// when there's no Xvfb.
//
//   g++ -std=c++17 -Iscreens screens/tests/gsforward-test.cpp screens/gsforward.cpp $(pkg-config --libs xcb xcb-xtest xcb-icccm)
#include "gsdisplay.h"
#include "gsforward.h"

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using namespace gsforward;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

static void Policy() {
    CHECK(Classify(true, false, false, 1280, 720) == Kind::Main);
    CHECK(Classify(true, false, false, 10, 10) == Kind::Ignore);    // a leader window
    CHECK(Classify(false, false, false, 1280, 720) == Kind::Ignore); // input only
    CHECK(Classify(true, true, false, 200, 30) == Kind::Popup);      // a menu
    CHECK(Classify(true, false, true, 400, 300) == Kind::Popup);     // a dialog
    CHECK(Classify(true, true, false, 1, 1) == Kind::Ignore);

    CHECK(PopupOwner(7, 9, {5, 7, 9}) == 7);
    CHECK(PopupOwner(3, 9, {5, 7, 9}) == 9);    // transient for one that isn't ours
    CHECK(PopupOwner(0, 0, {5, 7}) == 5);
    CHECK(PopupOwner(0, 4, {}) == 0);

    CHECK(AppKey(570, "dota", "x") == "steam:570");
    CHECK(AppKey(0, "konsole", "konsole") == "x11:konsole");
    CHECK(AppKey(0, "", "inst") == "x11:inst");
    CHECK(AppKey(0, "", "").empty());

    CHECK(XKeycode(30) == 38);   // KEY_A
    CHECK(XKeycode(300) == 0);
    Mods m;
    CHECK(State(m) == 0);
    m.shift = m.ctrl = true;
    CHECK(State(m) == 5);
    m = Mods{false, false, true, true};
    CHECK(State(m) == 72);
    // a Shift's own press isn't shifted; its release is
    CHECK(!Before(Mods{true, false, false, false}, 42, true).shift);
    CHECK(Before(Mods{false, false, false, false}, 42, false).shift);
    CHECK(Before(Mods{true, true, false, false}, 30, true).shift && Before(Mods{true, true, false, false}, 30, true).ctrl);

    CHECK(XButton(0x110) == 1);
    CHECK(XButton(0x111) == 3);
    CHECK(XButton(0x112) == 2);
    CHECK(XButton(0x113) == 8);

    uint32_t h[2];
    TargetHalves(0x123456789abcdef0ull, h);
    CHECK(h[0] == 0x9abcdef0u && h[1] == 0x12345678u);
}

static bool HaveXvfb() { return std::system("command -v Xvfb >/dev/null 2>&1") == 0; }

// Events until one of `type` for `win` (or a second passes).
static bool Wait(Link &link, Event::Type type, uint32_t win, Event *got = nullptr) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < until) {
        for (const Event &e : link.Poll())
            if (e.type == type && e.win.id == win) {
                if (got) *got = e;
                return true;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static void WithX() {
    const char *display = ":97";
    const pid_t x = fork();
    if (x == 0) {
        execlp("Xvfb", "Xvfb", display, "-screen", "0", "1280x720x24", "-nolisten", "tcp", nullptr);
        _exit(127);
    }
    xcb_connection_t *app = nullptr;
    for (int i = 0; i < 100; ++i) {
        app = xcb_connect(display, nullptr);
        if (!xcb_connection_has_error(app)) break;
        xcb_disconnect(app), app = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(app);
    if (!app) return (void)kill(x, SIGTERM);
    const xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(app)).data;

    Link link;
    std::string why;
    CHECK(!link.Open(display, true, &why));   // not a gamescope
    CHECK(why.find("forwards") != std::string::npos);
    {   // as gamescope's: its first Xwayland (Steam's) refused, another taken
        auto atom = [&](const char *n) {
            auto r = xcb_intern_atom_reply(app, xcb_intern_atom(app, 0, uint16_t(std::strlen(n)), n), nullptr);
            const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
            std::free(r);
            return a;
        };
        auto set = [&](const char *n, uint32_t v) {
            xcb_change_property(app, XCB_PROP_MODE_REPLACE, scr->root, atom(n), XCB_ATOM_CARDINAL, 32, 1, &v);
            std::free(xcb_get_input_focus_reply(app, xcb_get_input_focus(app), nullptr));
        };
        set("GAMESCOPE_VROVERLAY_FORWARDING", 3), set("GAMESCOPE_PID", 4242), set("GAMESCOPE_XWAYLAND_SERVER_ID", 0);
        CHECK(!link.Open(display, true, &why) && why.find("Steam's own") != std::string::npos);
        {   // Steam's own, as Role::Steam: its windows' tags kept and read, a change of one said
            const xcb_window_t sw = xcb_generate_id(app);
            xcb_create_window(app, XCB_COPY_FROM_PARENT, sw, scr->root, 0, 0, 1920, 1080, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              scr->root_visual, 0, nullptr);
            const uint32_t steams[2] = {39, 56};
            const xcb_atom_t target = atom("STEAM_GAMESCOPE_VROVERLAY_TARGET");
            xcb_change_property(app, XCB_PROP_MODE_REPLACE, sw, target, XCB_ATOM_CARDINAL, 32, 2, steams);
            xcb_map_window(app, sw);
            std::free(xcb_get_input_focus_reply(app, xcb_get_input_focus(app), nullptr));
            CHECK(link.Open(display, true, &why, Role::Steam));
            Event se;
            CHECK(Wait(link, Event::Created, sw, &se) && se.win.target == (uint64_t(56) << 32 | 39));
            auto r = xcb_get_property_reply(app, xcb_get_property(app, 0, sw, target, XCB_ATOM_CARDINAL, 0, 2), nullptr);
            CHECK(r && xcb_get_property_value_length(r) == 8);   // (not cleared: Steam's)
            std::free(r);
            link.Tag(sw, 0x0000000300000009ull);
            CHECK(Wait(link, Event::Retarget, sw, &se) && se.win.target == 0x0000000300000009ull);
            xcb_delete_property(app, sw, target);
            xcb_flush(app);
            CHECK(Wait(link, Event::Retarget, sw, &se) && se.win.target == 0);
            link.Close();
            xcb_destroy_window(app, sw);
            xcb_flush(app);
        }
        set("GAMESCOPE_XWAYLAND_SERVER_ID", 1);
        CHECK(!link.Open(display, true, &why, Role::Steam) && why.find("isn't Steam's own") != std::string::npos);
        CHECK(link.Open(display, true, &why) && link.GamescopePid() == 4242);
        link.Close();
        for (const char *n : {"GAMESCOPE_VROVERLAY_FORWARDING", "GAMESCOPE_PID", "GAMESCOPE_XWAYLAND_SERVER_ID"})
            xcb_delete_property(app, scr->root, atom(n));
        xcb_flush(app);
    }
    CHECK(link.Open(display, false, &why));
    CHECK(link.Poll().empty());

    // An app's window: created, described, mapped, tagged, resized, given a key and a click.
    const xcb_window_t w = xcb_generate_id(app);
    const uint32_t mask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS;
    xcb_create_window(app, XCB_COPY_FROM_PARENT, w, scr->root, 0, 0, 640, 400, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, XCB_CW_EVENT_MASK, &mask);
    const char cls[] = "konsole\0Konsole";
    xcb_icccm_set_wm_class(app, w, sizeof cls, cls);
    xcb_icccm_set_wm_name(app, w, XCB_ATOM_STRING, 8, 5, "Shell");
    xcb_flush(app);
    Event e;
    CHECK(Wait(link, Event::Created, w, &e));
    CHECK(e.win.kind == Kind::Main && e.win.w == 640 && e.win.h == 400);
    xcb_map_window(app, w);
    xcb_flush(app);
    CHECK(Wait(link, Event::Mapped, w, &e));
    link.Describe(&e.win);
    CHECK(e.win.key == "x11:Konsole");
    CHECK(e.win.title == "Shell");

    link.Tag(w, 0x0000000500000007ull);
    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(app, xcb_intern_atom(app, 0, 32, "STEAM_GAMESCOPE_VROVERLAY_TARGET"), nullptr);
    // The tag as the app sees it, once it's `set` (or gone), within a second: Link's connection
    // flushes but doesn't wait, so the X server may answer this one first.
    const auto tagged = [&](bool set) {
        xcb_get_property_reply_t *r = nullptr;
        for (int i = 0; i < 200; ++i) {
            std::free(r);
            r = xcb_get_property_reply(app, xcb_get_property(app, 0, w, a->atom, XCB_ATOM_CARDINAL, 0, 2), nullptr);
            if (r && (set ? xcb_get_property_value_length(r) == 8 : r->type == XCB_ATOM_NONE)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return r;
    };
    xcb_get_property_reply_t *p = tagged(true);
    CHECK(p && xcb_get_property_value_length(p) == 8);
    if (p && xcb_get_property_value_length(p) == 8) {
        const uint32_t *v = static_cast<const uint32_t *>(xcb_get_property_value(p));
        CHECK(v[0] == 7 && v[1] == 5);
    }
    std::free(p);
    // Focus with no gamescope to read the focus from (Xvfb): says so, and the tag stays as it was
    CHECK(link.FocusSaying(w).find("no first Xwayland") != std::string::npos);
    p = tagged(true);
    CHECK(p && xcb_get_property_value_length(p) == 8 && static_cast<const uint32_t *>(xcb_get_property_value(p))[0] == 7);
    std::free(p);
    link.Untag(w);
    p = tagged(false);
    CHECK(p && p->type == XCB_ATOM_NONE);
    std::free(p), std::free(a);
    CHECK(link.FocusSaying(w).empty());   // (not ours any more: left alone)

    link.Resize(w, 800, 500);
    CHECK(Wait(link, Event::Moved, w, &e));
    CHECK(e.win.w == 800 && e.win.h == 500);

    link.Key(w, XKeycode(30), State(Mods{true, false, false, false}), true);
    link.Pointer(100, 80);
    link.Button(XButton(0x110), true);
    link.Button(XButton(0x110), false);
    bool key = false, click = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!key || !click) && std::chrono::steady_clock::now() < until) {
        if (xcb_generic_event_t *ev = xcb_poll_for_event(app)) {
            const int t = ev->response_type & 0x7f;
            if (t == XCB_KEY_PRESS) {
                auto *k = reinterpret_cast<xcb_key_press_event_t *>(ev);
                key = k->detail == 38 && k->state == 1 && (ev->response_type & 0x80);   // (sent, not typed)
            }
            if (t == XCB_BUTTON_PRESS) {
                auto *b = reinterpret_cast<xcb_button_press_event_t *>(ev);
                click = b->detail == 1 && b->event_x == 100 && b->event_y == 80;
            }
            std::free(ev);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(key);
    CHECK(click);

    // A menu: override-redirect, a popup.
    const xcb_window_t menu = xcb_generate_id(app);
    const uint32_t over = 1;
    xcb_create_window(app, XCB_COPY_FROM_PARENT, menu, scr->root, 50, 60, 180, 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, XCB_CW_OVERRIDE_REDIRECT, &over);
    xcb_flush(app);
    CHECK(Wait(link, Event::Created, menu, &e));
    CHECK(e.win.kind == Kind::Popup && e.win.x == 50 && e.win.y == 60);
    xcb_destroy_window(app, menu);
    xcb_flush(app);
    CHECK(Wait(link, Event::Gone, menu));

    // Reparented (an app embedding a window): gone from the top level; given back: new, and mapped
    const xcb_window_t holder = xcb_generate_id(app), kid = xcb_generate_id(app);
    xcb_create_window(app, XCB_COPY_FROM_PARENT, holder, scr->root, 0, 0, 400, 400, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, 0, nullptr);
    xcb_create_window(app, XCB_COPY_FROM_PARENT, kid, scr->root, 0, 0, 200, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, 0, nullptr);
    xcb_map_window(app, kid);
    xcb_flush(app);
    CHECK(Wait(link, Event::Mapped, kid));
    xcb_reparent_window(app, kid, holder, 10, 10);
    xcb_flush(app);
    CHECK(Wait(link, Event::Gone, kid));
    xcb_reparent_window(app, kid, scr->root, 20, 30);
    xcb_flush(app);
    CHECK(Wait(link, Event::Mapped, kid, &e));
    CHECK(e.win.kind == Kind::Main && e.win.x == 20 && e.win.y == 30);
    xcb_destroy_window(app, kid);
    xcb_destroy_window(app, holder);
    xcb_flush(app);
    CHECK(Wait(link, Event::Gone, kid));

    xcb_unmap_window(app, w);
    xcb_flush(app);
    CHECK(Wait(link, Event::Unmapped, w));
    xcb_destroy_window(app, w);
    xcb_flush(app);
    CHECK(Wait(link, Event::Gone, w));

    // A window already there when Link opens comes as Created and Mapped.
    const xcb_window_t old = xcb_generate_id(app);
    xcb_create_window(app, XCB_COPY_FROM_PARENT, old, scr->root, 0, 0, 300, 300, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, 0, nullptr);
    xcb_map_window(app, old);
    std::free(xcb_get_input_focus_reply(app, xcb_get_input_focus(app), nullptr));   // (a round trip: it's there)
    CHECK(link.Open(display, false, &why));
    CHECK(Wait(link, Event::Mapped, old));

    // The server going: Link closes, and says so.
    xcb_disconnect(app);
    kill(x, SIGTERM);
    waitpid(x, nullptr, 0);
    const auto gone = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (link.IsOpen() && std::chrono::steady_clock::now() < gone) {
        link.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!link.IsOpen());
}

// Which display the apps are on: Steam's STEAM_GAME_DISPLAY_0, only a display's name taken
static void DisplaysToTry() {
    using namespace std::string_literals;
    CHECK(GameDisplayFrom("HOME=/home/a\0STEAM_GAME_DISPLAY_0=:3\0DISPLAY=:0\0"s) == ":3");
    CHECK(GameDisplayFrom("STEAM_GAME_DISPLAY_0=:12.0"s) == ":12.0");
    CHECK(GameDisplayFrom("XSTEAM_GAME_DISPLAY_0=:4\0DISPLAY=:0\0"s).empty());
    CHECK(GameDisplayFrom("STEAM_GAME_DISPLAY_0=host:1\0"s).empty());
    CHECK(GameDisplayFrom("STEAM_GAME_DISPLAY_0=:\0"s).empty() && GameDisplayFrom(""s).empty());
    CHECK(!DisplayName(":1.") && !DisplayName(":a") && !DisplayName("1") && DisplayName(":0"));
    using V = std::vector<std::string>;
    CHECK(Candidates(":7", ":3", {"X0", "X1"}) == V{":7"});   // GSAPP_DISPLAY alone
    CHECK(Candidates(nullptr, ":3", {".", "..", "X10", "X2", "X0", "Xjunk", "X3"}) == (V{":3", ":0", ":2", ":10", ":1"}));
    CHECK(Candidates("junk", "", {}) == V{":1"});
    setenv("GSAPP_DISPLAY", ":7", 1);
    CHECK(AppDisplays() == V{":7"});
    unsetenv("GSAPP_DISPLAY");
    const V found = AppDisplays();
    CHECK(std::find(found.begin(), found.end(), ":1") != found.end());   // (the default, if no socket said so)
}

int main() {
    Policy();
    DisplaysToTry();
    if (HaveXvfb()) WithX();
    else std::printf("gsforward-test: no Xvfb, the X half skipped\n");
    if (failures) return std::printf("gsforward-test: %d failed\n", failures), 1;
    std::printf("gsforward-test: ok\n");
    return 0;
}
