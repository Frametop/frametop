// gslink.h against Xvfb (standing in for gamescope's app display; it has no forwarding, so the
// link opens it without asking for that): it opens, its events come with names, tags go, a tag
// left by an ft-screens that went is cleared, and with the X server stopped (a hung gamescope)
// the caller's side doesn't wait: Take and the sends come straight back, and Stop gives up when
// it said it would. Skipped without Xvfb.
//
//   g++ -std=c++17 -pthread -Iscreens screens/tests/gslink-test.cpp screens/gslink.cpp screens/gsforward.cpp $(pkg-config --libs xcb xcb-xtest xcb-icccm)
#include "gslink.h"

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace gsforward;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

static double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Every update taken so far, in order, and how far the checks have read.
static std::vector<Update> seen;
static size_t readTo = 0;

// The next update (after the last one matched) that matches, waiting up to two seconds for it.
template <typename F>
static bool Until(AsyncLink &link, F match, Update *got = nullptr) {
    const auto until = Clock::now() + std::chrono::seconds(2);
    for (;;) {
        for (Update &u : link.Take()) seen.push_back(std::move(u));
        for (size_t i = readTo; i < seen.size(); ++i)
            if (match(seen[i])) {
                if (got) *got = seen[i];
                readTo = i + 1;
                return true;
            }
        if (Clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static xcb_atom_t Atom(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(std::strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    std::free(r);
    return a;
}
static int TagLength(xcb_connection_t *c, xcb_window_t w, xcb_atom_t a) {
    xcb_get_property_reply_t *p = xcb_get_property_reply(c, xcb_get_property(c, 0, w, a, XCB_ATOM_CARDINAL, 0, 2), nullptr);
    const int n = p ? xcb_get_property_value_length(p) : -1;
    std::free(p);
    return n;
}

int main() {
    if (std::system("command -v Xvfb >/dev/null 2>&1") != 0) return std::printf("gslink-test: no Xvfb, skipped\n"), 0;
    const char *display = ":95";
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
    if (!app) return kill(x, SIGTERM), 1;
    const xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(app)).data;
    const xcb_atom_t target = Atom(app, "STEAM_GAMESCOPE_VROVERLAY_TARGET");

    // a window with a tag left from before
    const xcb_window_t stale = xcb_generate_id(app);
    xcb_create_window(app, XCB_COPY_FROM_PARENT, stale, scr->root, 0, 0, 300, 300, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, 0, nullptr);
    const uint32_t halves[2] = {1, 2};
    xcb_change_property(app, XCB_PROP_MODE_REPLACE, stale, target, XCB_ATOM_CARDINAL, 32, 2, halves);
    std::free(xcb_get_input_focus_reply(app, xcb_get_input_focus(app), nullptr));
    CHECK(TagLength(app, stale, target) == 8);

    AsyncLink link(display, false);
    CHECK(link.Take().empty());   // off: nothing
    link.SetOn(true);
    CHECK(Until(link, [](const Update &u) { return u.type == Update::Opened; }));
    CHECK(link.IsOpen());
    CHECK(Until(link, [&](const Update &u) { return u.type == Update::Window && u.event.win.id == stale; }));
    CHECK(TagLength(app, stale, target) == 0);   // cleared at the open

    // a new window: created, then mapped with its name read on the thread
    const xcb_window_t w = xcb_generate_id(app);
    xcb_create_window(app, XCB_COPY_FROM_PARENT, w, scr->root, 0, 0, 640, 400, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, 0, nullptr);
    const char cls[] = "konsole\0Konsole";
    xcb_icccm_set_wm_class(app, w, sizeof cls, cls);
    xcb_icccm_set_wm_name(app, w, XCB_ATOM_STRING, 8, 5, "Shell");
    xcb_flush(app);
    CHECK(Until(link, [&](const Update &u) { return u.type == Update::Window && u.event.type == Event::Created && u.event.win.id == w; }));
    link.Tag(w, 0x0000000500000007ull);
    xcb_map_window(app, w);
    xcb_flush(app);
    Update mapped;
    CHECK(Until(link, [&](const Update &u) { return u.type == Update::Window && u.event.type == Event::Mapped && u.event.win.id == w; },
                &mapped));
    CHECK(mapped.event.win.key == "x11:Konsole" && mapped.event.win.title == "Shell");
    CHECK(TagLength(app, w, target) == 8);   // (sent before the map: done by the time the map came back)

    // the X server stopped: nothing on this side waits
    kill(x, SIGSTOP);
    xcb_map_window(app, stale);   // (an event the thread will want to read a reply for)
    xcb_flush(app);
    const auto t0 = Clock::now();
    for (int i = 0; i < 1000; ++i) {
        link.Pointer(i, i);
        link.Button(1, i % 2 == 0);
        link.Key(w, 38, 0, i % 2 == 0);
        link.Tag(w, 7);
        for (Update &u : link.Take()) seen.push_back(std::move(u));
        (void)link.IsOpen();
    }
    const double took = MsSince(t0);
    std::printf("gslink-test: 4000 sends and 1000 takes with the server stopped: %.1f ms\n", took);
    CHECK(took < 200);
    kill(x, SIGCONT);
    link.Untag(w);
    const auto t1 = Clock::now();
    while (TagLength(app, w, target) != 0 && MsSince(t1) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(TagLength(app, w, target) == 0);   // what waited went once it came back

    // the queue full while it's stopped: moves and presses drop, a release never does (council pass 2)
    kill(x, SIGSTOP);
    link.Pointer(20, 20);
    link.Button(1, true);
    for (size_t i = 0; i < AsyncLink::kMaxQueued + 500; ++i) link.Raise(w);
    link.Raise(stale);   // past the cap: it takes the last raise's place, not dropped (follow-up)
    link.Button(1, false);
    kill(x, SIGCONT);
    auto held = [&] {
        xcb_query_pointer_reply_t *q = xcb_query_pointer_reply(app, xcb_query_pointer(app, scr->root), nullptr);
        const bool down = q && (q->mask & XCB_BUTTON_MASK_1);
        std::free(q);
        return down;
    };
    const auto t3 = Clock::now();
    bool sawUp = false;
    while (MsSince(t3) < 3000 && !(sawUp = !held())) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // (all of the queue through)
    CHECK(sawUp && !held());
    {
        xcb_query_tree_reply_t *tree = xcb_query_tree_reply(app, xcb_query_tree(app, scr->root), nullptr);
        int atW = -1, atStale = -1;
        if (tree) {
            const xcb_window_t *kids = xcb_query_tree_children(tree);
            for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
                if (kids[i] == w) atW = i;
                if (kids[i] == stale) atStale = i;
            }
        }
        std::free(tree);
        CHECK(atW >= 0 && atStale > atW);   // (bottom to top: the raise past the cap went through)
    }

    // off: closed; on again: open again, the windows there again
    link.SetOn(false);
    CHECK(Until(link, [](const Update &u) { return u.type == Update::Closed; }));
    CHECK(!link.IsOpen());
    link.SetOn(true);
    CHECK(Until(link, [](const Update &u) { return u.type == Update::Opened; }));
    CHECK(Until(link, [&](const Update &u) { return u.type == Update::Window && u.event.win.id == w; }));

    // stopped again, Stop gives up when it said it would
    kill(x, SIGSTOP);
    xcb_map_window(app, w);
    xcb_flush(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    AsyncLink *hung = new AsyncLink(display, false);   // (opening it waits on the stopped server)
    hung->SetOn(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t2 = Clock::now();
    hung->Stop(150);
    const double stopTook = MsSince(t2);
    std::printf("gslink-test: Stop(150) with the server stopped: %.0f ms\n", stopTook);
    CHECK(stopTook >= 140 && stopTook < 400);
    delete hung;   // (its thread is left to the server: it keeps what it needs)
    kill(x, SIGCONT);

    // the server going: Closed
    xcb_disconnect(app);
    kill(x, SIGTERM);
    waitpid(x, nullptr, 0);
    CHECK(Until(link, [](const Update &u) { return u.type == Update::Closed; }));
    CHECK(!link.IsOpen());
    link.Stop(1000);

    if (failures) return std::printf("gslink-test: %d failed\n", failures), 1;
    std::printf("gslink-test: ok\n");
    return 0;
}
