// A Frametop Together session's rules and its commands (together/session), several sessions run against
// each other on a network made in the test (fakenet.h).
// Invites, asking and being let in or not, a full room, the guests' own links, leaving, a lost
// owner, another version, the panels people share and who watches them, the avatars they have
// on, and people who say what they may not. No sockets, no clock.
#include "../room/panels.h"
#include "fakenet.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static std::vector<uint8_t> hello(uint16_t version, const std::string &code, const uint8_t *room = nullptr) {
    proto::Hello h;
    h.version = version, h.code = code, h.name = "stranger", h.port = 47193;
    if (room) std::memcpy(h.room, room, proto::kRoomId);
    return proto::Pack(h);
}

static bool idle(Frame &f) { return f.s->mode() == session::Mode::kIdle; }

static void joining() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo");
    check("no invite without a room, and no room twice", a.ask("invite").rfind("error", 0) == 0 && a.ask("open 1 2 0.5") == "ok" && a.ask("open").rfind("error", 0) == 0);
    const std::string code = a.ask("invite").substr(3);
    bool ok = code.size() == 8 && a.has("\"mode\":\"hosting\"") && a.has("\"codes\":1");
    check("an owner opens a room and gets a code that reads aloud", ok && code.find_first_of("01OIL") == std::string::npos);

    ok = b.ask("join 100.64.0.1 47193 WRONGONE") == "ok" && idle(b) && b.has("isn't good") && !a.asking();
    check("a wrong code is refused, and its bearer told why", ok);
    std::string spaced = code.substr(0, 4) + "-" + code.substr(4);
    for (char &c : spaced) c = char(std::tolower(c));
    ok = b.ask("join frame-Ann@example.com 47193 " + spaced) == "ok" && b.s->mode() == session::Mode::kCalling;
    ok = ok && a.has("\"asks\":[{\"id\":1,\"name\":\"Bo\",\"login\":\"Bo@example.com\",\"address\":\"100.64.0.2\"}]") && a.has("\"codes\":0");
    check("a good one, typed small and with a dash, asks the owner: they see who Tailscale says it is", ok && a.s->room().people.size() == 1);
    ok = a.ask("accept 7").rfind("error", 0) == 0 && a.ask("accept 1") == "ok" && b.s->mode() == session::Mode::kJoined && b.s->me() == 1;
    ok = ok && a.s->room().people.size() == 2 && b.s->room().people.size() == 2 && b.has("\"note\":\"You are in Ann's room.\"") && a.has("Bo came in.");
    const proto::Person *bo = b.s->room().person(1);
    ok = ok && bo && bo->login == "Bo@example.com" && bo->address == "100.64.0.2" && bo->port == 47193 && a.s->MemberLinks().size() == 1 && b.s->MemberLinks().size() == 1;
    check("accepted, they are in the room, and both ends know it", ok && a.has("\"linked\":true") && !a.has("\"linked\":false"));
    // the owner stood at (1, 0, 2) turned half a radian: the first guest's spot is 0.9 m to their right
    ok = bo && std::fabs(bo->spot.at[0] - (1 + 0.9 * std::cos(0.5))) < 1e-5 && std::fabs(bo->spot.at[2] - (2 - 0.9 * std::sin(0.5))) < 1e-5 && bo->spot.yaw == 0.5f;
    check("the guest's spot is beside the owner, facing the way the owner faced", ok);

    Frame c(net, "100.64.0.3", 47193, "Cy");
    ok = c.ask("join 100.64.0.1 47193 " + code) == "ok" && idle(c) && c.has("Not let in");
    check("a code works once", ok);
    const std::string second = a.ask("invite").substr(3);
    ok = c.ask("join 100.64.0.1 47193 " + second) == "ok" && a.ask("decline 2") == "ok" && idle(c) && c.has("said no") && a.s->room().people.size() == 2;
    check("the owner can say no", ok);
    const std::string third = a.ask("invite").substr(3);
    net.now += 601;
    ok = c.ask("join 100.64.0.1 47193 " + third) == "ok" && idle(c) && !a.asking();
    check("a code is good for ten minutes", ok);
    const std::string fourth = a.ask("invite").substr(3);
    ok = c.ask("join 100.64.0.1 47193 " + fourth) == "ok" && c.s->mode() == session::Mode::kCalling && a.asking();
    net.wait(session::Session::kAskWaitS + 2);
    check("an ask the owner doesn't answer ends by itself", ok && idle(c) && c.has("didn't answer") && !a.asking());
    ok = c.ask("join 100.64.0.1 47193 " + a.ask("invite").substr(3)) == "ok" && c.ask("leave") == "ok" && idle(c) && c.has("stopped calling") && !a.asking();
    check("...and so does one whose asker gives up", ok);

    ok = b.ask("away off") == "ok" && a.s->room().person(1)->away == proto::kAwayOff && b.s->room().person(1)->away == proto::kAwayOff;
    ok = ok && a.ask("away game") == "ok" && b.s->room().person(0)->away == proto::kAwayGame && b.ask("away back") == "ok" && a.s->room().person(1)->away == 0;
    check("away and back reach everyone", ok && a.ask("away asleep").rfind("error", 0) == 0);
    ok = c.ask("away off") == "ok" && idle(c) && c.has("\"away\":2") && join(a, c) && a.s->room().person(2) && a.s->room().person(2)->away == proto::kAwayOff;
    ok = ok && b.s->room().person(2)->away == proto::kAwayOff && c.s->room().person(2)->away == proto::kAwayOff;
    ok = ok && c.ask("away back") == "ok" && a.s->room().person(2)->away == 0 && c.has("\"away\":0") && c.ask("leave") == "ok" && idle(c);
    check("someone away before they come in is away in the room from the start", ok);

    ok = b.ask("leave") == "ok" && idle(b) && a.s->room().people.size() == 1 && a.has("Bo left.") && a.s->MemberLinks().empty();
    check("a guest leaves, and the room goes on without them", ok);
    ok = join(a, b) && a.ask("leave") == "ok" && idle(a) && idle(b) && b.has("its owner closed it");
    check("the owner leaves, and the room ends for everyone", ok && net.far.empty());
    ok = a.ask("away game") == "ok" && a.ask("open") == "ok" && a.s->room().person(0)->away == proto::kAwayGame && a.ask("leave") == "ok" && a.has("\"away\":1");
    check("...and so is an owner who opens a room, in the next room too", ok && a.ask("away back") == "ok" && idle(a));
    ok = b.ask("join 100.64.0.1 47193 " + fourth) == "ok" && idle(b) && b.has("No room is open");
    check("nobody joins a headset with no room open", ok);

    ok = b.ask("join 100.64.0.9 47193 ABCDEFGH") == "ok" && idle(b) && b.has("Couldn't reach the room.");
    ok = ok && b.ask("join 100.64.0.1 0 X").rfind("error", 0) == 0 && b.ask("join").rfind("error", 0) == 0 && b.ask("open 1 2").rfind("error", 0) == 0 && b.ask("open 1 2 nan").rfind("error", 0) == 0;
    check("a call that reaches nobody says so, and commands that make no sense are errors", ok && b.ask("leave").rfind("error", 0) == 0 && b.ask("").rfind("error", 0) == 0);
    a.onTailnet = false;
    check("no room without Tailscale", a.ask("open").rfind("error Tailscale", 0) == 0);
}

static void mesh() {
    for (size_t piece : {size_t(0), size_t(1), size_t(13)}) {
        Net net;
        net.piece = piece;
        Frame a(net, "100.64.0.4", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy"), d(net, "100.64.0.1", 47193, "Di"), e(net, "100.64.0.5", 47193, "Eve");
        bool ok = a.ask("open") == "ok" && join(a, b) && join(a, c) && join(a, d);
        for (Frame *f : {&a, &b, &c, &d}) ok = ok && f->s->room().people.size() == 4 && f->s->MemberLinks().size() == 3 && !f->has("\"linked\":false");
        check(piece ? "...with the bytes coming in pieces" : "four people: everyone has a link to everyone, and the same room", ok && net.far.size() == 12);
        ok = d.called.size() == 3 && b.called == std::vector<std::string>{"100.64.0.4", "100.64.0.3"} && c.called.size() == 1;
        check("each two guests have one link, called by the one with the lower address", ok);
        const std::string invite = a.ask("invite").substr(3);
        ok = e.ask("join 100.64.0.4 47193 " + invite) == "ok" && idle(e) && e.has("The room is full.") && a.has("\"codes\":1");
        check("a fifth is told the room is full, and their code is still good", ok);

        ok = c.ask("leave") == "ok";
        for (Frame *f : {&a, &b, &d}) ok = ok && f->s->room().people.size() == 3 && !f->s->room().person(2) && f->s->MemberLinks().size() == 2;
        check("one leaves: the others' rooms and links follow", ok && net.far.size() == 6);
        ok = e.ask("join 100.64.0.4 47193 " + invite) == "ok" && a.ask("accept " + std::to_string(a.asking())) == "ok" && e.s->me() == 2;
        for (Frame *f : {&a, &b, &d, &e}) ok = ok && f->s->MemberLinks().size() == 3;
        check("their place goes to the next one in, who links up with everyone at once", ok);

        net.cut(b, d);
        ok = b.s->MemberLinks().size() == 2 && d.s->MemberLinks().size() == 2 && b.has("\"linked\":false");
        net.wait(session::Session::kMeshRetryS + 1);
        check("a lost link between two guests is called again", ok && b.s->MemberLinks().size() == 3 && d.s->MemberLinks().size() == 3);
        net.cut(a, d);
        ok = idle(d) && d.has("link to its owner was lost") && a.s->room().people.size() == 3 && b.s->room().people.size() == 3;
        net.wait(5);
        check("a guest who loses the owner is out, and the room goes on", ok && b.s->MemberLinks().size() == 2 && d.s->MemberLinks().empty() && net.far.size() == 6);
        net.cut(a, b), net.cut(a, e);
        check("the owner alone again", a.s->room().people.size() == 1 && idle(b) && idle(e) && net.far.empty());
    }
}

static void versions() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo");
    bool ok = a.ask("open") == "ok";
    const std::string code = a.ask("invite").substr(3);
    // Bo's hello says the next version on its way to Ann
    net.tamper = [&](Net::Event &e) {
        if (e.to.frame == &a && e.what == Net::Event::kBytes) e.bytes = hello(proto::kVersion + 1, code);
    };
    ok = ok && b.ask("join " + where(a) + " " + code) == "ok" && idle(b) && b.has("another version") && !a.asking() && a.has("\"codes\":1");
    check("a caller of another version is told to update, and their code is still good", ok);
    // ...and now Ann's does on its way to Bo
    net.tamper = [&](Net::Event &e) {
        if (e.to.frame == &b && e.what == Net::Event::kBytes) e.bytes = hello(proto::kVersion + 1, "");
    };
    ok = b.ask("join " + where(a) + " " + code) == "ok" && idle(b) && b.has("another version") && !a.asking();
    check("a room of another version: the caller hangs up, and knows why", ok);
}

static void strangers() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    Frame mal(net, "100.64.0.66", 47193, "Mal");
    const std::string from = mal.address;
    bool ok = a.ask("open") == "ok" && join(a, b);

    Net::End s = net.stranger(a, from);
    net.say(s, hello(proto::kVersion + 1, ""));
    check("another version is refused at hello", ok && net.refused(s) == int(proto::Why::kVersion) && net.shut.count(s.link));

    mal.onTailnet = false;
    const std::string code = a.ask("invite").substr(3);
    s = net.stranger(a, from);
    net.say(s, hello(proto::kVersion, code));
    check("someone Tailscale doesn't know is refused, good code or not", net.refused(s) == int(proto::Why::kBad) && !a.asking() && a.has("\"codes\":1"));
    mal.onTailnet = true;

    ok = true;
    for (int i = 0; i < 6; ++i) {   // the sixth has the right code
        s = net.stranger(a, from);
        net.say(s, hello(proto::kVersion, i < 5 ? "GUESS" + std::to_string(i) : code));
        ok = ok && net.refused(s) == int(proto::Why::kCode) && !a.asking() && a.has("\"codes\":1");
    }
    net.say(net.stranger(a, c.address), hello(proto::kVersion, code));
    check("five wrong guesses from one address, and it is refused unheard; others are not", ok && a.asking() && a.has("\"codes\":0"));
    a.ask("decline " + std::to_string(a.asking()));
    net.now += room::Invites::kWrongForS + 1;

    // things a stranger may not say
    ok = true;
    proto::Welcome w;
    w.you = 1, w.room = a.s->room();
    for (const std::vector<uint8_t> &bad : {proto::Pack(a.s->room()), proto::Pack(w), proto::PackLeave(), proto::Pack(proto::Away{}), proto::Pack(proto::Refuse{}), std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8},
                                            std::vector<uint8_t>{0, 0, 0, 0, 99, 99}}) {
        s = net.stranger(a, from);
        net.say(s, bad);
        ok = ok && net.shut.count(s.link);
        net.say(s, hello(proto::kVersion, a.ask("invite").substr(3)));   // too late: the link is gone
        ok = ok && !a.asking() && a.s->room().people.size() == 2;
    }
    check("a stranger who says anything but hello is dropped", ok);
    s = net.stranger(a, from);
    net.say(s, hello(proto::kVersion, a.ask("invite").substr(3)));
    ok = a.asking();
    net.say(s, hello(proto::kVersion, ""));
    check("...and so is one who says hello twice", ok && !a.asking() && net.shut.count(s.link));

    std::vector<Net::End> silent;
    for (int i = 0; i < 20; ++i) silent.push_back(net.stranger(a, from));
    size_t kept = 0;
    for (const Net::End &e : silent) kept += !net.shut.count(e.link);
    ok = kept == session::Session::kMostStrangers;
    net.wait(session::Session::kHelloWaitS + 2);
    for (const Net::End &e : silent) ok = ok && net.shut.count(e.link);
    s = net.stranger(a, from);
    net.say(s, hello(proto::kVersion, a.ask("invite").substr(3)));
    check("links that never say who they are: eight at once, closed in ten seconds, and others get in after", ok && a.asking());
    a.ask("decline " + std::to_string(a.asking()));

    // a guest who says what only an owner may
    ok = join(a, c) && a.s->room().people.size() == 3 && c.s->MemberLinks().size() == 2;
    proto::Room forged = a.s->room();
    forged.turn += 5, forged.people.erase(forged.people.begin() + 1);   // Bo taken out
    for (int link : c.s->MemberLinks()) c.Send(link, proto::Pack(forged));
    net.pump();
    ok = ok && a.s->room().people.size() == 2 && !a.s->room().person(2) && b.s->room().people.size() == 2 && b.s->room().person(1);
    check("a guest who sends the room is dropped by everyone, and the owner's room stands", ok && idle(c) && b.s->MemberLinks().size() == 1);
    ok = join(a, c);
    for (int link : b.s->MemberLinks())
        if (b.s->PersonOn(link) == 2) {   // Bo welcomes Cy to a room of Bo's making
            w.you = 2, w.room = forged;
            b.Send(link, proto::Pack(w));
            b.Send(link, proto::Pack(proto::Away{}));
        }
    net.pump();
    net.wait(session::Session::kMeshRetryS + 1);
    check("...and a guest listens to the owner only: another guest's welcome ends that link, not the room", ok && c.s->mode() == session::Mode::kJoined && c.s->room().people.size() == 3 && c.s->MemberLinks().size() == 2);

    // a guest's own door
    s = net.stranger(c, from);
    net.say(s, hello(proto::kVersion, "", a.s->room().room));
    ok = c.s->PersonOn(s.link) < 0 && !net.refused(s) && c.s->MemberLinks().size() == 2;
    Net::End twice = net.stranger(c, b.address);   // Bo's address, when Bo is linked already
    net.say(twice, hello(proto::kVersion, "", a.s->room().room));
    ok = ok && c.s->PersonOn(twice.link) < 0;
    net.wait(session::Session::kHelloWaitS + 2);
    ok = ok && net.shut.count(s.link) && net.shut.count(twice.link);
    s = net.stranger(b, c.address);
    uint8_t other[proto::kRoomId] = {1, 2, 3};
    net.say(s, hello(proto::kVersion, "", other));
    ok = ok && net.refused(s) == int(proto::Why::kNotInRoom) && b.s->MemberLinks().size() == 2;
    s = net.stranger(mal, c.address);
    net.say(s, hello(proto::kVersion, "ABCDEFGH"));
    check("a guest lets in only who the owner said is in the room, once, and only for this room", ok && net.refused(s) == int(proto::Why::kNotOpen));

    // an owner who names an address off the tailnet: nobody calls it
    proto::Room lie = a.s->room();
    ++lie.turn;
    proto::Person ghost;
    ghost.id = 3, ghost.name = "ghost", ghost.address = "192.168.1.1", ghost.port = 22;
    lie.people.push_back(ghost);
    const size_t before = b.called.size() + c.called.size();
    for (int link : a.s->MemberLinks()) a.Send(link, proto::Pack(lie));
    net.pump();
    net.wait(8);
    check("an address off the tailnet is never called, whoever names it", b.s->room().people.size() == 4 && c.s->room().people.size() == 4 && b.called.size() + c.called.size() == before);
    // ...and an older room, or one that isn't this room, changes nothing
    proto::Room old = a.s->room();
    for (int link : a.s->MemberLinks()) a.Send(link, proto::Pack(old));
    net.pump();
    ok = b.s->room().people.size() == 4 && b.s->mode() == session::Mode::kJoined;
    old.turn = lie.turn + 9, old.room[3] ^= 1;
    for (int link : a.s->MemberLinks()) a.Send(link, proto::Pack(old));
    net.pump();
    check("an older room is passed over, and another room's ends the link", ok && idle(b) && idle(c));
}

// Someone the owner hasn't told us of yet may knock first: they wait at the door.
static void knocks() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    bool ok = a.ask("open") == "ok" && join(a, b);
    const Net::End early = net.stranger(b, c.address);
    net.say(early, hello(proto::kVersion, "", a.s->room().room));
    ok = ok && b.s->PersonOn(early.link) < 0 && !net.shut.count(early.link);
    ok = ok && join(a, c) && b.s->PersonOn(early.link) == 2 && !net.heard[early.link].empty();
    check("a guest who knocks before the owner's word of them waits, and is let in when it comes", ok && b.called.size() == 1);
}

static proto::Panel panel(int owner, int number, float x = 0) {
    proto::Panel p;
    p.id = proto::PanelId(owner, number), p.width = 1920, p.height = 1080, p.at[0] = x, p.at[1] = 1.4f, p.metres = 1.2f;
    return p;
}

static void panels() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    const std::string screen = "share 0 2560 1440 0 1.4 -1.5 1 0 0 0 1.6 1.8 Screen 1";
    bool ok = a.ask(screen).rfind("error you aren't", 0) == 0 && a.ask("unshare 0").rfind("error", 0) == 0 && a.ask("open") == "ok" && join(a, b) && join(a, c);
    check("nobody shares a panel outside a room", ok);
    ok = a.ask(screen) == "ok" && a.has("\"panels\":[{\"id\":0,\"owner\":0,\"number\":0,\"name\":\"Screen 1\",\"size\":[2560,1440],\"pictures\":30,\"at\":[0,1.4,-1.5],\"turn\":[1,0,0,0],\"metres\":1.6,\"curve\":1.8}]");
    for (Frame *f : {&b, &c}) ok = ok && f->s->room().panels.size() == 1 && f->s->room().panel(0) && f->s->room().panel(0)->name == "Screen 1" && f->s->room().panel(0)->width == 2560;
    check("the owner shares a screen, and it is in everyone's room where the owner put it", ok);
    ok = b.ask("share 1 1920 1080 0.5 1.2 -1 0.7071068 0 0.7071068 0 1.2 0 Bo's window") == "ok";
    for (Frame *f : {&a, &b, &c}) {
        const proto::Panel *p = f->s->room().panel(9);
        ok = ok && f->s->room().panels.size() == 2 && p && p->owner() == 1 && p->name == "Bo's window" && p->at[0] == 0.5f && std::fabs(p->turn[2] - 0.7071068f) < 1e-4;
    }
    check("a guest shares a window through the owner: its id says whose it is", ok);

    size_t toOwner = 0;
    net.tamper = [&](Net::Event &e) { toOwner += e.to.frame == &a && e.what == Net::Event::kBytes; };
    uint32_t turn = a.s->room().turn;
    ok = b.ask("share 1 1920 1080 0.5 1.2 -1 0.7071068 0 0.7071068 0 1.2 0 Bo's window") == "ok" && toOwner == 0 && a.ask(screen) == "ok" && a.s->room().turn == turn;
    check("a panel said again as it is tells nobody anything", ok);
    ok = b.ask("share 1 1920 1080 0.75 1.2 -1 0.7071068 0 0.7071068 0 1.2 0 Bo's window") == "ok" && toOwner == 1 && a.s->room().turn == turn + 1;
    for (Frame *f : {&a, &b, &c}) ok = ok && f->s->room().panel(9)->at[0] == 0.75f && f->s->room().panels.size() == 2;
    check("moved, it moves in everyone's room", ok);
    net.tamper = nullptr;

    ok = true;
    for (const char *bad : {"share 7 1920 1080 0 0 0 1 0 0 0 1 0", "share -1 1920 1080 0 0 0 1 0 0 0 1 0", "share 0 100 1080 0 0 0 1 0 0 0 1 0", "share 0 1921 1080 0 0 0 1 0 0 0 1 0",
                            "share 0 1920 1080 0 0 0 0 0 0 0 1 0", "share 0 1920 1080 500 0 0 1 0 0 0 1 0", "share 0 1920 1080 0 0 0 1 0 0 0 0.01 0", "share 0 1920 1080 0 0 0 1 0 0 0 1 0.2",
                            "share 0 1920 1080 0 0 0 1 0 0 0 1", "share 0 1920 1080 0 nan 0 1 0 0 0 1 0", "share 0.5 1920 1080 0 0 0 1 0 0 0 1 0", "share", "unshare", "unshare x", "unshare 7"})
        ok = ok && c.ask(bad).rfind("error", 0) == 0;
    check("an eighth panel, a size no stream has, a turn that isn't one, a place a mile off: errors", ok && c.s->room().panels.size() == 2 && a.s->room().panels.size() == 2);
    ok = c.ask("share 2 1280 720 0 1 -1 1 0 0 0 1 0 A\x1b[31mname") == "ok" && a.s->room().panel(18) && a.s->room().panel(18)->name == "A[31mname" && c.ask("unshare 2") == "ok";
    for (Frame *f : {&a, &b, &c}) ok = ok && f->s->room().panels.size() == 2 && !f->s->room().panel(18);
    check("a name is cleaned on its way out, and a panel taken back is gone from every room", ok && c.ask("unshare 2") == "ok" && c.ask("unshare 0") == "ok");

    // who watches what
    proto::Watch w;
    w.panel = 0, w.level = proto::kInView, w.fresh = true;
    ok = c.s->Watch(w).empty();
    net.pump();
    ok = ok && a.watches.size() == 1 && a.s->PersonOn(a.watches[0].first) == 2 && a.watches[0].second.panel == 0 && a.watches[0].second.fresh && b.watches.empty();
    w.panel = 9, w.level = proto::kFocused, w.fresh = false;
    ok = ok && c.s->Watch(w).empty() && a.s->Watch(w).empty();
    net.pump();
    ok = ok && b.watches.size() == 2 && b.s->PersonOn(b.watches[0].first) == 2 && b.s->PersonOn(b.watches[1].first) == 0 && b.watches[0].second.level == proto::kFocused && a.watches.size() == 1;
    check("what a viewer has of a panel goes to the panel's owner, owner of the room or not, on the viewer's own link", ok);
    ok = !b.s->Watch(w).empty();
    w.panel = 1;
    ok = ok && !c.s->Watch(w).empty();
    net.cut(b, c);
    w.panel = 9;
    ok = ok && !c.s->Watch(w).empty();
    net.wait(session::Session::kMeshRetryS + 1);
    check("nobody watches their own panel, one there isn't, or one whose owner they have no link to", ok && c.s->Watch(w).empty());

    // what a guest may not say about panels
    proto::Unpanel u;
    u.panel = 0;
    ok = a.ask("unshare 0") == "ok" && a.ask(screen) == "ok";
    w.panel = 1, w.level = proto::kInView;   // a panel of Ann's that isn't shared: no harm, and nothing told
    const size_t told = a.watches.size();
    c.Send(c.s->MemberLinks()[0], proto::Pack(w));
    net.pump();
    check("a word about a panel that just went is passed over", ok && a.watches.size() == told && c.s->mode() == session::Mode::kJoined && a.s->room().people.size() == 3);
    const auto owners = [&](Frame &who) {
        for (int link : who.s->MemberLinks())
            if (who.s->PersonOn(link) == 0) return link;
        return -1;
    };
    c.Send(owners(c), proto::Pack(panel(1, 0, 9)));   // Cy shares a panel as Bo's
    net.pump();
    ok = idle(c) && a.s->room().people.size() == 2 && !a.s->room().panel(8) && a.s->room().panel(9)->at[0] == 0.75f;
    check("a guest who shares a panel as someone else's is dropped, and the room stands", ok);
    ok = join(a, c);
    c.Send(owners(c), proto::Pack(panel(2, room::kOwnPanels)));
    net.pump();
    ok = ok && idle(c) && join(a, c);
    u.panel = 9;
    c.Send(owners(c), proto::Pack(u));   // ...takes Bo's back
    net.pump();
    ok = ok && idle(c) && a.s->room().panel(9) && join(a, c);
    w.panel = 9;
    c.Send(owners(c), proto::Pack(w));   // ...tells Ann of watching Bo's
    net.pump();
    check("...so is one who shares past the seven a person may, takes another's panel back, or tells the wrong Frame what they watch", ok && idle(c) && a.s->room().panels.size() == 2 && join(a, c));
    for (int link : c.s->MemberLinks())
        if (c.s->PersonOn(link) == 1) c.Send(link, proto::Pack(panel(2, 0)));   // a panel is the owner's to take, not another guest's
    net.pump();
    ok = c.s->mode() == session::Mode::kJoined && b.s->MemberLinks().size() == 1 && b.s->room().panels.size() == 2;
    net.wait(session::Session::kMeshRetryS + 1);
    check("a guest takes no panel from another guest: that link ends, not the room", ok && b.s->MemberLinks().size() == 2);

    // a flood
    for (int i = 0; i < 200 && c.s->mode() == session::Mode::kJoined; ++i) c.ask("share 0 1280 720 " + std::to_string(i * 0.01) + " 1 -1 1 0 0 0 1 0");
    ok = idle(c) && a.s->room().people.size() == 2 && a.s->room().panels.size() == 2 && a.s->room().turn < turn + 80;
    check("a guest who moves a panel faster than anyone could is dropped, and their panel with them", ok);
    ok = join(a, c);
    for (int i = 0; i < 12 && ok; ++i) {   // a hand dragging a panel, four times a second
        net.now += 0.25;
        ok = c.ask("share 0 1280 720 " + std::to_string(i * 0.01) + " 1 -1 1 0 0 0 1 0") == "ok" && c.s->mode() == session::Mode::kJoined;
    }
    check("...and one who drags it by hand is not", ok && a.s->room().panels.size() == 3);

    // leaving
    ok = c.ask("leave") == "ok" && a.s->room().panels.size() == 2 && b.s->room().panels.size() == 2 && !b.s->room().panel(16);
    net.cut(a, b);
    check("someone's panels leave the room with them, however they go", ok && a.s->room().panels.size() == 1 && a.s->room().panel(0) && idle(b));
    ok = join(a, b) && b.s->room().panels.size() == 1 && b.ask("share 1 1920 1080 0 1 -1 1 0 0 0 1 0") == "ok" && a.s->room().panels.size() == 2;
    check("back in a room, a guest shares afresh", ok && a.ask("leave") == "ok" && idle(b) && b.s->room().panels.empty());
}

// Noise on every kind of link: nothing crashes (the sanitizers watch), and the owner's room
// only ever holds the people it let in.
static proto::Look look(uint8_t seed, uint32_t size) {
    proto::Look l;
    l.size = size;
    for (size_t k = 0; k < proto::kLookHash; ++k) l.hash[k] = uint8_t(seed + k);
    return l;
}

// The avatars people have on: the room says whose is which, and a file is asked of its wearer.
static void looks() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
    const proto::Look fox = look(1, 3u << 20), owl = look(90, 5000), cat = look(170, 70000);
    const auto has = [](Frame &f, int person, const proto::Look &l) { return f.s->room().person(person) && proto::SameLook(f.s->room().person(person)->look, l); };
    bool ok = a.s->Wear(fox, net.now).empty() && b.s->Wear(owl, net.now).empty() && idle(a) && proto::SameLook(a.s->look(), fox);
    ok = ok && a.ask("open") == "ok" && has(a, 0, fox) && join(a, b) && has(a, 0, fox) && has(b, 0, fox) && has(a, 1, owl) && has(b, 1, owl);
    check("what someone had on before a room is in it from the start: an owner's, and a guest's", ok);
    net.wait(session::Session::kLookEveryS + 1);
    ok = join(a, c) && has(c, 0, fox) && has(c, 1, owl) && has(c, 2, proto::Look{}) && c.s->MemberLinks().size() == 2;
    check("someone with no avatar to send has none in the room, and sees the others'", ok);
    net.now += 10;
    ok = c.s->Wear(cat, net.now).empty();
    net.pump();
    ok = ok && has(a, 2, cat) && has(b, 2, cat) && has(c, 2, cat) && a.s->Wear(owl, net.now).empty();
    net.pump();
    check("a change reaches everyone: a guest's through the owner, an owner's at once", ok && has(b, 0, owl) && has(c, 0, owl));
    const uint32_t turn = a.s->room().turn;
    ok = c.s->Wear(cat, net.now).empty() && a.s->Wear(owl, net.now).empty();
    net.wait(5);
    check("...and what didn't change isn't said again", ok && a.s->room().turn == turn);
    ok = b.s->Wear(proto::Look{}, net.now + 1).empty();
    net.pump();
    proto::Look big = fox;
    big.size = proto::kMostLook + 1;
    check("an avatar taken off is off for everyone, and one too large to send isn't put on", ok && has(a, 1, proto::Look{}) && has(c, 1, proto::Look{}) && !b.s->Wear(big, net.now).empty() && b.s->look().none());

    // a guest's word that the owner's room doesn't have yet is said again, not flooded
    net.now += 10;
    net.tamper = [](Net::Event &e) {
        proto::Parser parser;
        uint16_t kind = 0;
        std::vector<uint8_t> body;
        if (e.what == Net::Event::kBytes && parser.Feed(e.bytes.data(), e.bytes.size()) && parser.Next(kind, body) && kind == proto::kLook) e.bytes.clear();
    };
    ok = b.s->Wear(fox, net.now).empty();
    net.pump();
    ok = ok && has(a, 1, proto::Look{});
    net.tamper = nullptr;
    net.wait(session::Session::kLookEveryS + 1);
    check("a guest whose word got lost says it again until the room has it", ok && has(a, 1, fox) && has(c, 1, fox));
    for (int i = 0; i < 40; ++i) b.s->Wear(i % 2 ? owl : cat, net.now), net.pump();
    net.wait(session::Session::kLookEveryS + 1);
    check("someone who changes avatars all the time stays in the room: their own end holds the words back", b.s->mode() == session::Mode::kJoined && has(a, 1, owl) && a.s->room().people.size() == 3);

    // asking for a file
    ok = b.s->Want(0, owl).empty() && c.s->Want(1, owl).empty() && a.s->Want(2, cat).empty();
    net.pump();
    ok = ok && a.wants.size() == 1 && a.s->PersonOn(a.wants[0].first) == 1 && !std::memcmp(a.wants[0].second.hash, owl.hash, proto::kLookHash);
    ok = ok && b.wants.size() == 1 && b.s->PersonOn(b.wants[0].first) == 2 && c.wants.size() == 1 && c.s->PersonOn(c.wants[0].first) == 0;
    check("a file is asked of the one who has it on, on the link to them: guest to owner, guest to guest, owner to guest", ok);
    Frame d(net, "100.64.0.4", 47193, "Di");
    check("...not of oneself, of nobody, or by someone in no room", !b.s->Want(1, owl).empty() && !b.s->Want(3, owl).empty() && !d.s->Want(0, owl).empty() && !d.s->Want(-1, owl).empty());

    // what may not be said
    for (int link : b.s->MemberLinks())
        if (b.s->PersonOn(link) == 2) b.Send(link, proto::Pack(fox));   // to another guest: only the owner keeps the room
    net.pump();
    ok = c.s->mode() == session::Mode::kJoined && b.s->mode() == session::Mode::kJoined && has(c, 1, owl);
    net.wait(session::Session::kMeshRetryS + 1);
    check("a guest who tells another guest what it has on loses that link, not the room", ok && b.s->MemberLinks().size() == 2);
    net.now += 60;
    size_t before = a.wants.size();
    for (int i = 0; i < 20; ++i)
        for (int link : b.s->MemberLinks())
            if (b.s->PersonOn(link) == 0) b.Send(link, proto::Pack(proto::Want{}));
    net.pump();
    ok = a.wants.size() - before == size_t(session::Session::kLookWordsAtOnce) && a.s->room().people.size() == 2 && !a.s->room().person(1);
    check("a flood of words about avatars: a few are heard, then its sender is out of the room", ok && idle(b));
    Net::End s = net.stranger(a, "100.64.0.66");
    net.say(s, proto::Pack(fox));
    before = a.wants.size();
    Net::End s2 = net.stranger(a, "100.64.0.66");
    net.say(s2, proto::Pack(proto::Want{}));
    check("a stranger's words about avatars are a stranger's: dropped, unheard", net.shut.count(s.link) && net.shut.count(s2.link) && a.wants.size() == before);
    ok = a.ask("leave") == "ok" && idle(c) && proto::SameLook(c.s->look(), cat) && c.ask("open") == "ok" && has(c, 0, cat);
    check("after a room, the next one has what its wearer still has on", ok);
}

static void noise() {
    bool ok = true;
    for (int round = 0; round < 300; ++round) {
        Net net;
        net.piece = round % 3 ? 0 : 1 + round % 7;
        Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
        ok = ok && a.ask("open") == "ok" && join(a, b) && join(a, c) && a.ask("share 0 1280 720 0 1 -1 1 0 0 0 1 0") == "ok" && b.ask("share 2 1280 720 0 1 -1 1 0 0 0 1 0") == "ok";
        const std::string code = a.ask("invite").substr(3);
        for (int i = 0; i < 40; ++i) {
            std::vector<uint8_t> bytes;
            const uint64_t kind = roll() % 4;
            if (kind == 0) {
                bytes.resize(1 + roll() % 60);
                for (uint8_t &x : bytes) x = uint8_t(roll());
            } else {
                // a real message, whole or damaged, of a kind that may not be its sender's to send
                proto::Welcome w;
                w.you = 1, w.room = a.s->room();
                proto::Watch watch;
                watch.panel = uint8_t(roll() % 34), watch.level = uint8_t(roll() % 4);
                proto::Unpanel unpanel;
                unpanel.panel = uint8_t(roll() % 34);
                const std::vector<uint8_t> real[9] = {proto::Pack(a.s->room()), proto::Pack(w), hello(proto::kVersion, code, a.s->room().room), proto::PackLeave(), proto::Pack(proto::Away{}), proto::Pack(proto::Refuse{}),
                                                      proto::Pack(panel(int(roll() % 4), int(roll() % 8))), proto::Pack(unpanel), proto::Pack(watch)};
                bytes = real[roll() % 9];
                if (kind == 2) bytes[roll() % bytes.size()] = uint8_t(roll());
                if (kind == 3) bytes.resize(1 + roll() % bytes.size());
            }
            Frame *sender = roll() % 2 ? &b : &c;
            const std::vector<int> links = sender->s->MemberLinks();
            if (roll() % 3 == 0) net.say(net.stranger(roll() % 2 ? a : b, "100.64.0.66"), bytes);
            else if (!links.empty()) sender->Send(links[roll() % links.size()], bytes), net.pump();
            if (roll() % 8 == 0) net.wait(2);
            const proto::Room &r = a.s->room();
            ok = ok && a.s->mode() == session::Mode::kHosting && r.person(0) && r.people.size() <= 3 && r.people.size() == a.s->MemberLinks().size() + 1 && !a.asking();
            ok = ok && r.panels.size() <= size_t(room::kOwnPanels) * r.people.size();
            for (const proto::Panel &p : r.panels) ok = ok && r.person(p.owner()) && p.number() < room::kOwnPanels;
            for (Frame *g : {&b, &c})
                if (g->s->mode() == session::Mode::kJoined) ok = ok && g->s->room().person(g->s->me()) && r.person(g->s->me()) && g->s->room().people.size() <= 3;
        }
    }
    check("noise and forged messages on every link: the owner's room holds only who it let in, and only their own panels", ok);
}

// What the local tailnet says isn't checked as a peer's text is: a login with a control
// character in it still makes good JSON (commands.cpp Quoted).
static void tailnetText() {
    Net net;
    Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo");
    b.login = "Bo\x01\n@example.com";
    a.ask("open");
    const std::string code = a.ask("invite").substr(3);
    const bool ok = b.ask("join 100.64.0.1 47193 " + code) == "ok" && a.asking();
    check("a login with control characters is written escaped in the state", ok && a.has("\"login\":\"Bo\\u0001\\u000a@example.com\"") && !a.has("Bo\x01"));
}

int main() {
    joining();
    mesh();
    versions();
    strangers();
    knocks();
    panels();
    looks();
    noise();
    tailnetText();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
