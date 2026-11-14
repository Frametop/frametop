// The wearer's hand on someone else's panel (together/local/panelhand.h): what ft-screens'
// words become for ft-together, how often moves go, what is said of a pointer that rests and
// one that leaves, and that nothing stays down. No sockets, no clock.
#include "../local/panelhand.h"

#include <cstdio>
#include <map>

using local::PanelHand;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint32_t g_seed = 90210;
static uint32_t roll() { return g_seed = g_seed * 1664525u + 1013904223u, g_seed >> 8; }

static bool pointer(const std::string &one, proto::Point &p) { return proto::Read(reinterpret_cast<const uint8_t *>(one.data()), one.size(), p); }

// What went out since the last look, in order: an act as its words after the panel, a pointer
// datagram as "@PANEL X Y" with a "!" when it says a button is down.
static std::string said(PanelHand::Out &out, int panel = 9) {
    const std::string act = "control act " + std::to_string(panel) + " ";
    std::string all;
    for (const std::string &one : out) {
        proto::Point p;
        std::string text;
        if (pointer(one, p)) text = "@" + std::to_string(p.panel) + " " + std::to_string(p.x) + " " + std::to_string(p.y) + (p.flags ? "!" : "");
        else text = one.rfind(act, 0) == 0 ? one.substr(act.size()) : "?" + one;
        all += (all.empty() ? "" : " | ") + text;
    }
    out.clear();
    return all;
}

// A hand on panel 9 whose picture is 1920 x 1080, with the pointer in its middle at second 10.
struct Hand {
    PanelHand hand{9};
    PanelHand::Out out;

    explicit Hand(bool on = true) {
        hand.Size(1920, 1080);
        if (on) hand.Word("move 960 540", 10, out), out.clear();
    }
    std::string word(const char *msg, double now) { return hand.Word(msg, now, out) ? said(out) : "not the hand's"; }
    std::string tick(double now) { return hand.Tick(now, out), said(out); }
    std::string end(double now) { return hand.End(now, out), said(out); }
};

static void moves() {
    PanelHand unsized(9);
    PanelHand::Out out;
    check("until the picture's size is known a move goes nowhere", unsized.Word("move 5 5", 1, out) && out.empty() && !unsized.on() && unsized.Wait(1) < 0);

    Hand h(false);
    check("a move goes out as an act for the owner and a pointer for the room, across and down the picture", h.word("move 960 540", 10) == "move 32768 32768 | @9 32768 32768");
    check("its far corners are the picture's", h.word("move 0 0", 11) == "move 0 0 | @9 0 0" && h.word("move 1919.9 1079.9", 12) == "move 65532 65529 | @9 65532 65529");
    check("a move past an edge is on the edge, and one that is no number is at the start",
          h.word("move 5000 -3", 13) == "move 65535 0 | @9 65535 0" && h.word("move nan 1e999", 14) == "move 0 65535 | @9 0 65535");

    Hand fast;
    check("moves that come faster than sixty a second wait", fast.word("move 100 100", 11) != "" && fast.word("move 200 200", 11.005) == "" && fast.word("move 300 300", 11.010) == "");
    check("...and Tick says when the last of them is due", fast.hand.Wait(11.010) > 0.006 && fast.hand.Wait(11.010) < 0.007 && fast.tick(11.012) == "");
    check("...and sends that one only", fast.tick(11.017) == "move 10240 18204 | @9 10240 18204" && fast.tick(11.05) == "");
    check("a move after a pause goes at once", fast.word("move 0 0", 11.1) == "move 0 0 | @9 0 0");
}

static void buttons() {
    Hand h;
    check("a button down goes where the pointer is, and the room sees it held", h.word("button 272 1", 11) == "button 1 1 | @9 32768 32768!");
    check("it goes down once", h.word("button 272 1", 11.1) == "");
    check("a second button is the owner's right one", h.word("button 273 1", 11.2) == "button 2 1 | @9 32768 32768!");
    check("letting one go leaves the other held", h.word("button 272 0", 11.3) == "button 1 0 | @9 32768 32768!");
    check("letting the last go says so to the room", h.word("button 273 0", 11.4) == "button 2 0 | @9 32768 32768" && h.word("button 273 0", 11.5) == "");
    check("the middle button is the third", h.word("button 274 1", 11.6) == "button 3 1 | @9 32768 32768!" && h.word("button 274 0", 11.7) == "button 3 0 | @9 32768 32768");
    check("a button that isn't one of the three goes nowhere", h.word("button 275 1", 12) == "" && h.word("button 30 1", 12) == "" && h.word("button 0 1", 12) == "");

    Hand drag;
    drag.word("move 100 100", 11);
    check("a move that waited goes before the button pressed where it points",
          drag.word("move 480 270", 11.004) == "" && drag.word("button 272 1", 11.005) == "move 16384 16384 | @9 16384 16384 | button 1 1 | @9 16384 16384!");
    check("...and nothing of it is sent twice", drag.tick(11.03) == "");
    check("while it is held the moves say so", drag.word("move 960 540", 11.1) == "move 32768 32768 | @9 32768 32768!");

    Hand off(false);
    check("with the pointer nowhere on the panel a button doesn't go down", off.word("button 272 1", 11) == "" && off.word("button 272 0", 11.1) == "");
}

static void scrolls() {
    Hand h;
    check("a notch down is 120", h.word("scroll 0.000 1.000", 11) == "scroll 0 120");
    check("part of a notch to the left and up", h.word("scroll -0.5 -0.25", 11.1) == "scroll -60 -30");
    check("more than an act may say is the most it may", h.word("scroll 100 -1e9", 11.2) == "scroll 2400 -2400");
    check("a scroll too small to count, or no number, goes nowhere", h.word("scroll 0 0", 11.3) == "" && h.word("scroll 0.001 0", 11.3) == "" && h.word("scroll nan nan", 11.3) == "");
    h.word("move 100 100", 12);
    check("a move that waited goes before the scroll there", h.word("move 480 270", 12.004) == "" && h.word("scroll 0 1", 12.005) == "move 16384 16384 | @9 16384 16384 | scroll 0 120");
    Hand off(false);
    check("with the pointer nowhere on the panel nothing scrolls", off.word("scroll 0 1", 11) == "");
}

static void keys() {
    Hand h(false);
    check("a key goes down and up, wherever the pointer is", h.word("key 30 1", 11) == "key 30 1" && h.word("key 30 0", 11.1) == "key 30 0");
    check("it goes down once, and comes up only if it went down here", h.word("key 30 1", 12) == "key 30 1" && h.word("key 30 1", 12.1) == "" && h.word("key 31 0", 12.2) == "");
    check("what is no key of a keyboard goes nowhere", h.word("key 0 1", 12.3) == "" && h.word("key 256 1", 12.3) == "" && h.word("key 4000000000 1", 12.3) == "");
    for (int code = 40; code < 47; ++code) h.word(("key " + std::to_string(code) + " 1").c_str(), 13);
    check("eight keys are held at most", h.word("key 50 1", 13.1) == "" && h.word("key 50 0", 13.2) == "");
    check("when typing goes elsewhere they come up, the last pressed first", h.word("blur", 14) == "key 46 0 | key 45 0 | key 44 0 | key 43 0 | key 42 0 | key 41 0 | key 40 0 | key 30 0");
    check("...once", h.word("blur", 14.1) == "" && h.word("key 46 0", 14.2) == "");
}

static void resting() {
    Hand h;
    check("a pointer that rests says nothing for a second", h.tick(10.5) == "" && h.tick(10.99) == "" && h.hand.Wait(10.5) > 0.49 && h.hand.Wait(10.5) < 0.51);
    check("...then says where it is again, to the room only", h.tick(11.0) == "@9 32768 32768" && h.tick(11.5) == "" && h.tick(12.0) == "@9 32768 32768");
    h.word("button 272 1", 12.5);
    check("held, it says so each time", h.tick(13.5) == "@9 32768 32768!");
    check("when it leaves, what it held comes up and the room hears it points at nothing", h.word("leave", 14) == "button 1 0 | @255 32768 32768");
    check("...once, and nothing more is said of it", h.word("leave", 14.1) == "" && h.tick(20) == "" && h.hand.Wait(20) < 0 && !h.hand.on());
    check("it comes back with the next move", h.word("move 0 0", 21) == "move 0 0 | @9 0 0" && h.hand.on());

    Hand waiting;
    waiting.word("move 100 100", 11);
    waiting.word("move 200 200", 11.004);
    check("a move that waited when the pointer left isn't sent after it", waiting.word("leave", 11.005) == "@255 6826 12136" && waiting.tick(11.1) == "");
}

static void ending() {
    Hand h;
    h.word("key 42 1", 11), h.word("key 30 1", 11), h.word("button 272 1", 11), h.word("button 274 1", 11);
    check("at the end the keys and the buttons come up and the pointer points at nothing", h.end(12) == "key 30 0 | key 42 0 | button 3 0 | button 1 0 | @255 32768 32768");
    check("...once", h.end(12.1) == "");
    Hand idle(false);
    check("a hand that did nothing has nothing to end", idle.end(12) == "");
}

static void others() {
    Hand h;
    bool none = true;
    for (const char *msg : {"", "focus", "release 1", "attention view", "attention focused", "quit", "modifiers 0 1", "pad off", "moved 1 2", "move 1", "button 272", "key", "scroll 1",
                            "Move 1 2", " move 1 2", "leave now", "blurred"})
        none = none && h.word(msg, 11) == "not the hand's";
    check("words that say nothing of the hand are left to the program, and send nothing", none && h.tick(10.5) == "");
}

// Whatever ft-screens says and whenever, every line that goes out is input a panel takes, every
// datagram is a pointer on this panel or on none, and at the end nothing is left down.
static void anything() {
    bool taken = true, pointers = true;
    std::map<std::pair<std::string, int>, int> down;
    PanelHand hand(17);
    PanelHand::Out out;
    hand.Size(1280, 800);
    double now = 5;
    const auto look = [&] {
        for (const std::string &one : out) {
            proto::Point p;
            if (pointer(one, p)) {
                pointers = pointers && (p.panel == 17 || p.panel == proto::kNoPanel);
                continue;
            }
            char what[8] = "";
            int panel = -1, a = 0, b = 0, used = 0;
            proto::Act act, back;
            if (std::sscanf(one.c_str(), "control act %d %7s %d %d%n", &panel, what, &a, &b, &used) != 4 || size_t(used) != one.size() || panel != 17) {
                taken = false;
                continue;
            }
            const std::string w = what;
            act.panel = 17, act.what = w == "move" ? proto::kActMove : w == "button" ? proto::kActButton : w == "scroll" ? proto::kActScroll : w == "key" ? proto::kActKey : 99;
            act.a = a, act.b = b;
            taken = taken && proto::Read(proto::Body(act), back);
            if (w == "button" || w == "key") {
                int &held = down[{w, a}];
                taken = taken && held == (b ? 0 : 1);   // down only when up, up only when down
                held = b;
            }
        }
        out.clear();
    };
    for (int i = 0; i < 20000; ++i) {
        now += (roll() % 30) / 1000.0;
        char msg[64];
        switch (roll() % 9) {
            case 0: case 1: case 2: std::snprintf(msg, sizeof msg, "move %.1f %.1f", int(roll() % 3000) - 500.0, int(roll() % 2000) - 500.0); break;
            case 3: std::snprintf(msg, sizeof msg, "button %u %u", 270 + roll() % 8, roll() % 2); break;
            case 4: std::snprintf(msg, sizeof msg, "scroll %.3f %.3f", (int(roll() % 600) - 300) / 10.0, (int(roll() % 600) - 300) / 10.0); break;
            case 5: case 6: std::snprintf(msg, sizeof msg, "key %u %u", roll() % 300, roll() % 2); break;
            case 7: std::snprintf(msg, sizeof msg, "%s", roll() % 4 ? "blur" : "leave"); break;
            default: msg[0] = 0; hand.Tick(now, out); break;
        }
        if (msg[0]) hand.Word(msg, now, out);
        look();
    }
    hand.End(now, out);
    look();
    bool up = true;
    for (const auto &[key, held] : down) up = up && !held;
    check("whatever is said, every line out is input a panel takes", taken);
    check("...every datagram is a pointer on this panel or on none", pointers);
    check("...and at the end nothing is left down", up && !down.empty());
}

int main() {
    moves();
    buttons();
    scrolls();
    keys();
    resting();
    ending();
    others();
    anything();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
