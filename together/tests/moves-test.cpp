// Moving shared panels (together/flow/moves.h, behind together/local/moveask.h's lines of text;
// together/proto/move.h has the bytes): several Frames' sessions, their control and their moves
// run against each other on a network made in the test (fakenet.h). Who may move whose panel;
// where a move ends up and for how long; what goes nowhere; and people who say what they may
// not. No sockets, no clock.
#include "../flow/control.h"
#include "../flow/moves.h"
#include "../local/controlask.h"
#include "../local/moveask.h"
#include "fakenet.h"

#include <cstdio>

static int failed = 0;
static bool check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

static proto::Move move(uint8_t panel, float x, float y, float z) {
    proto::Move m;
    m.panel = panel, m.at[0] = x, m.at[1] = y, m.at[2] = z;
    return m;
}

// One Frame's control and moves. Nobody's input is looked at here.
struct Hand : flow::Inputs {
    Frame &h;
    flow::Control control;
    flow::Moves moves;

    explicit Hand(Frame &frame)
        : h(frame), control(*frame.s, *this), moves(*frame.s, [this](int number, int person, double now) { return control.Granted(number, person, now); },
                                                     [this](uint8_t panel, double now) { return control.Holds(panel, now); }) {
        h.onSaid = [this](int link, uint16_t kind, const std::vector<uint8_t> &body, double now) {
            return kind == proto::kMove ? moves.Said(link, body, now) : control.Said(link, kind, body, now);
        };
    }
    void Input(int, const proto::Act &) override {}
    std::string ask(const std::string &text) {
        std::string reply = "nobody's";
        if (!local::ControlAsk(control, text, h.net.now, reply)) local::MoveAsk(moves, text, h.net.now, reply);
        h.net.pump();
        return reply;
    }
    // What the wearer's own program sends without asking its ft-together whether it may.
    void forge(int to, const std::vector<uint8_t> &message) {
        h.s->Tell(to, message);
        h.net.pump();
    }
};

static const char *kScreen = " 1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen";
static const char *kLevel = " 1 0 0 0";   // a panel that faces the way the room does
static const uint8_t kA0 = 0, kA1 = 1, kB0 = 8;   // Ann's two panels and Bo's one, by their ids

struct Table {
    Net net;
    Frame a{net, "100.64.0.1", 47193, "Ann"}, b{net, "100.64.0.2", 47193, "Bo"}, c{net, "100.64.0.3", 47193, "Cy"};
    Hand ha{a}, hb{b}, hc{c};

    bool sit() {
        return a.ask("open") == "ok" && join(a, b) && join(a, c) && b.s->Linked(c.s->me()) && c.s->Linked(b.s->me()) && a.ask(std::string("share 0") + kScreen) == "ok" &&
               a.ask(std::string("share 1") + kScreen) == "ok" && b.ask(std::string("share 0") + kScreen) == "ok";
    }
    // Time goes by, a tenth of a second at a look. `here`: someone says that wearer is there.
    void tick(double seconds, Hand *here = nullptr) {
        for (const double end = net.now + seconds - 1e-6; net.now < end;) {
            net.now += 0.1;
            if (here) here->control.Here(net.now);
            for (Hand *hand : {&ha, &hb, &hc}) hand->h.s->Tick(net.now), hand->control.Tick(net.now), hand->moves.Tick(net.now);
            net.pump();
        }
    }
};

static void bytes() {
    proto::Move m = move(13, 1.5f, -2.25f, 99), back;
    m.turn[0] = 0.5f, m.turn[1] = -0.5f, m.turn[2] = 0.5f, m.turn[3] = 0.5f;
    const std::vector<uint8_t> body = proto::Body(m);
    bool ok = proto::Read(body, back) && back.panel == 13 && back.at[0] == 1.5f && back.at[1] == -2.25f && back.at[2] == 99;
    for (int i = 0; i < 4; ++i) ok = ok && std::fabs(back.turn[i] - m.turn[i]) < 1e-4f;
    check("a move reads back as it was written: the panel, where it is, how it is turned", ok && body.size() == 21);
    std::vector<uint8_t> shorter(body.begin(), body.end() - 1), longer = body;
    longer.push_back(0);
    check("one that is a byte short or a byte long isn't one", !proto::Read(shorter, back) && !proto::Read(longer, back));
    check("nor is one for a panel no room has", !proto::Read(proto::Body(move(32, 0, 0, 0)), back) && proto::Read(proto::Body(move(31, 0, 0, 0)), back));
    check("nor one further out than the room goes, or nowhere", !proto::Read(proto::Body(move(0, 100.5f, 0, 0)), back) && !proto::Read(proto::Body(move(0, 0, -101, 0)), back) &&
                                                                  !proto::Read(proto::Body(move(0, 0, 0, NAN)), back) && !proto::Read(proto::Body(move(0, INFINITY, 0, 0)), back) &&
                                                                  proto::Read(proto::Body(move(0, 100, -100, 100)), back));
    proto::Move none = move(0, 0, 0, 0), half = none, twice = none, two = none;
    none.turn[0] = 0, half.turn[0] = 0.5f, twice.turn[0] = 1, twice.turn[1] = 0.3f, two.turn[1] = 1;
    check("nor one whose turn is none, or more than one", !proto::Read(proto::Body(none), back) && !proto::Read(proto::Body(half), back) && !proto::Read(proto::Body(two), back));
    ok = proto::Read(proto::Body(twice), back);
    double norm = 0;
    for (float v : back.turn) norm += double(v) * v;
    check("a turn a little off is made one again", ok && std::fabs(norm - 1) < 1e-6);
}

static void moving() {
    Table t;
    Hand &a = t.ha, &b = t.hb, &c = t.hc;
    if (!check("three people sit in a room, two of them sharing panels", t.sit() && t.a.s->room().panel(kA0) && t.a.s->room().panel(kA1) && t.c.s->room().panel(kB0))) return;
    check("nobody put anything anywhere yet", a.ask("moves") == "ok 0" && b.ask("moves") == "ok 0");

    // the room's owner
    check("the room's owner moves a guest's panel", a.ask(std::string("move 8 1.5 1.25 -3") + kLevel) == "ok");
    check("...and its owner's Frame is told where, and by whom", b.ask("moves") == "ok 1 0:0:1.5000:1.2500:-3.0000:1.00000:0.00000:0.00000:0.00000");
    check("...once", b.ask("moves") == "ok 0");
    check("the room has it where it was until its owner's Frame says otherwise", t.a.s->room().panel(kB0)->at[2] == -2);
    bool ok = a.ask(std::string("move 8 1 1 -1") + kLevel) == "ok" && a.ask("move 8 2 1 -1 0.7071 0 0.7071 0") == "ok";
    check("of two moves the newer is kept", ok && b.ask("moves") == "ok 1 0:0:2.0000:1.0000:-1.0000:0.70711:0.00000:0.70711:0.00000");

    // everyone else
    check("a guest doesn't move someone's panel unasked", c.ask(std::string("move 0 1 1 -1") + kLevel) == "error you may not move that panel" &&
                                                           c.ask(std::string("move 8 1 1 -1") + kLevel) == "error you may not move that panel");
    c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));
    c.forge(1, proto::Pack(move(kB0, 1, 1, -1)));
    check("...and what is sent anyway goes nowhere", a.ask("moves") == "ok 0" && b.ask("moves") == "ok 0" && a.moves.dropped() == 1 && b.moves.dropped() == 1 && t.a.s->room().person(2));
    check("your own panel moves with its screen, not by a word", b.ask(std::string("move 8 1 1 -1") + kLevel) == "error that panel is yours");
    check("a panel there isn't moves nowhere", a.ask(std::string("move 9 1 1 -1") + kLevel) == "error no such panel" && a.ask(std::string("move 40 1 1 -1") + kLevel) == "error no such panel");
    check("a place the room doesn't have isn't sent", a.ask(std::string("move 8 150 1 -1") + kLevel) == "error that isn't a place in the room" &&
                                                        a.ask("move 8 1 1 -1 0 0 0 0") == "error that isn't a place in the room" && b.ask("moves") == "ok 0");

    // a grant
    ok = a.ask("control here") == "ok" && a.ask("control give 0 1") == "ok";
    check("someone given a panel moves it", ok && b.ask(std::string("move 0 -1 1.5 -2.5") + kLevel) == "ok" && a.ask("moves") == "ok 1 0:1:-1.0000:1.5000:-2.5000:1.00000:0.00000:0.00000:0.00000");
    check("the grant is for that panel", b.ask(std::string("move 1 1 1 -1") + kLevel) == "error you may not move that panel" &&
                                           (b.forge(0, proto::Pack(move(kA1, 1, 1, -1))), a.ask("moves") == "ok 0") && t.a.s->room().person(1));
    c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));
    check("...and for that person", a.ask("moves") == "ok 0" && t.a.s->room().person(2));
    ok = a.ask("control give 1 2") == "ok" && b.ask(std::string("move 0 1 1 -1") + kLevel) == "ok" && c.ask(std::string("move 1 2 1 -1") + kLevel) == "ok";
    check("each panel's place is kept by itself", ok && a.ask("moves") == "ok 2 0:1:1.0000:1.0000:-1.0000:1.00000:0.00000:0.00000:0.00000 1:2:2.0000:1.0000:-1.0000:1.00000:0.00000:0.00000:0.00000");
    ok = b.ask(std::string("move 0 1 1 -1") + kLevel) == "ok" && c.ask(std::string("move 1 2 1 -1") + kLevel) == "ok" && a.ask("control take 0 1") == "ok";
    check("taken back, it is theirs to move no longer", ok && b.ask(std::string("move 0 3 1 -1") + kLevel) == "error you may not move that panel" &&
                                                         (b.forge(0, proto::Pack(move(kA0, 3, 1, -1))), a.ask("moves") == "ok 2 0:1:1.0000:1.0000:-1.0000:1.00000:0.00000:0.00000:0.00000 1:2:2.0000:1.0000:-1.0000:1.00000:0.00000:0.00000:0.00000"));

    // the lease
    t.tick(1, &a);
    ok = a.ask("control give 0 1") == "ok" && b.ask(std::string("move 0 1 1 -1") + kLevel) == "ok" && a.ask("moves").rfind("ok 1 0:1:", 0) == 0;
    t.tick(10.5);
    const size_t dropped = a.moves.dropped();
    b.forge(0, proto::Pack(move(kA0, 3, 1, -1)));
    check("a grant that ran out, with no word that the wearer is there, moves nothing", ok && b.ask(std::string("move 0 1 1 -1") + kLevel) == "error you may not move that panel" &&
                                                                                        a.ask("moves") == "ok 0" && a.moves.dropped() == dropped + 1);
    check("the room's owner needs none: a guest's panel is theirs to move though its guest isn't there", a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok" && b.ask("moves").rfind("ok 1 0:0:", 0) == 0);

    // what isn't taken
    ok = a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok";
    t.tick(4.9);
    check("a place waits a few seconds for the program that takes it", ok && a.ask(std::string("move 8 0 2 -1") + kLevel) == "ok" && (t.tick(4.9), b.ask("moves").rfind("ok 1 0:0:0.0000:2.0000:", 0) == 0));
    ok = a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok";
    t.tick(5.2);
    check("...and no longer", ok && b.ask("moves") == "ok 0");
    ok = a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok";
    t.net.now += 5.2;   // and nobody looked meanwhile
    check("...whether anyone looked meanwhile or not", ok && b.ask("moves") == "ok 0");
    ok = a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok" && t.b.ask("unshare 0") == "ok";
    t.tick(0.1);
    check("a panel that is no longer shared isn't put anywhere", ok && b.ask("moves") == "ok 0" && a.ask(std::string("move 8 0 1 -1") + kLevel) == "error no such panel");
    const size_t gone = b.moves.dropped();
    a.forge(1, proto::Pack(move(kB0, 0, 1, -1)));
    check("...whatever is sent: it is dropped as it comes", b.moves.dropped() == gone + 1 && b.ask("moves") == "ok 0" && t.b.s->room().person(0));

    // leaving
    ok = t.b.ask(std::string("share 0") + kScreen) == "ok" && a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok" && t.b.ask("leave") == "ok";
    t.tick(0.1);
    check("out of the room, nothing of it is kept and nothing is moved", ok && b.ask("moves") == "ok 0" && b.ask(std::string("move 0 0 1 -1") + kLevel) == "error you aren't in a room");
    check("what isn't a question about moving is someone else's", b.ask("state") == "nobody's" && b.ask("moved") == "nobody's" && b.ask("move").rfind("error move PANEL", 0) == 0 &&
                                                                    b.ask("move 0 1 2 3 1 0 0").rfind("error move PANEL", 0) == 0 && b.ask("move 0 1 2 3 1 0 0 0 0").rfind("error move PANEL", 0) == 0 &&
                                                                    b.ask("move 0.5 1 2 3 1 0 0 0").rfind("error move PANEL", 0) == 0 && b.ask("move 0 1 x 3 1 0 0 0").rfind("error move PANEL", 0) == 0 &&
                                                                    b.ask("move 0 1 nan 3 1 0 0 0").rfind("error move PANEL", 0) == 0 && b.ask("move -1 1 2 3 1 0 0 0").rfind("error move PANEL", 0) == 0);
}

// Whether Cy, in the room, is put out of it for saying `message` to Ann. They come in again after.
static bool thrown(Table &t, const std::vector<uint8_t> &message) {
    if (!t.a.s->room().person(2)) return false;
    t.hc.forge(0, message);
    const bool out = !t.a.s->room().person(2) && t.c.s->mode() == session::Mode::kIdle;
    t.tick(0.1, &t.ha);
    if (out && !join(t.a, t.c)) return false;
    return out;
}

static void rules() {
    Table t;
    Hand &a = t.ha, &b = t.hb, &c = t.hc;
    if (!check("three people sit in a room", t.sit())) return;
    wire::Out three;
    three.U8(0), three.U8(1), three.U8(2);
    std::vector<uint8_t> far = proto::Body(move(kA0, 0, 0, 0));
    const float out = 500;
    std::memcpy(far.data() + 1, &out, 4);
    wire::Out beyond;
    beyond.b = far;

    check("a move of a panel that isn't the reader's closes the link", thrown(t, proto::Pack(move(kB0, 1, 1, -1))) && b.ask("moves") == "ok 0");
    check("...so does one that doesn't read, or that puts a panel out of the room", thrown(t, proto::Framed(proto::kMove, three)) && thrown(t, proto::Framed(proto::kMove, beyond)));
    check("a kind nobody knows is passed over", !thrown(t, proto::Framed(99, three)) && t.a.s->room().person(2));

    for (int i = 0; i < 60; ++i) c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));
    check("sixty moves in one go are taken, though they move nothing", t.a.s->room().person(2) && a.moves.dropped() == 60);
    check("...and one more closes the link", thrown(t, proto::Pack(move(kA0, 1, 1, -1))));
    for (int i = 0; i < 60; ++i) c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));   // (their new link's sixty)
    t.tick(1.1, &a);
    for (int i = 0; i < 32; ++i) c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));
    check("a second later a link takes thirty more", t.a.s->room().person(2) && a.moves.dropped() == 60 + 60 + 32);
    for (int i = 0; i < 2; ++i) c.forge(0, proto::Pack(move(kA0, 1, 1, -1)));   // (1.1 s: the 33rd is on the edge, the 34th is not)
    const bool closed = !t.a.s->room().person(2);
    t.tick(0.1, &a);
    check("...and no more than that", closed && join(t.a, t.c));
    check("a panel number past the last one isn't some other panel", a.ask(std::string("move 264 0 1 -1") + kLevel).rfind("error move PANEL", 0) == 0 && b.ask("moves") == "ok 0");

    int sent = 0;
    for (int i = 0; i < 50; ++i) sent += a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok";
    check("the wearer's own programs are held to less than a link takes: the rest isn't sent", sent == 40 && t.b.s->room().person(0) && a.ask(std::string("move 8 0 1 -1") + kLevel) == "error too fast");
    t.tick(1);
    check("...and a second later they go again", a.ask(std::string("move 8 0 1 -1") + kLevel) == "ok");

    const Net::End s = t.net.stranger(t.a, "100.64.0.9");
    t.net.say(s, proto::Pack(move(kA0, 1, 1, -1)));
    check("a stranger's move closes their link, and moves nothing", t.net.shut.count(s.link) && a.ask("moves") == "ok 0");
    check("nor is one taken from a link nobody in the room is on", !a.moves.Said(s.link, proto::Body(move(kA0, 1, 1, -1)), t.net.now) && a.ask("moves") == "ok 0");

    // someone else in a place
    bool ok = a.ask("control here") == "ok" && a.ask("control give 0 2") == "ok" && c.ask(std::string("move 0 1 1 -1") + kLevel) == "ok" && a.ask("moves").rfind("ok 1 0:2:", 0) == 0;
    Frame d{t.net, "100.64.0.4", 47193, "Di"};
    Hand hd{d};
    ok = ok && t.c.ask("leave") == "ok" && join(t.a, d) && d.s->me() == 2;
    hd.forge(0, proto::Pack(move(kA0, 1, 1, -1)));
    check("someone who takes the place of one who left has none of their grants, however soon they came", ok && a.ask("moves") == "ok 0" && t.a.s->room().person(2));
    t.net.frames.pop_back();   // Di goes before the table does
}

int main() {
    bytes();
    moving();
    rules();
    std::printf(failed ? "FAILED\n" : "all ok\n");
    return failed != 0;
}
