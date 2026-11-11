// Control of a shared panel, its bytes and its small rules (together/proto/control.h and
// pointwire.h, together/room/grants.h and desk.h, together/local/pointgate.h): the three
// messages and the pointer datagram there and back, cut short, out of bounds and as noise; who
// holds a grant and for how long; who has the desk, and what is let go for them; which
// pointers are passed on. No sockets, no session.
#include "../local/pointgate.h"
#include "../proto/control.h"
#include "../room/desk.h"
#include "../room/grants.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint32_t g_seed = 4711;
static uint32_t roll() { return g_seed = g_seed * 1664525u + 1013904223u, g_seed >> 8; }

// A framed message's body, and its kind.
static std::vector<uint8_t> body(const std::vector<uint8_t> &framed) { return {framed.begin() + 6, framed.end()}; }
static int kind(const std::vector<uint8_t> &framed) { return framed[4] | framed[5] << 8; }

static proto::Act act(uint8_t what, int32_t a, int32_t b, uint8_t panel = 9) {
    proto::Act m;
    m.panel = panel, m.what = what, m.a = a, m.b = b;
    return m;
}
static bool reads(const proto::Act &m) {
    proto::Act back;
    return proto::Read(proto::Body(m), back) && back.panel == m.panel && back.what == m.what && back.a == m.a && back.b == m.b;
}

static void bytes() {
    proto::AskControl ask, askBack;
    ask.panel = 17;
    check("an ask there and back", kind(proto::Pack(ask)) == proto::kAskControl && proto::Read(body(proto::Pack(ask)), askBack) && askBack.panel == 17);
    proto::Grant grant, grantBack;
    grant.panel = 3, grant.seconds = 10;
    check("a grant there and back", kind(proto::Pack(grant)) == proto::kGrant && proto::Read(body(proto::Pack(grant)), grantBack) && grantBack.panel == 3 && grantBack.seconds == 10);
    grant.seconds = 200;
    check("a grant says thirty seconds at most, and a longer one doesn't read", proto::Read(body(proto::Pack(grant)), grantBack) && grantBack.seconds == proto::kMostLease &&
                                                                                    !proto::Read({3, 31}, grantBack) && proto::Read({3, 0}, grantBack));
    check("a panel no room has doesn't read", !proto::Read({32}, askBack) && !proto::Read({32, 5}, grantBack) && !reads(act(proto::kActMove, 1, 1, 32)));

    check("an act frames as one", kind(proto::Pack(act(proto::kActMove, 1, 2))) == proto::kAct && body(proto::Pack(act(proto::kActMove, 1, 2))) == proto::Body(act(proto::kActMove, 1, 2)));
    check("a move anywhere on the panel", reads(act(proto::kActMove, 0, 0)) && reads(act(proto::kActMove, 65535, 65535)) && reads(act(proto::kActMove, 30000, 12)));
    check("...and nowhere off it", !reads(act(proto::kActMove, -1, 0)) && !reads(act(proto::kActMove, 0, 65536)) && !reads(act(proto::kActMove, 1 << 30, 0)));
    check("the three buttons, down and up", reads(act(proto::kActButton, proto::kLeft, 1)) && reads(act(proto::kActButton, proto::kMiddle, 0)) && reads(act(proto::kActButton, proto::kRight, 1)));
    check("...no others, and nothing between down and up", !reads(act(proto::kActButton, 0, 1)) && !reads(act(proto::kActButton, 4, 1)) && !reads(act(proto::kActButton, 1, 2)) &&
                                                               !reads(act(proto::kActButton, 1, -1)));
    check("the wheel either way", reads(act(proto::kActScroll, 0, 120)) && reads(act(proto::kActScroll, -240, 0)) && reads(act(proto::kActScroll, 2400, -2400)));
    check("...not further than one turns, and not nowhere", !reads(act(proto::kActScroll, 0, 2401)) && !reads(act(proto::kActScroll, -2401, 0)) && !reads(act(proto::kActScroll, 0, 0)));
    check("a key down and up", reads(act(proto::kActKey, 30, 1)) && reads(act(proto::kActKey, 255, 0)) && !reads(act(proto::kActKey, 0, 1)) && !reads(act(proto::kActKey, 256, 1)) &&
                                   !reads(act(proto::kActKey, 30, 2)));
    check("letting everything go is never said on a link, nor a kind nobody knows", !reads(act(proto::kActLetGo, 0, 0)) && !reads(act(5, 0, 0)) && !reads(act(255, 1, 1)));

    bool ok = true;
    const std::vector<uint8_t> whole = proto::Body(act(proto::kActKey, 30, 1));
    for (size_t n = 0; n < whole.size(); ++n) {
        proto::Act back;
        ok = ok && !proto::Read(std::vector<uint8_t>(whole.begin(), whole.begin() + long(n)), back);
    }
    std::vector<uint8_t> longer = whole;
    longer.push_back(0);
    proto::Act back;
    check("an act cut short at any length, or with a byte too many, doesn't read", ok && !proto::Read(longer, back) && !proto::Read({}, askBack) && !proto::Read({1, 2, 3}, grantBack));

    int read = 0;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> noise(roll() % 14);
        for (uint8_t &b : noise) b = uint8_t(roll());
        proto::Act a;
        proto::Point p;
        if (proto::Read(noise, a)) ++read, ok = ok && a.panel < 32 && a.what >= proto::kActMove && a.what <= proto::kActKey;
        if (proto::Read(noise.data(), noise.size(), p)) ok = ok && (p.panel < 32 || p.panel == proto::kNoPanel) && p.flags <= 1;
        proto::Read(noise, askBack), proto::Read(noise, grantBack);
    }
    check("noise reads as an act only when it is one", ok && read < 2000);
}

static std::vector<uint8_t> point(uint16_t count, uint8_t panel = 9, uint16_t x = 100, uint16_t y = 200, uint8_t flags = 0) {
    proto::Point p;
    p.count = count, p.panel = panel, p.x = x, p.y = y, p.flags = flags;
    return proto::Pack(p);
}

static void points() {
    proto::Point back;
    const std::vector<uint8_t> d = point(513, 17, 65535, 1, proto::kPointPressed);
    check("a pointer there and back", d.size() == proto::kPointBytes && d[0] == proto::kPointDatagram && proto::Read(d.data(), d.size(), back) && back.count == 513 &&
                                          back.panel == 17 && back.x == 65535 && back.y == 1 && back.flags == proto::kPointPressed);
    const std::vector<uint8_t> none = point(1, proto::kNoPanel);
    check("...and one that points at nothing", proto::Read(none.data(), none.size(), back) && back.panel == proto::kNoPanel);
    std::vector<uint8_t> bad = d;
    bool ok = true;
    for (size_t n = 0; n < d.size(); ++n) ok = ok && !proto::Read(d.data(), n, back);
    bad.push_back(0);
    ok = ok && !proto::Read(bad.data(), bad.size(), back);
    bad = d, bad[0] = 1;
    ok = ok && !proto::Read(bad.data(), bad.size(), back);
    bad = point(1, 32);
    ok = ok && !proto::Read(bad.data(), bad.size(), back);
    bad = point(1, 9, 0, 0, 2);
    check("what isn't one doesn't read: cut short, too long, another kind, a panel no room has, a flag nobody knows", ok && !proto::Read(bad.data(), bad.size(), back));

    local::PointGate g;
    const auto take = [&g](int link, const std::vector<uint8_t> &p, double now) { return g.Take(link, p.data(), p.size(), now); };
    check("a link's first pointer is passed on, a newer one too, an older or the same one isn't", take(1, point(65530), 0) && take(1, point(65531), 0.03) && !take(1, point(65531), 0.04) &&
                                                                                                  !take(1, point(65500), 0.05) && take(1, point(2), 0.06));
    check("each link has its own count, and rubbish is nothing", take(2, point(1), 0.1) && !take(2, bad, 0.1) && !g.Take(2, nullptr, 0, 0.1) && take(2, point(2), 0.2));
    int taken = 0;
    uint16_t count = 100;
    for (int i = 0; i < 1000; ++i) taken += take(3, point(count++), 1 + i * 0.001);
    check("a link that sends a thousand a second gets about ninety through", taken >= 100 && taken <= 130);
    g.Forget(1);
    check("a link that is gone starts afresh", take(1, point(1), 3));

    local::PointCount c;
    std::vector<uint8_t> out;
    const std::vector<uint8_t> mine = point(40000, 5, 7, 8, 1);
    check("this Frame's own pointer goes out with ft-together's count", c.Stamp(mine.data(), mine.size(), out) && proto::Read(out.data(), out.size(), back) && back.count == 1 &&
                                                                            back.panel == 5 && back.x == 7 && back.y == 8 && back.flags == 1 && c.Stamp(mine.data(), mine.size(), out) &&
                                                                            proto::Read(out.data(), out.size(), back) && back.count == 2);
    check("...and what isn't a pointer goes nowhere", !c.Stamp(bad.data(), bad.size(), out) && !c.Stamp(nullptr, 0, out));
}

static bool has(const std::vector<room::Granted> &list, int number, int person) {
    for (const room::Granted &g : list)
        if (g.number == number && g.person == person) return true;
    return false;
}

static void grants() {
    room::Grants g;
    const auto all = [](int, int) { return true; };
    check("nothing is given while nobody says the wearer is there", !g.Give(0, 1, 100) && g.left(100) == 0 && !g.May(0, 1, 100));
    g.Here(100);
    check("someone asks, and it isn't theirs yet", g.Ask(0, 1, 100) && has(g.asks(), 0, 1) && !g.May(0, 1, 100));
    check("the wearer gives it: the ask is answered, and it is theirs", g.Give(0, 1, 101) && g.asks().empty() && has(g.given(), 0, 1) && g.May(0, 1, 101));
    check("...that panel only, and that person only", !g.May(1, 1, 101) && !g.May(0, 2, 101) && g.any(1) && !g.any(2));
    check("asking for what is theirs already is no new ask", !g.Ask(0, 1, 102) && g.asks().empty());
    check("it stands ten seconds after the wearer was last known there", g.left(105) == 5 && g.May(0, 1, 109.9) && !g.May(0, 1, 110.1) && g.left(111) == 0);
    g.Here(109);
    check("...and on from each word that they are", g.May(0, 1, 118.9) && g.Sweep(118.9, all).empty() && has(g.given(), 0, 1));
    const std::vector<room::Granted> ended = g.Sweep(119.5, all);
    check("with no word it is gone, and has to be given again", ended.size() == 1 && has(ended, 0, 1) && g.given().empty() && (g.Here(120), !g.May(0, 1, 120)));

    g.Give(0, 1, 120), g.Give(1, 1, 120), g.Give(1, 2, 120), g.Ask(2, 3, 120);
    check("taken back from one person", g.Take(1, 1) && !g.May(1, 1, 120) && g.May(1, 2, 120) && g.May(0, 1, 120) && !g.Take(1, 1));
    check("an ask is refused the same way", g.Take(2, 3) && g.asks().empty() && !g.Take(2, 3));
    g.Ask(2, 3, 120);
    std::vector<room::Granted> gone = g.Sweep(121, [](int number, int) { return number != 1; });
    check("a panel that is no longer shared takes its grants with it", gone.size() == 1 && has(gone, 1, 2) && g.May(0, 1, 121) && has(g.asks(), 2, 3));
    gone = g.Sweep(121, [](int, int person) { return person != 3; });
    check("someone who left takes their asks with them", gone.empty() && g.asks().empty());
    g.Ask(2, 2, 121), g.Here(239);
    check("an ask waits two minutes for an answer", g.Sweep(240.9, all).empty() && has(g.asks(), 2, 2) && (g.Sweep(241.1, all), g.asks().empty()));
    g.Ask(2, 2, 242);
    gone = g.End(false);
    check("the wearer goes away: every grant ends, and the asks wait", gone.size() == 1 && has(gone, 0, 1) && g.given().empty() && has(g.asks(), 2, 2));
    g.Give(0, 1, 242);
    check("the room is left: nothing stands", g.End(true).size() == 1 && g.given().empty() && g.asks().empty());
}

static bool ups(const std::vector<proto::Act> &let, std::initializer_list<std::pair<int, int>> what) {
    if (let.size() != what.size()) return false;
    size_t i = 0;
    for (const auto &w : what) {
        if (let[i].what != w.first || let[i].a != w.second || let[i].b != 0) return false;
        ++i;
    }
    return true;
}

static void desk() {
    using room::Desk;
    Desk d;
    std::vector<proto::Act> let;
    const proto::Act move = act(proto::kActMove, 5, 5), down = act(proto::kActButton, proto::kLeft, 1), up = act(proto::kActButton, proto::kLeft, 0);
    check("nobody has the desk, and whoever acts first does", d.holder() == Desk::kNobody && d.Take(1, move, 10) && d.holder() == 1);
    check("someone else's input isn't taken while they do", !d.Take(2, move, 10.5) && !d.Take(2, down, 11) && d.holder() == 1);
    check("...until a second and a half after their last", d.Take(1, move, 11.2) && !d.Take(2, move, 12.6) && d.Take(2, move, 12.8) && d.holder() == 2);
    check("it frees by itself", (d.Tick(14.4, let), d.holder() == Desk::kNobody) && let.empty());

    check("a button goes down once and up once", d.Take(1, down, 20) && !d.Take(1, down, 20.1) && d.down() == 1 && d.Take(1, up, 20.2) && !d.Take(1, up, 20.3) && d.down() == 0);
    check("an up for what they don't hold isn't passed on", !d.Take(1, act(proto::kActKey, 30, 0), 20.4));
    d.Take(1, down, 21);
    check("while something is held down the desk stays theirs", (d.Tick(40, let), d.holder() == 1) && let.empty() && !d.Take(2, move, 40) && d.Take(1, move, 40.1));
    check("...but not for longer than a minute: it is let go for them", (d.Tick(81.5, let), ups(let, {{proto::kActButton, proto::kLeft}})) && d.down() == 0 && d.holder() == 1 &&
                                                                          (d.Tick(83.1, let), d.holder() == Desk::kNobody));

    let.clear();
    d.Take(1, down, 100), d.Take(1, act(proto::kActKey, 42, 1), 100), d.Take(1, act(proto::kActKey, 30, 1), 100);
    d.Leave(2, let);
    check("someone who doesn't have the desk leaving changes nothing", let.empty() && d.holder() == 1 && d.down() == 3);
    d.Leave(1, let);
    check("its holder losing it: what they held is let go, the last first, and the desk is free", ups(let, {{proto::kActKey, 30}, {proto::kActKey, 42}, {proto::kActButton, proto::kLeft}}) &&
                                                                                                    d.holder() == Desk::kNobody && d.Take(2, move, 100.1));
    let.clear();
    d.Take(2, down, 101);
    d.Own(0, 101.5, let);
    check("the wearer's own hand takes it from anyone, and their button is let go", ups(let, {{proto::kActButton, proto::kLeft}}) && d.holder() == 0 && !d.Take(2, move, 102) &&
                                                                                       !d.Take(2, up, 102));
    let.clear();
    check("...for as long as it is on their desktop, and a moment more", (d.Own(0, 103, let), !d.Take(2, move, 104.4)) && d.Take(2, move, 104.6) && let.empty());

    Desk keys;
    bool ok = true;
    for (int key : {1, 14, 28, 30, 57, 59, 88, 100, 103, 111}) ok = ok && room::Typed(key) && keys.Take(1, act(proto::kActKey, key, 1), 1) && keys.Take(1, act(proto::kActKey, key, 0), 1);
    check("the keys of text and of moving about in it may be typed", ok);
    ok = true;
    for (int key : {0, 99, 112, 113, 114, 115, 116, 125, 126, 142, 255}) ok = ok && !room::Typed(key) && !keys.Take(1, act(proto::kActKey, key, 1), 2);
    check("the desktop's own keys may not: Meta, SysRq, power, sleep, the volume", ok);
    Desk many;
    ok = true;
    for (int key = 2; key < 10; ++key) ok = ok && many.Take(1, act(proto::kActKey, key, 1), 1);
    check("eight things are held down at once at most", ok && !many.Take(1, act(proto::kActKey, 10, 1), 1) && many.Take(1, act(proto::kActKey, 2, 0), 1) &&
                                                             many.Take(1, act(proto::kActKey, 10, 1), 1));
    Desk refused;
    check("input that isn't taken doesn't take the desk", !refused.Take(1, act(proto::kActKey, 125, 1), 1) && !refused.Take(1, up, 1) && refused.holder() == Desk::kNobody);
}

int main() {
    bytes();
    points();
    grants();
    desk();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
