// The shared panels' video through ft-together (together/flow/video.h): several Frames in a room
// on the network made in the test (fakenet.h), each with its video rules, a wire for pictures
// that the test can lose, hold back and refuse, and programs that only remember what they were
// told. Who gets a picture and when, what a viewer shows and in what order, the keyframes asked
// for, the bitrate, and the pictures nobody may send.
#include "../flow/video.h"
#include "fakenet.h"

#include <cstdio>

static int failed = 0;
static bool check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

using Headset = Frame;   // fakenet's: the wire below has a Frame of its own
using Unit = std::vector<uint8_t>;

struct Shown {
    uint8_t panel;
    bool key;
    Unit unit;
};

// One Frame's video, the wire its pictures go out on, and the programs it talks to.
struct Seat : flow::Wires, flow::Programs {
    Net &net;
    Headset &headset;
    std::map<Headset *, Seat *> &seats;
    flow::Video video;

    // the programs: what they were last told, and how often
    std::map<int, int> rate, keys;
    std::map<int, bool> watched;
    std::vector<Shown> shown;

    // the wire
    bool refuse = false;                           // nothing can be sent
    std::set<int> losing;                          // links whose pictures never arrive
    bool hold = false;                             // pictures wait in `held` for the test to deliver
    std::vector<std::function<void()>> held;
    std::vector<std::pair<int, uint32_t>> lost;    // given up, not yet told
    int sent = 0;

    Seat(Net &n, Headset &h, std::map<Headset *, Seat *> &all) : net(n), headset(h), seats(all), video(*h.s, *this, *this) {
        seats[&h] = this;
        h.onWatch = [this](int link, const proto::Watch &w, double now) { video.Watch(link, w, now); };
    }

    bool Frame(int link, std::vector<uint8_t> bytes, uint32_t tag) override {
        if (refuse) return false;
        ++sent;
        if (losing.count(link)) return lost.push_back({link, tag}), true;
        const auto far = net.far.find({&headset, link});
        if (far == net.far.end()) return true;
        Seat *to = seats.at(far->second.frame);
        const int theirs = far->second.link;
        auto deliver = [this, to, theirs, bytes = std::move(bytes)] { to->video.Frame(theirs, bytes.data(), bytes.size(), net.now); };
        if (hold) held.push_back(std::move(deliver));
        else deliver();
        return true;
    }
    void Rate(int number, int bitrate) override { rate[number] = bitrate; }
    void Key(int number) override { ++keys[number]; }
    void Watched(int number, bool w) override { watched[number] = w; }
    void Show(uint8_t panel, bool key, const Unit &unit) override { shown.push_back({panel, key, unit}); }

    // The source of panel `number` hands over a picture of `size` bytes.
    Unit unit(int number, bool key, size_t size = 600) {
        Unit u(size);
        for (uint8_t &b : u) b = uint8_t(roll());
        video.Unit(number, key, u.data(), u.size(), net.now);
        net.pump();
        return u;
    }
    // The links say which pictures they gave up.
    void tell() {
        for (const auto &[link, tag] : lost) video.Lost(link, tag, net.now);
        lost.clear();
    }
    // The units shown since `from`, and whether they were all of `panel`.
    std::vector<Unit> since(size_t from, uint8_t panel) const {
        std::vector<Unit> out;
        for (size_t i = from; i < shown.size(); ++i) out.push_back(shown[i].panel == panel ? shown[i].unit : Unit{});
        return out;
    }
};

struct Table {
    Net net;
    Headset a{net, "100.64.0.1", 47193, "Ann"}, b{net, "100.64.0.2", 47193, "Bo"}, c{net, "100.64.0.3", 47193, "Cy"};
    std::map<Headset *, Seat *> seats;
    Seat sa{net, a, seats}, sb{net, b, seats}, sc{net, c, seats};

    bool sit() { return a.ask("open") == "ok" && join(a, b) && join(a, c) && b.s->Linked(c.s->me()) && c.s->Linked(b.s->me()); }
    void tick(double seconds) {
        net.now += seconds;
        for (const auto &[h, s] : seats) s->video.Tick(net.now);
        net.pump();
    }
    // The number `from` has for its link to `to`.
    int link(Headset &from, Headset &to) {
        for (const auto &[end, far] : net.far)
            if (end.first == &from && far.frame == &to) return end.second;
        return -1;
    }
};

static const char *kScreen = " 1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen";

static void watching() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb, &sc = t.sc;
    if (!check("three people sit in a room", t.sit())) return;
    const uint8_t pa = proto::PanelId(t.a.s->me(), 0);

    sa.video.SourceCame(0);
    check("a source is told its bitrate, and that nobody watches", sa.rate[0] == flow::Pace::kStart && sa.watched.count(0) && !sa.watched[0]);
    sa.unit(0, true);
    check("a picture of a panel that isn't shared goes nowhere", sa.sent == 0);
    bool ok = t.a.ask(std::string("share 0") + kScreen) == "ok";
    sa.unit(0, true);
    check("...nor one of a shared panel nobody watches", ok && sa.sent == 0 && !sa.watched[0] && sa.keys[0] == 0);
    for (int i = 0; i < 4; ++i) t.tick(3);
    check("...whose bitrate stays where it is: nothing is sent that could say the link is good", sa.rate[0] == flow::Pace::kStart);

    sb.video.SinkCame(pa, t.net.now);
    t.net.pump();
    check("someone begins to watch: the source hears that, and is asked for a keyframe", sa.watched[0] && sa.keys[0] == 1 && sa.video.viewers(0) == 1);
    sa.unit(0, false);
    check("a picture that leans on one they never had isn't sent to them", sa.sent == 0 && sb.shown.empty());
    const Unit k = sa.unit(0, true), d1 = sa.unit(0, false);
    ok = sb.shown.size() == 2 && sb.shown[0].panel == pa && sb.shown[0].key && sb.shown[0].unit == k && !sb.shown[1].key && sb.shown[1].unit == d1;
    check("the keyframe is shown, and what follows it", ok && sc.shown.empty() && sa.keys[0] == 1);

    t.tick(0.1);
    sc.video.SinkCame(pa, t.net.now);
    t.net.pump();
    const Unit d2 = sa.unit(0, false);
    check("a second viewer waits for a keyframe while the first goes on", sb.shown.size() == 3 && sb.shown[2].unit == d2 && sc.shown.empty() && sa.video.viewers(0) == 2);
    check("...which isn't asked for within half a second of the last", sa.keys[0] == 1);
    t.tick(0.6);
    const Unit k2 = sa.unit(0, true), d3 = sa.unit(0, false);
    check("...and then goes to both: the panel is encoded once", sa.keys[0] == 2 && sb.since(3, pa) == (std::vector<Unit>{k2, d3}) && sc.since(0, pa) == (std::vector<Unit>{k2, d3}));

    // pictures that don't arrive
    const int toB = t.link(t.a, t.b);
    size_t nb = sb.shown.size(), nc = sc.shown.size();
    sa.losing.insert(toB);
    const Unit d4 = sa.unit(0, false);
    sa.tell();
    sa.losing.clear();
    const int sent = sa.sent;
    const Unit d5 = sa.unit(0, false);
    check("a picture given up on one link: nothing more goes there, and the others go on",
          sa.sent == sent + 1 && sb.shown.size() == nb && sc.since(nc, pa) == (std::vector<Unit>{d4, d5}));
    check("one of them doesn't change the bitrate", sa.rate[0] == flow::Pace::kStart && sa.video.bitrate(0) == flow::Pace::kStart);
    t.tick(0.6);
    check("a keyframe is asked for", sa.keys[0] == 3);
    sa.losing.insert(toB);
    sa.unit(0, true);
    sa.tell();
    sa.losing.clear();
    check("a second within a second: the source is told to encode at less", sa.rate[0] == flow::Pace::kStart * flow::Pace::kDown / 100 && sb.shown.size() == nb);
    t.tick(0.6);
    const Unit k3 = sa.unit(0, true);
    check("the next keyframe puts them back in step", sa.keys[0] == 4 && sb.since(nb, pa) == std::vector<Unit>{k3});

    nb = sb.shown.size(), nc = sc.shown.size();
    sa.refuse = true;
    sa.unit(0, false);
    sa.refuse = false;
    sa.unit(0, false);
    check("a picture the link can't take at all counts as given up", sb.shown.size() == nb && sc.shown.size() == nc && sa.rate[0] < flow::Pace::kStart * flow::Pace::kDown / 100);
    t.tick(0.6);
    sa.unit(0, true);
    check("...and a keyframe mends that too", sb.shown.size() == nb + 1 && sc.shown.size() == nc + 1);
    const int low = sa.rate[0];
    t.tick(4);
    ok = sa.rate[0] == low;
    t.tick(1.1);
    ok = ok && sa.rate[0] == low * flow::Pace::kUp / 100;
    for (int i = 0; i < 40; ++i) t.tick(5.1);
    check("five seconds with none given up: a little more, and so on up to the cap", ok && sa.rate[0] == flow::Video::kBalanced);

    const int asked = sa.keys[0];
    sa.video.Unit(0, true, nullptr, 0, t.net.now);
    sa.unit(0, false);
    ok = sb.shown.size() == nb + 2 && sa.keys[0] == asked;
    sa.unit(0, false, proto::kMostFrame + 1);
    sa.unit(0, false);
    check("a picture longer than any may be isn't sent, nor what leans on it: a keyframe is asked for", ok && sb.shown.size() == nb + 2 && sc.shown.size() == nc + 2 && sa.keys[0] == asked + 1);
    const Unit big = sa.unit(0, true, proto::kMostFrame);
    check("...and the longest there may be is sent", sb.shown.size() == nb + 3 && sb.shown.back().unit == big && sc.shown.back().unit == big);
}

static void order() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb;
    if (!check("three people sit in a room", t.sit() && t.a.ask(std::string("share 0") + kScreen) == "ok")) return;
    const uint8_t pa = proto::PanelId(t.a.s->me(), 0);
    sa.video.SourceCame(0);
    sb.video.SinkCame(pa, t.net.now);
    t.net.pump();

    sa.hold = true;
    const Unit k = sa.unit(0, true), d1 = sa.unit(0, false), d2 = sa.unit(0, false), d3 = sa.unit(0, false);
    bool ok = sa.held.size() == 4;
    sa.held[0](), sa.held[2](), sa.held[3]();
    ok = ok && sb.since(0, pa) == std::vector<Unit>{k};
    sa.held[1]();
    check("pictures that overtake one another are shown in the order they were made", ok && sb.since(0, pa) == (std::vector<Unit>{k, d1, d2, d3}));
    sa.held[1](), sa.held[0](), sa.held[3]();
    check("one that comes twice is shown once", sb.shown.size() == 4);

    sa.held.clear();
    sa.unit(0, false);   // never arrives, and its sender doesn't hear of it
    const Unit d5 = sa.unit(0, false);
    sa.held[1]();
    const int asked = sa.keys[0];
    t.tick(0.2);
    ok = sb.shown.size() == 4 && sa.keys[0] == asked;
    t.tick(0.6);
    check("a viewer who waits for a picture that never comes asks for a keyframe", ok && sa.keys[0] == asked + 1);
    sa.hold = false;
    const Unit k2 = sa.unit(0, true), d6 = sa.unit(0, false);
    check("...and goes on from it: the one it held is old by then", sb.since(4, pa) == (std::vector<Unit>{k2, d6}) && d5 != k2);

    // what the viewer has of the panel
    const size_t n = sb.shown.size();
    sb.video.Attention(pa, proto::kHidden, false, t.net.now);
    t.net.pump();
    sa.unit(0, false);
    check("a viewer who can't see the panel gets none of it, and with nobody watching the source hears so", sb.shown.size() == n && !sa.watched[0] && sa.video.viewers(0) == 0);
    sb.video.Attention(pa, 7, false, t.net.now);
    sb.video.Attention(proto::PanelId(t.a.s->me(), 2), proto::kFocused, false, t.net.now);
    t.net.pump();
    check("a level there isn't, or a word about a panel nothing here shows, says nothing", !sa.watched[0]);
    t.tick(0.6);
    sb.video.Attention(pa, proto::kFocused, false, t.net.now);
    t.net.pump();
    sa.unit(0, false);
    ok = sa.watched[0] && sb.shown.size() == n;
    const Unit k3 = sa.unit(0, true);
    check("looking at it again starts with a keyframe", ok && sb.since(n, pa) == std::vector<Unit>{k3});
    t.tick(0.6);
    const int before = sa.keys[0];
    sa.hold = true;
    sa.unit(0, false);   // on its way when the decoder loses its place
    sa.hold = false;
    sb.video.Attention(pa, proto::kFocused, true, t.net.now);
    t.net.pump();
    sa.held.back()();
    sa.unit(0, false);
    ok = sa.keys[0] == before + 1 && sb.shown.size() == n + 1;
    const Unit k4 = sa.unit(0, true);
    check("a decoder that lost its place says so, is shown nothing that was on its way, and gets a keyframe", ok && sb.since(n + 1, pa) == std::vector<Unit>{k4});

    const size_t told = t.a.watches.size();
    for (int i = 0; i < 3; ++i) t.tick(1.01);
    check("a viewer says what it has again every second, for an owner that missed it", t.a.watches.size() == told + 3 && !t.a.watches.back().second.fresh);

    // a sink that came before its panel
    const uint8_t pa1 = proto::PanelId(t.a.s->me(), 1);
    sb.video.SinkCame(pa1, t.net.now);
    t.net.pump();
    sa.video.SourceCame(1);
    ok = !sa.watched[1] && t.a.ask(std::string("share 1") + kScreen) == "ok";
    t.tick(0.1);
    check("a program that shows a panel before it is shared watches it once it is", ok && sa.watched[1] && sa.keys[1] == 1);

    sb.video.SinkWent(pa);
    t.net.pump();
    sa.unit(0, true);
    check("the program that shows a panel goes: its owner is told nobody looks", !sa.watched[0] && sb.since(n + 2, pa).empty());
}

static void rules() {
    Table t;
    Seat &sa = t.sa, &sb = t.sb, &sc = t.sc;
    if (!check("three people sit in a room", t.sit() && t.a.ask(std::string("share 0") + kScreen) == "ok")) return;
    const uint8_t pa = proto::PanelId(t.a.s->me(), 0), pa2 = proto::PanelId(t.a.s->me(), 2), pb = proto::PanelId(t.b.s->me(), 0);
    const int fromA = t.link(t.b, t.a), fromC = t.link(t.b, t.c);
    sa.video.SourceCame(0);
    sb.video.SinkCame(pa, t.net.now);
    sb.video.SinkCame(pa2, t.net.now);
    t.net.pump();
    const Unit k = sa.unit(0, true);
    check("the owner's picture is shown", sb.since(0, pa) == std::vector<Unit>{k} && sb.video.dropped() == 0);

    const Unit u(500, 7);
    const auto picture = [&u](uint8_t panel, uint32_t number, bool key) { return proto::PackFrame({panel, number, key}, u.data(), u.size()); };
    const auto take = [&](int link, const std::vector<uint8_t> &bytes) {
        const size_t was = sb.video.dropped(), shown = sb.shown.size();
        sb.video.Frame(link, bytes.data(), bytes.size(), t.net.now);
        return sb.video.dropped() == was + 1 && sb.shown.size() == shown;
    };
    check("a picture of someone else's panel isn't taken: only its owner sends it", take(fromC, picture(pa, 50, true)));
    check("...nor one of a panel that isn't in the room, or that nothing here shows", take(fromA, picture(pa2, 1, true)) && take(fromA, picture(proto::PanelId(t.a.s->me(), 1), 1, true)));
    check("...nor one of the wearer's own", take(fromA, picture(pb, 1, true)));
    check("...nor one from a link that is nobody's", take(999, picture(pa, 50, true)) && take(-1, picture(pa, 50, true)));
    proto::Watch word;
    word.level = proto::kFocused, word.fresh = true;
    for (int panel = 0; panel < 256; ++panel) word.panel = uint8_t(panel), sb.video.Watch(fromA, word, t.net.now);
    check("a word about a panel the wearer doesn't share, or that no headset could, changes nothing", sb.watched.empty() && sb.keys.empty());
    std::vector<uint8_t> bad = picture(pa, 50, true);
    bad[0] = 9;
    bool ok = take(fromA, bad) && take(fromA, {}) && take(fromA, std::vector<uint8_t>(proto::kFrameHead, 2));
    for (int i = 0; i < 2000 && ok; ++i) {
        std::vector<uint8_t> noise(roll() % 40);
        for (uint8_t &b : noise) b = uint8_t(roll());
        if (!noise.empty() && i % 2) noise[0] = proto::kFrameStream;
        const size_t shown = sb.shown.size();
        sb.video.Frame(fromA, noise.data(), noise.size(), t.net.now);
        ok = sb.shown.size() == shown || (sb.shown.back().panel == pa && sb.shown.back().key);   // noise that happens to be a keyframe of that panel is one
    }
    check("...nor anything that isn't a picture", ok);

    // more than a panel may send
    t.tick(2);
    sb.video.Attention(pa, proto::kInView, true, t.net.now);
    size_t n = sb.shown.size();
    for (uint32_t i = 0; i < 1000; ++i) {
        const std::vector<uint8_t> p = picture(pa, 1000 + i, i == 0);
        sb.video.Frame(fromA, p.data(), p.size(), t.net.now + i * 1e-6);
    }
    check("a thousand pictures in a moment: thirty are taken, the rest dropped", sb.shown.size() - n == size_t(flow::Video::kPicturesAtOnce));
    t.tick(1);
    n = sb.shown.size();
    for (uint32_t i = 0; i < 60; ++i) {
        const std::vector<uint8_t> p = picture(pa, 5000 + i, i == 0);
        sb.video.Frame(fromA, p.data(), p.size(), t.net.now + i / 60.0);
    }
    t.net.now += 1;
    check("sixty a second all pass", sb.shown.size() - n == 60);

    // a guest's panel, over the guests' own links
    ok = t.b.ask(std::string("share 0") + kScreen) == "ok" && t.a.s->room().panel(pb) && t.c.s->room().panel(pb);
    sb.video.SourceCame(0);
    sa.video.SinkCame(pb, t.net.now);
    sc.video.SinkCame(pb, t.net.now);
    t.net.pump();
    const Unit kb = sb.unit(0, true);
    check("a guest's panel goes to the owner and to the other guest alike", ok && sb.watched[0] && sa.since(0, pb) == std::vector<Unit>{kb} && sc.since(0, pb) == std::vector<Unit>{kb});

    ok = t.c.ask("leave") == "ok";
    t.tick(0.1);
    const Unit db = sb.unit(0, false);
    check("someone who leaves the room stops being sent to", ok && sb.video.viewers(0) == 1 && sa.shown.back().unit == db && sc.shown.size() == 1);
    sc.unit(0, true);
    check("...and sends nothing", sc.sent == 0);

    ok = t.b.ask("unshare 0") == "ok";
    t.tick(0.1);
    const int sentB = sb.sent;
    sb.unit(0, true);
    check("a panel taken back: its source hears nobody watches, and its pictures go nowhere", ok && !sb.watched[0] && sb.sent == sentB && sb.video.viewers(0) == 0);
    ok = t.b.ask(std::string("share 0") + kScreen) == "ok";
    t.tick(1.01);
    const Unit kb2 = sb.unit(0, true);
    check("shared again, whoever still shows it watches it again", ok && sb.watched[0] && sa.shown.back().unit == kb2);

    const size_t na = sa.shown.size();
    n = sb.shown.size();
    ok = t.a.ask("leave") == "ok";
    t.tick(0.1);
    sa.unit(0, true), sb.unit(0, true);
    t.tick(1.01);
    check("the room ends: nothing is sent or shown any more", ok && sa.shown.size() == na && sb.shown.size() == n && !sa.watched[0] && !sb.watched[0]);
}

int main() {
    watching();
    order();
    rules();
    std::printf(failed ? "FAILED\n" : "all ok\n");
    return failed ? 1 : 0;
}
