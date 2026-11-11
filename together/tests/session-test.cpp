// A Frametop Together session's rules and its commands (together/session), several sessions run against
// each other on a network made here: a link is two queues, and the test hands the bytes over.
// Invites, asking and being let in or not, a full room, the guests' own links, leaving, a lost
// owner, another version, and people who say what they may not. No sockets, no clock.
#include "../session/commands.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <set>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint64_t g_seed = 99;
static uint64_t roll() { return g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull, g_seed >> 11; }

struct Net;

// One Frame: its session, and its end of every link.
struct Frame : session::Links, session::Tailnet {
    Net &net;
    std::string address, login;
    uint16_t port;
    bool onTailnet = true;
    std::unique_ptr<session::Session> s;
    std::vector<std::string> called;   // every host it called

    Frame(Net &n, const std::string &address, uint16_t port, const std::string &name);
    int Call(const std::string &host, uint16_t port) override;
    void Send(int link, const std::vector<uint8_t> &message) override;
    void Close(int link) override;
    bool Self(session::Who &who, std::string &at) override { return who.login = login, who.name = "frame-" + login, at = address, onTailnet; }
    bool WhoIs(const std::string &at, session::Who &who) override;
    bool MayCall(const std::string &at) override { return at.rfind("100.", 0) == 0; }

    std::string ask(const std::string &text);
    std::string state() { return session::State(*s); }
    bool has(const std::string &text) { return state().find(text) != std::string::npos; }
    // The number of the first ask waiting for this owner's answer, or 0.
    int asking() {
        const std::string st = state();
        const size_t at = st.find("\"asks\":[{\"id\":");
        return at == std::string::npos ? 0 : std::atoi(st.c_str() + at + 14);
    }
};

// The network between them. A link has a number at each end; what one end does reaches the
// other when the test pumps.
struct Net {
    struct End {
        Frame *frame = nullptr;
        int link = -1;
    };
    struct Event {
        End to;
        enum { kUp, kBytes, kDown } what;
        std::string address;
        std::vector<uint8_t> bytes;
    };
    std::vector<Frame *> frames;
    std::map<std::pair<Frame *, int>, End> far;   // each end's other end
    std::deque<Event> queue;
    std::map<int, std::vector<uint8_t>> heard;    // what was sent on a link whose other end is the test's
    std::set<int> shut;                           // ...and the ones of those that were closed
    std::function<void(Event &)> tamper;          // rewrites what is on its way
    int nextLink = 1;
    double now = 1000;
    size_t piece = 0;   // bytes arrive in pieces this long (0: whole)

    void pump() {
        while (!queue.empty()) {
            Event e = queue.front();
            queue.pop_front();
            if (tamper) tamper(e);
            session::Session &s = *e.to.frame->s;
            if (e.what == Event::kUp) s.Up(e.to.link, e.address, now);
            else if (e.what == Event::kDown) s.Down(e.to.link, now);
            else if (!piece) s.Bytes(e.to.link, e.bytes.data(), e.bytes.size(), now);
            else
                for (size_t at = 0; at < e.bytes.size(); at += piece) s.Bytes(e.to.link, e.bytes.data() + at, std::min(piece, e.bytes.size() - at), now);
        }
    }
    void wait(double seconds) {
        for (double end = now + seconds; now < end;) {
            now += 1;
            for (Frame *f : frames) f->s->Tick(now);
            pump();
        }
    }
    // The link between two Frames breaks under both.
    void cut(Frame &a, Frame &b) {
        for (auto it = far.begin(); it != far.end();) {
            if (it->first.first != &a || it->second.frame != &b) {
                ++it;
                continue;
            }
            queue.push_back({{&a, it->first.second}, Event::kDown, "", {}});
            queue.push_back({it->second, Event::kDown, "", {}});
            far.erase({it->second.frame, it->second.link});
            it = far.erase(it);
        }
        pump();
    }
    // A new link to `to` whose other end is the test's: what comes in on it is the test's to write.
    End stranger(Frame &to, const std::string &from) {
        const End theirs = {&to, nextLink++};
        queue.push_back({theirs, Event::kUp, from, {}});
        pump();
        return theirs;
    }
    void say(const End &to, const std::vector<uint8_t> &bytes) {
        queue.push_back({to, Event::kBytes, "", bytes});
        pump();
    }
    // Why the test's end of a link was refused, or 0 when it wasn't.
    int refused(const End &theirs) {
        const std::vector<uint8_t> &bytes = heard[theirs.link];
        proto::Parser parser;
        parser.Feed(bytes.data(), bytes.size());
        uint16_t kind = 0;
        std::vector<uint8_t> body;
        proto::Refuse r;
        while (parser.Next(kind, body))
            if (kind == proto::kRefuse && proto::Read(body, r)) return int(r.why);
        return 0;
    }
};

Frame::Frame(Net &n, const std::string &at, uint16_t p, const std::string &name) : net(n), address(at), login(name + "@example.com"), port(p) {
    session::Config c;
    c.name = name, c.port = p, c.dice = roll;
    s = std::make_unique<session::Session>(*this, *this, c);
    net.frames.push_back(this);
}

int Frame::Call(const std::string &host, uint16_t p) {
    called.push_back(host);
    const int mine = net.nextLink++;
    Frame *callee = nullptr;
    for (Frame *f : net.frames)
        if ((f->address == host || "frame-" + f->login == host) && f->port == p) callee = f;
    if (!callee) return net.queue.push_back({{this, mine}, Net::Event::kDown, "", {}}), mine;
    const int theirs = net.nextLink++;
    net.far[{this, mine}] = {callee, theirs};
    net.far[{callee, theirs}] = {this, mine};
    net.queue.push_back({{callee, theirs}, Net::Event::kUp, address, {}});
    net.queue.push_back({{this, mine}, Net::Event::kUp, callee->address, {}});
    return mine;
}

void Frame::Send(int link, const std::vector<uint8_t> &message) {
    const auto it = net.far.find({this, link});
    if (it != net.far.end()) net.queue.push_back({it->second, Net::Event::kBytes, "", message});
    else net.heard[link].insert(net.heard[link].end(), message.begin(), message.end());
}

void Frame::Close(int link) {
    const auto it = net.far.find({this, link});
    if (it == net.far.end()) return void(net.shut.insert(link));
    net.queue.push_back({it->second, Net::Event::kDown, "", {}});
    net.far.erase({it->second.frame, it->second.link});
    net.far.erase({this, link});
}

bool Frame::WhoIs(const std::string &at, session::Who &who) {
    for (Frame *f : net.frames)
        if (f->address == at && f->onTailnet) return who.login = f->login, who.name = "frame-" + f->login, true;
    return false;
}

std::string Frame::ask(const std::string &text) {
    const std::string reply = session::Command(*s, text, net.now);
    net.pump();
    return reply;
}

static std::string where(const Frame &f) { return f.address + " " + std::to_string(f.port); }

// `guest` joins `owner`'s room with a fresh invite and is let in.
static bool join(Frame &owner, Frame &guest) {
    const std::string invite = owner.ask("invite");
    if (invite.rfind("ok ", 0) != 0 || guest.ask("join " + where(owner) + " " + invite.substr(3)) != "ok") return false;
    return owner.ask("accept " + std::to_string(owner.asking())) == "ok" && guest.s->mode() == session::Mode::kJoined;
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
    check("away and back reach everyone", ok && a.ask("away asleep").rfind("error", 0) == 0 && c.ask("away off").rfind("error", 0) == 0);

    ok = b.ask("leave") == "ok" && idle(b) && a.s->room().people.size() == 1 && a.has("Bo left.") && a.s->MemberLinks().empty();
    check("a guest leaves, and the room goes on without them", ok);
    ok = join(a, b) && a.ask("leave") == "ok" && idle(a) && idle(b) && b.has("its owner closed it");
    check("the owner leaves, and the room ends for everyone", ok && net.far.empty());
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

// Noise on every kind of link: nothing crashes (the sanitizers watch), and the owner's room
// only ever holds the people it let in.
static void noise() {
    bool ok = true;
    for (int round = 0; round < 300; ++round) {
        Net net;
        net.piece = round % 3 ? 0 : 1 + round % 7;
        Frame a(net, "100.64.0.1", 47193, "Ann"), b(net, "100.64.0.2", 47193, "Bo"), c(net, "100.64.0.3", 47193, "Cy");
        ok = ok && a.ask("open") == "ok" && join(a, b) && join(a, c);
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
                const std::vector<uint8_t> real[6] = {proto::Pack(a.s->room()), proto::Pack(w), hello(proto::kVersion, code, a.s->room().room), proto::PackLeave(), proto::Pack(proto::Away{}), proto::Pack(proto::Refuse{})};
                bytes = real[roll() % 6];
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
            for (Frame *g : {&b, &c})
                if (g->s->mode() == session::Mode::kJoined) ok = ok && g->s->room().person(g->s->me()) && r.person(g->s->me()) && g->s->room().people.size() <= 3;
        }
    }
    check("noise and forged messages on every link: the owner's room holds only who it let in", ok);
}

int main() {
    joining();
    mesh();
    versions();
    strangers();
    knocks();
    noise();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
