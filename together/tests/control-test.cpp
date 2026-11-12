// Control of shared panels (together/flow/control.h, behind together/local/controlask.h's
// lines of text): several Frames' sessions and their control's rules run against each other on
// a network made in the test (fakenet.h), each with a desktop that remembers the input it was
// given. Asking, giving and taking back; whose input reaches a desktop and whose doesn't; one
// person at a time; what is let go for whom; a grant that runs out with no word that the
// wearer is there; people who say what they may not; and an act's gate. No sockets, no clock.
#include "../flow/control.h"
#include "../local/controlask.h"
#include "fakenet.h"

#include <cstdio>

static int failed = 0;
static bool check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

using Headset = Frame;

static proto::Act act(uint8_t panel, uint8_t what, int32_t a, int32_t b) {
    proto::Act m;
    m.panel = panel, m.what = what, m.a = a, m.b = b;
    return m;
}

// One Frame's control, and its desktop: the input it was given, and whose.
struct Hand : flow::Inputs {
    Headset &h;
    flow::Control control;
    std::vector<std::pair<int, proto::Act>> got;

    explicit Hand(Headset &headset) : h(headset), control(*headset.s, *this) {
        h.onSaid = [this](int link, uint16_t kind, const std::vector<uint8_t> &body, double now) { return control.Said(link, kind, body, now); };
    }
    void Input(int person, const proto::Act &a) override { got.push_back({person, a}); }
    std::string ask(const std::string &text) {
        std::string reply = "not control's";
        local::ControlAsk(control, text, h.net.now, reply);
        h.net.pump();
        return reply;
    }
    bool has(const std::string &text) { return ask("control").find(text) != std::string::npos; }
    // The last input was this, from them.
    bool last(int person, uint8_t what, int32_t a, int32_t b) const {
        return !got.empty() && got.back().first == person && got.back().second.what == what && got.back().second.a == a && got.back().second.b == b;
    }
    // What the wearer's own program sends without asking its ft-together whether it may.
    void forge(int to, const std::vector<uint8_t> &message) {
        h.s->Tell(to, message);
        h.net.pump();
    }
};

static const char *kScreen = " 1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen";

struct Table {
    Net net;
    Headset a{net, "100.64.0.1", 47193, "Ann"}, b{net, "100.64.0.2", 47193, "Bo"}, c{net, "100.64.0.3", 47193, "Cy"}, d{net, "100.64.0.4", 47193, "Di"};
    Hand ha{a}, hb{b}, hc{c}, hd{d};

    bool sit() {
        return a.ask("open") == "ok" && join(a, b) && join(a, c) && b.s->Linked(c.s->me()) && c.s->Linked(b.s->me()) && a.ask(std::string("share 0") + kScreen) == "ok" &&
               a.ask(std::string("share 1") + kScreen) == "ok" && b.ask(std::string("share 0") + kScreen) == "ok";
    }
    // Time goes by, a tenth of a second at a look. `here`: someone says that wearer is there.
    void tick(double seconds, Hand *here = nullptr) {
        for (const double end = net.now + seconds - 1e-6; net.now < end;) {
            net.now += 0.1;
            if (here) here->control.Here(net.now);
            for (Hand *hand : {&ha, &hb, &hc, &hd}) hand->h.s->Tick(net.now), hand->control.Tick(net.now);
            net.pump();
        }
    }
};

static const uint8_t kA0 = 0, kA1 = 1, kB0 = 8;   // Ann's two panels and Bo's one, by their ids

static void granting() {
    Table t;
    Hand &a = t.ha, &b = t.hb, &c = t.hc;
    if (!check("three people sit in a room, two of them sharing panels", t.sit() && t.a.s->room().panel(kA0) && t.a.s->room().panel(kA1) && t.c.s->room().panel(kB0))) return;

    check("without a grant nothing is sent", b.ask("control act 0 move 100 200") == "error you have no control of that panel" && a.got.empty());
    b.forge(0, proto::Pack(act(kA0, proto::kActMove, 100, 200)));
    check("...and what is sent anyway goes nowhere", a.got.empty() && a.control.dropped() == 1 && t.a.s->room().person(1));
    check("your own panel isn't asked for, nor one there isn't", b.ask("control ask 8") == "error that panel is yours" && b.ask("control ask 5") == "error no such panel");

    check("someone asks for a panel, and its owner sees who", b.ask("control ask 0") == "ok" && a.has("\"asks\":[{\"number\":0,\"person\":1}]") && b.has("\"asked\":[0]"));
    check("an ask alone is nothing", b.ask("control act 0 move 1 1") == "error you have no control of that panel");
    check("it is given only while someone says the wearer is there", a.ask("control give 0 1").rfind("error nobody says you have the headset on", 0) == 0 && a.has("\"there\":false"));
    check("...and then it is", a.ask("control here") == "ok" && a.ask("control give 0 1") == "ok" && a.has("\"asks\":[],\"given\":[{\"number\":0,\"person\":1}]") &&
                                   b.has("\"held\":[{\"panel\":0,\"left\":10}],\"asked\":[]"));
    check("only a panel you share, to someone who is there, and not to yourself",
          a.ask("control give 2 1") == "error you don't share that panel" && a.ask("control give 0 3") == "error nobody is in that place" && a.ask("control give 0 0") == "error nobody is in that place");

    bool ok = b.ask("control act 0 move 100 200") == "ok" && a.last(1, proto::kActMove, 100, 200);
    ok = ok && b.ask("control act 0 button 1 1") == "ok" && a.last(1, proto::kActButton, 1, 1) && b.ask("control act 0 button 1 0") == "ok" && a.last(1, proto::kActButton, 1, 0);
    ok = ok && b.ask("control act 0 key 30 1") == "ok" && b.ask("control act 0 key 30 0") == "ok" && a.last(1, proto::kActKey, 30, 0);
    ok = ok && b.ask("control act 0 scroll 0 -120") == "ok" && a.last(1, proto::kActScroll, 0, -120);
    check("their input reaches the owner's desktop, in order, as theirs", ok && a.got.size() == 6 && a.got[0].second.panel == kA0 && a.control.passed() == 6);
    check("what no panel takes isn't sent", b.ask("control act 0 move 70000 0") == "error that isn't input a panel takes" && b.ask("control act 0 key 125 1") == "error that isn't input a panel takes" &&
                                              b.ask("control act 0 button 4 1") == "error that isn't input a panel takes" && a.got.size() == 6);
    check("the grant is for that panel: the owner's other one takes nothing from them", b.ask("control act 1 move 1 1") == "error you have no control of that panel" &&
                                                                                           (b.forge(0, proto::Pack(act(kA1, proto::kActMove, 1, 1))), a.got.size() == 6) && t.a.s->room().person(1));
    c.forge(0, proto::Pack(act(kA0, proto::kActMove, 1, 1)));
    check("...and for that person: someone else's input on it goes nowhere", a.got.size() == 6 && t.a.s->room().person(2));
    check("a key the desktop keeps for itself isn't taken even with a grant", (b.forge(0, proto::Pack(act(kA0, proto::kActKey, 125, 1))), a.got.size() == 6) && t.a.s->room().person(1));

    // one at a time
    t.tick(1.6, &a);
    ok = a.ask("control give 0 2") == "ok" && b.ask("control act 0 move 5 5") == "ok" && a.last(1, proto::kActMove, 5, 5) && a.has("\"holder\":1");
    check("two people may control one panel, and one at a time does", ok && c.ask("control act 0 move 9 9") == "ok" && a.last(1, proto::kActMove, 5, 5));
    t.tick(1.4, &a);
    check("...for as long as the first keeps at it, and a moment more", c.ask("control act 0 move 9 9") == "ok" && a.last(1, proto::kActMove, 5, 5) && a.has("\"holder\":1"));
    t.tick(0.2, &a);
    check("the desk frees a second and a half after the last input", a.has("\"holder\":-1") && c.ask("control act 0 move 9 9") == "ok" && a.last(2, proto::kActMove, 9, 9));

    // what is held down is let go
    t.tick(1.6, &a);
    ok = b.ask("control act 0 button 1 1") == "ok" && a.last(1, proto::kActButton, 1, 1);
    check("taken back while they hold a button: it is let go for them, and they hear it", ok && a.ask("control take 0 1") == "ok" && a.last(1, proto::kActButton, 1, 0) &&
                                                                                           !b.has("\"panel\":0") && b.ask("control act 0 move 1 1") == "error you have no control of that panel");
    ok = c.ask("control act 0 key 42 1") == "ok" && a.last(2, proto::kActKey, 42, 1);
    check("the wearer's own hand takes the desk from anyone", ok && a.ask("control own") == "ok" && a.last(2, proto::kActKey, 42, 0) && a.has("\"holder\":0") &&
                                                                (c.ask("control act 0 move 3 3"), a.last(2, proto::kActKey, 42, 0)));
    t.tick(1.6, &a);
    check("...for a moment", c.ask("control act 0 move 3 3") == "ok" && a.last(2, proto::kActMove, 3, 3));

    // the lease
    t.tick(1.6, &a);
    ok = a.ask("control give 0 1") == "ok" && b.ask("control act 0 key 30 1") == "ok" && a.last(1, proto::kActKey, 30, 1);
    t.tick(12, &a);
    check("a grant stands while someone says the wearer is there", ok && a.has("{\"number\":0,\"person\":1}") && b.has("\"held\":[{\"panel\":0") && c.has("\"held\":[{\"panel\":0"));
    t.tick(9.5);
    check("...and ten seconds on", a.has("{\"number\":0,\"person\":1}") && b.has("\"held\":[{\"panel\":0"));
    t.tick(1);
    check("then it is gone for everyone: what they held is let go, and they hear it", a.has("\"there\":false") && a.has("\"given\":[]") && a.last(1, proto::kActKey, 30, 0) &&
                                                                                        !b.has("\"panel\":0") && !c.has("\"panel\":0"));
    const size_t n = a.got.size();
    b.forge(0, proto::Pack(act(kA0, proto::kActMove, 1, 1)));
    check("...and has to be given again", a.got.size() == n && (a.control.Here(t.net.now), b.forge(0, proto::Pack(act(kA0, proto::kActMove, 1, 1))), a.got.size() == n));
    ok = a.ask("control give 0 1") == "ok";
    t.net.now += 10.5;   // and nobody looked meanwhile
    a.control.Here(t.net.now);
    b.forge(0, proto::Pack(act(kA0, proto::kActMove, 1, 1)));
    check("a word that the wearer is there, come after a grant ran out, doesn't bring it back", ok && a.got.size() == n && a.has("\"there\":true") && a.has("\"given\":[]"));

    // away
    ok = c.ask("control ask 0") == "ok" && a.ask("control give 0 1") == "ok" && b.has("\"held\":[{\"panel\":0") && t.a.ask("away game") == "ok";
    t.tick(0.1, &a);
    check("the wearer goes away: what they gave is taken back at once, and an ask waits", ok && a.has("\"asks\":[{\"number\":0,\"person\":2}],\"given\":[]") && !b.has("\"panel\":0"));
    check("...and nothing is given meanwhile", a.ask("control give 0 2") == "error you are away" && t.a.ask("away back") == "ok");
    check("an ask is refused", a.ask("control take 0 2") == "ok" && a.has("\"asks\":[]") && c.has("\"asked\":[]") && a.ask("control take 0 2") == "error nobody has that panel or asks for it");

    // a panel taken back
    ok = a.ask("control give 1 1") == "ok" && b.has("\"held\":[{\"panel\":1") && t.a.ask("unshare 1") == "ok";
    t.tick(0.1, &a);
    check("a panel that is no longer shared takes its grants with it", ok && a.has("\"given\":[]") && b.has("\"held\":[]"));

    // a guest's panel, over the guests' own link
    ok = c.ask("control ask 8") == "ok" && b.has("\"asks\":[{\"number\":0,\"person\":2}]") && b.ask("control here") == "ok" && b.ask("control give 0 2") == "ok";
    check("a guest's panel is its guest's to give", ok && c.ask("control act 8 move 7 8") == "ok" && b.last(2, proto::kActMove, 7, 8) && b.got.back().second.panel == kB0);
    ok = b.ask("control give 0 0") == "ok" && a.has("\"held\":[{\"panel\":8");
    t.tick(1.6, &b);
    check("...to the room's owner too, who has no more say over it than anyone", ok && a.ask("control act 8 button 1 1") == "ok" && b.last(0, proto::kActButton, 1, 1));

    // someone else in a place
    a.control.Here(t.net.now);
    ok = a.ask("control give 0 2") == "ok" && t.c.ask("leave") == "ok" && join(t.a, t.d) && t.d.s->me() == 2;
    const size_t before = a.got.size();
    t.hd.forge(0, proto::Pack(act(kA0, proto::kActMove, 1, 1)));
    check("someone who takes the place of one who left has none of their grants, however soon they came", ok && a.got.size() == before && !a.has("\"person\":2") && t.a.s->room().person(2));

    // leaving
    ok = t.b.ask("leave") == "ok";
    t.tick(0.1);
    check("the panel's owner leaves: what was held down on their desktop is let go, and nobody holds their panel", ok && b.last(0, proto::kActButton, 1, 0) && b.has("\"given\":[]") &&
                                                                                                                 a.has("\"held\":[]"));
    check("outside a room there is nothing to ask for or give", b.ask("control ask 0") == "error you aren't in a room" && b.ask("control give 0 0") == "error you aren't in a room" &&
                                                                  b.ask("control take 0") == "error you aren't in a room" && b.ask("control act 0 move 1 1") == "error you aren't in a room" &&
                                                                  b.ask("control own") == "ok");
    check("what isn't a question about control is someone else's", b.ask("state") == "not control's" && b.ask("controls") == "not control's" && b.ask("control fly 1").rfind("error control |", 0) == 0 &&
                                                                     b.ask("control act 0 fly 1 1").rfind("error control |", 0) == 0 && b.ask("control give 0").rfind("error control |", 0) == 0);
}

static void rates() {
    Table t;
    Hand &a = t.ha, &b = t.hb;
    if (!check("three people sit in a room", t.sit() && a.ask("control here") == "ok" && a.ask("control give 0 1") == "ok")) return;
    int sent = 0;
    for (int i = 0; i < 300; ++i) sent += b.ask("control act 0 move " + std::to_string(i) + " 1") == "ok";
    check("the wearer's own programs are held to less than a link takes: the rest isn't sent", sent == 200 && a.got.size() == 200 && t.a.s->room().person(1) &&
                                                                                                b.ask("control act 0 move 1 1") == "error too fast");
    t.tick(1, &a);
    check("...and a second later they go again", b.ask("control act 0 move 1 1") == "ok" && a.got.size() == 201);
    sent = 0;
    for (int i = 0; i < 6; ++i) sent += t.hc.ask("control ask 0") == "ok";
    check("asking again and again is said four times", sent == 4 && t.a.s->room().person(2) && t.hc.ask("control ask 0") == "error you asked too often");
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
    Hand &a = t.ha, &c = t.hc;
    if (!check("three people sit in a room", t.sit())) return;
    wire::Out three;
    three.U8(0), three.U8(1), three.U8(2);
    proto::AskControl ask;
    proto::Grant grant;
    ask.panel = kB0, grant.panel = kA0, grant.seconds = 10;

    check("input for a panel that isn't the reader's closes the link", thrown(t, proto::Pack(act(kB0, proto::kActMove, 1, 1))));
    check("...so does an ask for one", thrown(t, proto::Pack(ask)));
    check("a grant of a panel that isn't the sender's does too: nobody gives away another's", thrown(t, proto::Pack(grant)) && !c.has("\"panel\""));
    check("what doesn't read does", thrown(t, proto::Framed(proto::kAct, three)) && thrown(t, proto::Framed(proto::kGrant, three)) && thrown(t, proto::Framed(proto::kAskControl, three)));
    check("...as does input no panel takes, and letting go, which is never said on a link",
          thrown(t, proto::Pack(act(kA0, proto::kActMove, 70000, 1))) && thrown(t, proto::Pack(act(kA0, proto::kActLetGo, 0, 0))));
    check("a kind nobody knows is passed over", !thrown(t, proto::Framed(99, three)) && t.a.s->room().person(2));

    ask.panel = kA0;
    for (int i = 0; i < 8; ++i) c.forge(0, proto::Pack(ask));
    check("eight asks in one go are taken", t.a.s->room().person(2) && a.has("\"asks\":[{\"number\":0,\"person\":2}]"));
    check("...and a ninth closes the link", thrown(t, proto::Pack(ask)));

    bool ok = a.ask("control here") == "ok" && a.ask("control give 0 2") == "ok";
    const size_t before = a.got.size();
    for (int i = 0; i < 480; ++i) c.forge(0, proto::Pack(act(kA0, proto::kActMove, i, i)));
    check("input comes as fast as a hand gives it: 480 in one go are taken", ok && a.got.size() == before + 480 && t.a.s->room().person(2));
    check("...and more than that closes the link", thrown(t, proto::Pack(act(kA0, proto::kActMove, 1, 1))) && a.got.size() == before + 480);

    const Net::End s = t.net.stranger(t.a, "100.64.0.9");
    t.net.say(s, proto::Pack(act(kA0, proto::kActMove, 1, 1)));
    check("a stranger's input closes their link, and reaches nothing", t.net.shut.count(s.link) && a.got.size() == before + 480);

    ok = a.ask("control give 0 2") == "ok" && c.ask("control act 0 button 1 1") == "ok" && a.last(2, proto::kActButton, 1, 1);
    t.net.cut(t.a, t.c);
    t.tick(0.1, &a);
    check("a link lost with a button down: it is let go", ok && !t.a.s->room().person(2) && a.last(2, proto::kActButton, 1, 0) && a.has("\"given\":[]") && a.has("\"holder\":-1"));

    ok = t.a.ask("leave") == "ok";
    t.tick(0.1);
    check("the room ends: nothing of it stands anywhere", ok && a.has("\"asks\":[],\"given\":[],\"holder\":-1,\"held\":[],\"asked\":[]") && t.hb.has("\"held\":[],\"asked\":[]"));
}

// An act's gate (local/controlask.h): what it lets through, in the order it says, is what the
// panel's owner gets.
static void gate() {
    Table t;
    Hand &a = t.ha, &b = t.hb;
    if (!check("someone is given a panel", t.sit() && b.ask("control ask 0") == "ok" && a.ask("control here") == "ok" && a.ask("control give 0 1") == "ok")) return;
    std::vector<proto::Act> held;
    const local::ActGate moves_wait = [&held](const proto::Act &act, std::vector<proto::Act> &go) {
        if (act.what == proto::kActMove) return held.push_back(act);
        go = held, go.push_back(act), held.clear();
    };
    const auto said = [&](const std::string &text) {
        std::string reply = "not control's";
        local::ControlAsk(b.control, text, t.net.now, reply, moves_wait);
        t.net.pump();
        return reply;
    };
    check("an act the gate keeps is taken, and nothing is given to the owner for it", said("control act 0 move 100 200") == "ok" && said("control act 0 move 300 400") == "ok" && a.got.empty());
    bool ok = said("control act 0 button 1 1") == "ok" && a.got.size() == 3 && a.got[0].second.what == proto::kActMove && a.got[0].second.a == 100 && a.got[1].second.a == 300;
    check("what the gate lets go for the next one reaches the owner, in the gate's order", ok && a.last(1, proto::kActButton, 1, 1));
    check("the answer is the last one's: what no panel takes is still refused", said("control act 0 move 1 1") == "ok" && said("control act 0 button 9 1") == "error that isn't input a panel takes");
    check("...and so is an act on a panel that wasn't given, whatever the gate does", said("control act 8 button 1 1") == "error that panel is yours" && said("control act 1 button 1 1") == "error you have no control of that panel");
    check("with no gate an act goes as it was said", b.ask("control act 0 move 7 8") == "ok" && a.last(1, proto::kActMove, 7, 8));
}

int main() {
    granting();
    rates();
    rules();
    gate();
    std::printf(failed ? "FAILED\n" : "all ok\n");
    return failed ? 1 : 0;
}
