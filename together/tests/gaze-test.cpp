// Where the wearer looks on its way out of the Frame (together/local/gaze.h, hush.h, own.h and
// gazeask.h): shared only when they say so and only in that room, a direction that is kept only
// while it is told, the pointer that keeps quiet while their eyes move it, and the lines of
// text all of it is asked with. No sockets, no session.
#include "../local/gazeask.h"
#include "../local/own.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A pose as it goes out through `gaze` at `now`: "none" when it tells no gaze, else "YAW,PITCH".
static std::string looks(const local::Gaze &gaze, double now, bool claims = false) {
    proto::Pose p;
    p.looks = claims, p.gaze[0] = 33, p.gaze[1] = -21;
    gaze.Into(p, now);
    if (!p.looks) return p.gaze[0] == 0 && p.gaze[1] == 0 ? "none" : "a direction left in a pose that tells none";
    char text[48];
    std::snprintf(text, sizeof text, "%g,%g", double(p.gaze[0]), double(p.gaze[1]));
    return text;
}

static void rules() {
    local::Gaze g;
    check("nothing is shared until the wearer says so", !g.shared() && !g.fresh(0) && looks(g, 0) == "none");
    check("...and a direction told before that is not kept", !g.Say(10, 5, 1) && looks(g, 1) == "none");
    check("...whatever the pose that was handed over says of one itself", looks(g, 1, true) == "none");
    g.Share(true);
    check("sharing alone tells nothing: there is no direction yet", g.shared() && !g.fresh(2) && looks(g, 2) == "none");
    check("a direction told while it is shared goes out in the poses", g.Say(10, -5.5, 2) && g.fresh(2) && looks(g, 2) == "10,-5.5" && looks(g, 2.9, true) == "10,-5.5");
    check("...for a second after it was told, and no longer", looks(g, 3) == "10,-5.5" && looks(g, 3.01) == "none" && !g.fresh(3.01));
    check("...and not before it was", looks(g, 1.9) == "none");
    check("a newer one takes its place", g.Say(-20, 0, 4) && looks(g, 4.5) == "-20,0");
    check("no further than the wire holds", g.Say(500, -500, 5) && looks(g, 5) == "63.5,-63.5");
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    check("what isn't a number is no direction, and the last one stands", !g.Say(nan, 0, 5.5) && !g.Say(0, inf, 5.5) && looks(g, 5.5) == "63.5,-63.5" && looks(g, 6.2) == "none");
    g.Say(1, 2, 7);
    g.Share(false);
    check("turned off, the direction is gone at once", !g.shared() && !g.fresh(7) && looks(g, 7) == "none");
    g.Share(true);
    check("...and isn't back when it is turned on again", looks(g, 7.1) == "none" && g.Say(3, 4, 7.2) && looks(g, 7.2) == "3,4");
    g.Lift();
    check("out of the room it is off: the next room is told nothing", !g.shared() && looks(g, 7.3) == "none" && !g.Say(3, 4, 7.3));

    local::Gaze p;
    check("until someone says how the pointer is moved, it is taken to follow the eyes", p.quiet(0) && p.quiet(100));
    p.Pointer(false, 100);
    check("a pointer the hand moves isn't quiet", !p.quiet(100) && !p.quiet(105));
    check("...for five seconds after that was said", p.quiet(105.1) && p.quiet(99));
    p.Pointer(true, 106);
    check("one the eyes move is", p.quiet(106) && p.quiet(107));
    p.Share(true);
    check("...unless where they look is shared anyway", !p.quiet(107) && !p.quiet(1e6));
    p.Share(false);
    check("...and quiet again when that stops", p.quiet(107));
}

static proto::Point point(uint8_t panel, uint16_t x, uint16_t y, bool pressed = false) {
    proto::Point p;
    p.panel = panel, p.x = x, p.y = y, p.flags = pressed ? proto::kPointPressed : 0;
    return p;
}
// What `hush` lets of a pointer through: "-" nothing, else "PANEL:X:Y" with "!" while pressed.
static std::string through(local::Hush &hush, proto::Point p, bool quiet, double now) {
    if (!hush.Point(p, quiet, now)) return "-";
    return std::to_string(p.panel) + ":" + std::to_string(p.x) + ":" + std::to_string(p.y) + (p.flags & proto::kPointPressed ? "!" : "");
}
static proto::Act act(uint8_t what, int32_t a, int32_t b, uint8_t panel = 9) {
    proto::Act m;
    m.panel = panel, m.what = what, m.a = a, m.b = b;
    return m;
}
// What `hush` lets of an act through, in order: "move 1 2, button 1 1", "-" for nothing.
static std::string given(local::Hush &hush, const proto::Act &a, bool quiet, double now) {
    static const char *const kWhat[] = {"letgo", "move", "button", "scroll", "key"};
    std::vector<proto::Act> out;
    hush.Act(a, quiet, now, out);
    std::string said;
    for (const proto::Act &o : out)
        said += (said.empty() ? "" : ", ") + std::string(o.what <= proto::kActKey ? kWhat[o.what] : "?") + (o.panel == 9 ? "" : "@" + std::to_string(o.panel)) + " " + std::to_string(o.a) + " " +
                std::to_string(o.b);
    return said.empty() ? "-" : said;
}

static void pointer() {
    local::Hush open;
    check("a pointer that isn't quiet is told as it is", through(open, point(9, 100, 200), false, 0) == "9:100:200" && through(open, point(9, 101, 200, true), false, 0) == "9:101:200!" &&
                                                           through(open, point(proto::kNoPanel, 0, 0), false, 0) == "255:0:0");
    check("...and every act goes as it was said",
          given(open, act(proto::kActMove, 5, 6), false, 0) == "move 5 6" && given(open, act(proto::kActButton, proto::kLeft, 1), false, 0) == "button 1 1" &&
              given(open, act(proto::kActMove, 7, 8), false, 0) == "move 7 8" && given(open, act(proto::kActButton, proto::kLeft, 0), false, 0) == "button 1 0" &&
              given(open, act(proto::kActScroll, 0, 120), false, 0) == "scroll 0 120" && given(open, act(proto::kActKey, 30, 1), false, 0) == "key 30 1");

    local::Hush h;
    check("a quiet pointer that only moves is told to nobody", through(h, point(9, 100, 200), true, 0) == "-" && through(h, point(9, 300, 200), true, 0.1) == "-");
    check("...nor given to the panel's owner", given(h, act(proto::kActMove, 100, 200), true, 0) == "-" && given(h, act(proto::kActMove, 300, 250), true, 0.1) == "-");
    check("a click goes where the pointer last was: the move first, then the button", given(h, act(proto::kActButton, proto::kLeft, 1), true, 0.2) == "move 300 250, button 1 1");
    check("...and the room sees it pressed there", through(h, point(9, 300, 250, true), true, 0.2) == "9:300:250!");
    check("while the button is held the moves go: a drag", given(h, act(proto::kActMove, 310, 250), true, 0.3) == "move 310 250" && through(h, point(9, 310, 250, true), true, 0.3) == "9:310:250!");
    check("the release goes", given(h, act(proto::kActButton, proto::kLeft, 0), true, 0.4) == "button 1 0");
    check("...and the room is told the pointer is nowhere, once", through(h, point(9, 310, 250), true, 0.4) == "255:0:0" && through(h, point(9, 311, 250), true, 0.5) == "-");
    check("after it, moves wait again", given(h, act(proto::kActMove, 400, 100), true, 0.5) == "-");
    check("a scroll takes the move with it too, and no move twice", given(h, act(proto::kActScroll, 0, -120), true, 0.6) == "move 400 100, scroll 0 -120" &&
                                                                         given(h, act(proto::kActScroll, 0, -120), true, 0.7) == "scroll 0 -120");
    check("...and moves wait after a scroll", given(h, act(proto::kActMove, 401, 100), true, 0.8) == "-");
    check("keys go whatever the pointer does, and take no move with them", given(h, act(proto::kActKey, 30, 1), true, 0.9) == "key 30 1" && given(h, act(proto::kActKey, 30, 0), true, 0.9) == "key 30 0");
    check("a move that waited for one panel doesn't go before a click on another", given(h, act(proto::kActButton, proto::kRight, 1, 17), true, 1.0) == "button@17 2 1" &&
                                                                                       given(h, act(proto::kActButton, proto::kRight, 0, 17), true, 1.0) == "button@17 2 0");
    check("leaving the panel is told as ever, when the room knew the pointer was there", through(h, point(9, 5, 5, true), true, 1.1) == "9:5:5!" &&
                                                                                           through(h, point(proto::kNoPanel, 0, 0), true, 1.2) == "255:0:0");

    local::Hush none;
    check("a quiet pointer that points at nothing is told as it is: that says nothing of the eyes", through(none, point(proto::kNoPanel, 0, 0), true, 0) == "255:0:0" &&
                                                                                                     through(none, point(proto::kNoPanel, 0, 0), true, 1) == "255:0:0");

    local::Hush two;
    given(two, act(proto::kActButton, proto::kLeft, 1), true, 0);
    given(two, act(proto::kActButton, proto::kRight, 1), true, 0);
    given(two, act(proto::kActButton, proto::kLeft, 0), true, 0.1);
    check("with two buttons down, one coming up leaves it a drag", given(two, act(proto::kActMove, 1, 1), true, 0.2) == "move 1 1");
    given(two, act(proto::kActButton, proto::kRight, 0), true, 0.3);
    check("...until the other is up too", given(two, act(proto::kActMove, 2, 2), true, 0.4) == "-");

    local::Hush stuck;
    given(stuck, act(proto::kActButton, proto::kLeft, 1), true, 0);
    check("a held button stands three seconds after its last word", given(stuck, act(proto::kActMove, 1, 1), true, 2.9) == "move 1 1" && given(stuck, act(proto::kActMove, 2, 2), true, 5.8) == "move 2 2");
    check("...and no longer: one whose release never came doesn't open the pointer for good", given(stuck, act(proto::kActMove, 3, 3), true, 9) == "-");
    given(stuck, act(proto::kActButton, proto::kLeft, 1), true, 10);
    through(stuck, point(9, 3, 3, true), true, 12.5);
    check("a pointer that rests pressed keeps it held", given(stuck, act(proto::kActMove, 4, 4), true, 15) == "move 4 4");
    through(stuck, point(17, 3, 3, true), true, 17.5);
    check("...but not one pressed on another panel", given(stuck, act(proto::kActMove, 5, 5), true, 18.1) == "-");
    given(stuck, act(proto::kActButton, proto::kLeft, 1), true, 20);
    check("a move on another panel than the button's is no drag", given(stuck, act(proto::kActMove, 6, 6, 17), true, 20.1) == "-");
    check("a button nobody has opens nothing", given(stuck, act(proto::kActButton, 0, 1), true, 21) == "button 0 1" && given(stuck, act(proto::kActMove, 7, 7), true, 21.1) == "-" &&
                                                 given(stuck, act(proto::kActButton, 40, 1), true, 21.2) == "move 7 7, button 40 1" && given(stuck, act(proto::kActMove, 8, 8), true, 21.3) == "-");

    local::Hush middle;
    check("the middle button drags as the others do", given(middle, act(proto::kActButton, proto::kMiddle, 1), true, 0) == "button 3 1" && given(middle, act(proto::kActMove, 1, 1), true, 0.1) == "move 1 1");
    local::Hush wheel;
    check("a scroll is no button, whatever its numbers", given(wheel, act(proto::kActScroll, proto::kLeft, 120), true, 0) == "scroll 1 120" && given(wheel, act(proto::kActMove, 1, 1), true, 0.1) == "-");

    local::Hush turns;
    check("when the pointer stops being quiet, the next move goes, and nothing that waited", given(turns, act(proto::kActMove, 1, 1), true, 0) == "-" &&
                                                                                                given(turns, act(proto::kActMove, 2, 2), false, 0.1) == "move 2 2" &&
                                                                                                given(turns, act(proto::kActButton, proto::kLeft, 1), false, 0.2) == "button 1 1");
    check("...and when it turns quiet over a pointer the room sees, the room is told it is nowhere", through(turns, point(9, 1, 1), false, 1) == "9:1:1" &&
                                                                                                        through(turns, point(9, 2, 2), true, 1.1) == "255:0:0" &&
                                                                                                        through(turns, point(9, 3, 3), true, 1.2) == "-");
}

static std::vector<uint8_t> pose(bool claims = false) {
    proto::Pose p;
    p.eyes[1] = 1.6f, p.looks = claims, p.gaze[0] = 40;
    return proto::Pack(p);
}

static void own() {
    local::Own own;
    std::vector<uint8_t> out = {1, 2, 3};
    proto::Pose p;
    const std::vector<uint8_t> mine = pose(), claims = pose(true);
    bool ok = own.Take(mine.data(), mine.size(), 0, out) && proto::Read(out.data(), out.size(), p) && !p.looks && p.turn == 1;
    ok = ok && own.Take(claims.data(), claims.size(), 0, out) && proto::Read(out.data(), out.size(), p) && !p.looks && p.turn == 2 && out.size() == mine.size();
    check("the wearer's pose goes out counted, and with no gaze: not even one the pose itself claims", ok);
    own.gaze().Share(true);
    own.gaze().Say(12, -3, 1);
    ok = own.Take(mine.data(), mine.size(), 1.5, out) && proto::Read(out.data(), out.size(), p) && p.looks && p.gaze[0] == 12 && p.gaze[1] == -3 && p.turn == 3;
    ok = ok && own.Take(claims.data(), claims.size(), 1.6, out) && proto::Read(out.data(), out.size(), p) && p.looks && p.gaze[0] == 12;
    check("...with where they look once they share it and it is told", ok);
    ok = own.Take(mine.data(), mine.size(), 2.5, out) && proto::Read(out.data(), out.size(), p) && !p.looks;
    check("...and without when it stopped being told", ok);

    const uint8_t mouth[local::kMouthBytes] = {local::kMouthToTogether, 200, 0, 0, 0, 9};
    ok = own.Take(mouth, sizeof mouth, 3, out) && out.empty() && own.Take(mine.data(), mine.size(), 3.1, out) && proto::Read(out.data(), out.size(), p) && p.mouth[0] == 200 && p.mouth[4] == 9;
    check("a mouth is kept for the poses and goes nowhere itself", ok);

    std::vector<uint8_t> voice = {proto::kVoiceDatagram, 7, 0, 0xfc, 0xff, 0xfe};
    ok = own.Take(voice.data(), voice.size(), 4, out) && out == voice;
    voice.resize(3);
    check("a voice goes as it came, and what only begins like one goes nowhere", ok && own.Take(voice.data(), voice.size(), 4, out) && out.empty());

    local::Own quiet;   // nobody said how its pointer is moved
    const std::vector<uint8_t> hover = proto::Pack(point(9, 100, 200)), press = proto::Pack(point(9, 100, 200, true));
    proto::Point q;
    ok = quiet.Take(hover.data(), hover.size(), 0, out) && out.empty();
    ok = ok && quiet.Take(press.data(), press.size(), 0, out) && proto::Read(out.data(), out.size(), q) && q.panel == 9 && q.count == 1 && (q.flags & proto::kPointPressed);
    ok = ok && quiet.Take(hover.data(), hover.size(), 0, out) && proto::Read(out.data(), out.size(), q) && q.panel == proto::kNoPanel && q.count == 2;
    check("a pointer the eyes may be moving is told only while pressed, with ft-together's count", ok);
    quiet.gaze().Pointer(false, 1);
    ok = quiet.Take(hover.data(), hover.size(), 1, out) && proto::Read(out.data(), out.size(), q) && q.panel == 9 && q.x == 100 && q.count == 3;
    check("...and wherever it is once the hand is known to move it", ok);
    quiet.gaze().Pointer(true, 2);
    ok = quiet.Take(hover.data(), hover.size(), 2, out) && proto::Read(out.data(), out.size(), q) && q.panel == proto::kNoPanel && quiet.Take(hover.data(), hover.size(), 2, out) && out.empty();
    quiet.gaze().Share(true);
    ok = ok && quiet.Take(hover.data(), hover.size(), 2, out) && proto::Read(out.data(), out.size(), q) && q.panel == 9;
    check("...not while the eyes move it, unless where they look is shared anyway", ok);
    std::vector<uint8_t> bad = hover;
    bad.push_back(0);
    check("what only begins like a pointer or a pose goes nowhere", quiet.Take(bad.data(), bad.size(), 2, out) && out.empty() && quiet.Take(mine.data(), mine.size() - 1, 2, out) && out.empty());

    std::vector<proto::Act> go;
    local::Own hand;
    hand.Act(act(proto::kActMove, 1, 2), 0, go);
    ok = go.empty();
    hand.Act(act(proto::kActButton, proto::kLeft, 1), 0, go);
    ok = ok && go.size() == 2 && go[0].what == proto::kActMove && go[1].what == proto::kActButton;
    go.clear();
    hand.gaze().Pointer(false, 1);
    hand.Act(act(proto::kActButton, proto::kLeft, 0), 1, go);
    hand.Act(act(proto::kActMove, 3, 4), 1, go);
    check("the wearer's acts pass the same rule: a move waits for the click while the eyes may be moving the pointer", ok && go.size() == 2 && go[1].what == proto::kActMove && go[1].a == 3);

    const std::string text = "state";
    check("a question is none of these", !own.Take(reinterpret_cast<const uint8_t *>(text.data()), text.size(), 5, out) && out.empty() && !own.Take(nullptr, 0, 5, out));
}

static std::string ask(local::Gaze &gaze, const std::string &asked, bool inRoom, double now) {
    std::string reply = "not asked";
    return local::GazeAsk(gaze, asked, inRoom, now, reply) ? reply : "not a gaze question";
}

static void asks() {
    local::Gaze g;
    check("asked, it says nothing is shared and the pointer is quiet", ask(g, "gaze", false, 0) == "ok {\"shared\":false,\"fresh\":false,\"quiet\":true}");
    check("it can't be turned on outside a room", ask(g, "gaze on", false, 0) == "error you aren't in a room" && !g.shared());
    check("...and a direction isn't taken before it is", ask(g, "gaze say 3 4", true, 0) == "error you don't share where you look");
    check("in a room it can", ask(g, "gaze on", true, 0) == "ok" && ask(g, "gaze", true, 0) == "ok {\"shared\":true,\"fresh\":false,\"quiet\":false}");
    check("a direction is told", ask(g, "gaze say 12.5 -3", true, 1) == "ok" && ask(g, "gaze", true, 1.5) == "ok {\"shared\":true,\"fresh\":true,\"quiet\":false}" && looks(g, 1.5) == "12.5,-3");
    check("...with numbers and nothing else", ask(g, "gaze say 1", true, 1).rfind("error gaze |", 0) == 0 && ask(g, "gaze say 1 2 3", true, 1).rfind("error gaze |", 0) == 0 &&
                                                ask(g, "gaze say 1x 2", true, 1).rfind("error gaze |", 0) == 0 && ask(g, "gaze say  ", true, 1).rfind("error gaze |", 0) == 0 &&
                                                ask(g, "gaze say 1 nan", true, 1) == "error that is no direction" && ask(g, "gaze say inf 0", true, 1) == "error that is no direction" &&
                                                ask(g, "gaze say 00000000000000001 2", true, 1).rfind("error gaze |", 0) == 0 && looks(g, 1.5) == "12.5,-3");
    check("it is turned off, in a room or out", ask(g, "gaze off", false, 2) == "ok" && !g.shared() && ask(g, "gaze", true, 2) == "ok {\"shared\":false,\"fresh\":false,\"quiet\":true}");
    check("how the pointer is moved is said", ask(g, "gaze pointer hand", false, 3) == "ok" && !g.quiet(3) && ask(g, "gaze", false, 3) == "ok {\"shared\":false,\"fresh\":false,\"quiet\":false}" &&
                                                ask(g, "gaze pointer eyes", false, 4) == "ok" && g.quiet(4));
    check("...in those two words only", ask(g, "gaze pointer", false, 4).rfind("error gaze |", 0) == 0 && ask(g, "gaze pointer feet", false, 4).rfind("error gaze |", 0) == 0 &&
                                          ask(g, "gaze pointer hand now", false, 4).rfind("error gaze |", 0) == 0 && g.quiet(4));
    check("anything else about it is answered with what there is", ask(g, "gaze up", true, 4).rfind("error gaze |", 0) == 0 && ask(g, "gaze on now", true, 4).rfind("error gaze |", 0) == 0 &&
                                                                     ask(g, "gaze off now", true, 4).rfind("error gaze |", 0) == 0 && !g.shared());
    check("what only begins like it isn't its question", ask(g, "gazes", true, 4) == "not a gaze question" && ask(g, "state", true, 4) == "not a gaze question" && ask(g, "", true, 4) == "not a gaze question");
}

int main() {
    rules();
    pointer();
    own();
    asks();
    std::printf(failed ? "FAILED\n" : "all ok\n");
    return failed != 0;
}
