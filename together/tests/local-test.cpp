// What ft-together does for this Frame's own programs (together/local): which poses it passes
// on, the count and the mouth it sends its own with, who watches, the others' input on its way
// to the desktop, where the floor lies in the room, and the messages on the media programs'
// socket.
#include "../../conf/ftsock.h"
#include "../local/floor.h"
#include "../local/inputs.h"
#include "../local/mediawire.h"
#include "../local/mouth.h"
#include "../local/posegate.h"
#include "../local/voicegate.h"
#include "../local/watchers.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <utility>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static std::vector<uint8_t> pose(uint16_t turn, float x = 0) {
    proto::Pose p;
    p.turn = turn, p.eyes[0] = x, p.eyes[1] = 1.6f;
    return proto::Pack(p);
}

static void gate() {
    local::PoseGate g;
    const auto take = [&g](int link, const std::vector<uint8_t> &d, double now) { return g.Take(link, d.data(), d.size(), now); };
    check("a link's first pose is passed on, whatever its count", take(1, pose(40000), 0));
    check("a newer one too, and the same one again isn't", take(1, pose(40001), 0.03) && !take(1, pose(40001), 0.04));
    check("an older one isn't: it came late", !take(1, pose(39990), 0.05) && take(1, pose(40005), 0.06));
    check("the count goes round", take(1, pose(65535), 0.1) && take(1, pose(3), 0.13) && !take(1, pose(65535), 0.16));
    check("each link has its own count", take(2, pose(1), 0.2) && take(2, pose(2), 0.23) && take(1, pose(4), 0.26));

    std::vector<uint8_t> bad = pose(7);
    bad.pop_back();
    std::vector<uint8_t> far = pose(8, 1e6f), other = pose(9);
    other[0] = 2;
    check("what isn't a pose isn't passed on: cut short, nowhere near the room, another kind, nothing",
          !take(3, bad, 1) && !take(3, far, 1) && !take(3, other, 1) && !g.Take(3, nullptr, 0, 1));
    check("...and doesn't stand in the way of the pose after it", take(3, pose(1), 1.01));

    // more than a link may send
    int taken = 0;
    uint16_t turn = 100;
    for (int i = 0; i < 1000; ++i) taken += take(4, pose(turn++), 2 + i * 0.001);   // a thousand in a second
    check("a link that sends a thousand a second has about ninety of them passed on, and the rest dropped",
          taken >= int(local::PoseGate::kASecond) && taken <= int(local::PoseGate::kASecond + local::PoseGate::kAtOnce) + 1);
    taken = 0;
    for (int i = 0; i < 1000; ++i) taken += take(5, i % 10 ? bad : pose(turn++), 4 + i * 0.001);
    check("rubbish counts against it too", taken < 40);
    taken = 0;
    for (int i = 0; i < 90; ++i) taken += take(6, pose(turn++), 6 + i / 30.0);
    check("thirty a second all pass, for as long as they come", taken == 90);
    check("...and a flood on one link takes nothing from another", take(1, pose(5), 7));

    g.Forget(1);
    check("a link that is gone is forgotten: its number starts afresh", g.links() == 5 && take(1, pose(2), 8) && g.links() == 6);
}

static void count() {
    local::PoseCount c;
    const local::Mouth shut;
    std::vector<uint8_t> out;
    const std::vector<uint8_t> mine = pose(555, 0.5f);
    proto::Pose a, b;
    bool ok = c.Stamp(mine.data(), mine.size(), shut, 0, out) && proto::Read(out.data(), out.size(), a) && c.Stamp(mine.data(), mine.size(), shut, 0, out) &&
              proto::Read(out.data(), out.size(), b);
    check("this Frame's own poses go out with ft-together's count, one more each time", ok && a.turn == 1 && b.turn == 2 && a.eyes[0] == 0.5f && b.eyes[1] == 1.6f);
    std::vector<uint8_t> bad = mine;
    bad.push_back(0);
    out.clear();
    check("what isn't a pose doesn't go out", !c.Stamp(bad.data(), bad.size(), shut, 0, out) && !c.Stamp(nullptr, 0, shut, 0, out) && out.empty());
    local::PoseGate g;
    ok = true;
    for (int i = 0; i < 70000 && ok; ++i) ok = c.Stamp(mine.data(), mine.size(), shut, 0, out) && g.Take(1, out.data(), out.size(), i / 30.0);
    check("...and the far end takes every one of seventy thousand, round the count and on", ok);
}

// The mouth of the pose `mine` as it goes out at `now`: its five weights, "a,b,c,d,e".
static std::string sent(local::PoseCount &c, const local::Mouth &m, const std::vector<uint8_t> &mine, double now) {
    std::vector<uint8_t> out;
    proto::Pose p;
    if (!c.Stamp(mine.data(), mine.size(), m, now, out) || !proto::Read(out.data(), out.size(), p)) return "none";
    std::string said;
    for (uint8_t w : p.mouth) said += (said.empty() ? "" : ",") + std::to_string(w);
    return said;
}

static void mouth() {
    local::PoseCount c;
    local::Mouth m;
    const std::vector<uint8_t> mine = pose(1, 0.5f);
    check("a pose goes out with its mouth shut while nobody told of the wearer's", sent(c, m, mine, 5) == "0,0,0,0,0");
    const uint8_t open[local::kMouthBytes] = {local::kMouthToTogether, 200, 0, 30, 4, 255};
    check("the wearer's mouth, told by the program that hears them, goes out in their next pose", m.Take(open, sizeof open, 10) && sent(c, m, mine, 10.01) == "200,0,30,4,255");
    check("...and in those after it, for as long as it is held", sent(c, m, mine, 10 + local::Mouth::kHeldS) == "200,0,30,4,255");
    check("one that isn't told again shuts: the program may be gone", sent(c, m, mine, 10.01 + local::Mouth::kHeldS) == "0,0,0,0,0");
    const uint8_t less[local::kMouthBytes] = {local::kMouthToTogether, 9, 8, 7, 6, 5};
    check("told again, it is as it was last told", m.Take(open, sizeof open, 20) && m.Take(less, sizeof less, 20.02) && sent(c, m, mine, 20.03) == "9,8,7,6,5");
    const uint8_t shut[local::kMouthBytes] = {local::kMouthToTogether, 0, 0, 0, 0, 0};
    check("...and shut when it is told shut", m.Take(shut, sizeof shut, 20.04) && sent(c, m, mine, 20.05) == "0,0,0,0,0");

    // a pose that has a mouth of its own keeps it while none was told lately
    proto::Pose own;
    own.eyes[1] = 1.6f, own.mouth[2] = 77;
    const std::vector<uint8_t> theirs = proto::Pack(own);
    check("a pose with a mouth of its own keeps it while none was told lately", sent(c, m, theirs, 30) == "0,0,77,0,0");
    check("...and has the one that was told while it is", m.Take(less, sizeof less, 30) && sent(c, m, theirs, 30.1) == "9,8,7,6,5");

    uint8_t wrong[local::kMouthBytes + 1] = {local::kMouthToTogether, 1, 2, 3, 4, 5, 6};
    bool refused = !m.Take(wrong, sizeof wrong, 30.2) && !m.Take(wrong, sizeof wrong - 2, 30.2) && !m.Take(wrong, 1, 30.2) && !m.Take(wrong, 0, 30.2);
    wrong[0] = proto::kPoseDatagram;
    refused = refused && !m.Take(wrong, local::kMouthBytes, 30.2) && !m.Take(mine.data(), mine.size(), 30.2);
    check("what isn't a mouth isn't taken for one, and changes nothing", refused && sent(c, m, mine, 30.3) == "9,8,7,6,5");
    check("a clock that went back doesn't open it", sent(c, m, mine, 29) == "0,0,0,0,0");
}

// A socket in the abstract namespace, named or not.
static int sock(const char *name) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr;
    if (name && bind(fd, reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(name, &addr)) != 0) std::perror("bind"), ++failed;
    return fd;
}

static std::string name(const char *what) { return "ft_together_localtest_" + std::to_string(getpid()) + "_" + what; }

// The datagram waiting at `fd`, "" when none does.
static std::string next(int fd) {
    char buf[600];
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    return n < 0 ? "" : std::string(buf, size_t(n));
}

static void watchers() {
    local::Watchers w;
    const int from = sock(name("hub").c_str()), a = sock(name("a").c_str()), b = sock(name("b").c_str());
    sockaddr_un at[6];
    socklen_t len[6];
    const int more[4] = {sock(name("c").c_str()), sock(name("d").c_str()), sock(name("e").c_str()), sock(nullptr)};
    const char *const names[5] = {"a", "b", "c", "d", "e"};
    for (int i = 0; i < 5; ++i) len[i] = ft_sock_addr(name(names[i]).c_str(), &at[i]);
    const std::vector<uint8_t> p = pose(3);

    w.Send(from, 2, p.data(), p.size(), 0);
    check("with nobody watching, a pose goes nowhere", next(a).empty() && w.count() == 0);
    check("a program asks to watch, and asking again is the same watcher", w.Add(at[0], len[0], 0) && w.Add(at[0], len[0], 1) && w.count() == 1);
    w.Send(from, 2, p.data(), p.size(), 1);
    const std::string got = next(a);
    check("it gets a pose as 0x81, whose it is, and the pose", got.size() == p.size() + 2 && uint8_t(got[0]) == local::kPoseToWatcher && got[1] == 2 &&
                                                                    got.substr(2) == std::string(p.begin(), p.end()) && next(b).empty());
    check("a second one gets them too", w.Add(at[1], len[1], 1) && (w.Send(from, 0, p.data(), p.size(), 1.5), !next(a).empty() && !next(b).empty()));

    sockaddr_un none{};
    none.sun_family = AF_UNIX;
    check("a program with no name to send to can't watch", !w.Add(none, sizeof(sa_family_t), 2) && w.count() == 2);
    check("four may watch, and a fifth is told no", w.Add(at[2], len[2], 2) && w.Add(at[3], len[3], 2) && !w.Add(at[4], len[4], 2) && w.count() == 4);

    w.Add(at[1], len[1], 6);
    w.Send(from, 1, p.data(), p.size(), 7.5);   // a, c and d last asked more than five seconds ago
    check("one that stopped asking stops being sent to", next(a).empty() && !next(b).empty() && w.count() == 1);
    check("...and there is room for another", w.Add(at[4], len[4], 8) && w.count() == 2);
    close(b);
    w.Send(from, 1, p.data(), p.size(), 8.1);
    check("one that is gone is dropped at once, and the others still get theirs", w.count() == 1 && !next(more[2]).empty());

    // a watcher that doesn't read: nobody waits for it
    w.Add(at[0], len[0], 9);
    size_t missed = 0;
    for (int i = 0; i < 5000; ++i) missed += w.Send(from, 1, p.data(), p.size(), 9 + i * 1e-4), next(more[2]);
    check("a watcher that doesn't read holds nothing up, and still watches", w.count() == 2 && !next(a).empty());
    check("...and how many it missed is known", missed > 4000 && missed < 5000);
    for (int fd : {from, a, more[0], more[1], more[2], more[3]}) close(fd);
}

// The input ft-together passes to the program that gives it to the desktop.
static void inputs() {
    const int from = sock(name("ihub").c_str()), a = sock(name("ia").c_str()), b = sock(name("ib").c_str());
    sockaddr_un at[2];
    const socklen_t len[2] = {ft_sock_addr(name("ia").c_str(), &at[0]), ft_sock_addr(name("ib").c_str(), &at[1])};
    local::Inputs in(from);
    proto::Act move, press;
    move.panel = 9, move.what = proto::kActMove, move.a = 40000, move.b = 7;
    press.panel = 9, press.what = proto::kActButton, press.a = proto::kLeft, press.b = 1;
    const auto is = [](const std::string &got, int person, const proto::Act &act) {
        const std::vector<uint8_t> body = proto::Body(act);
        return got.size() == body.size() + 2 && uint8_t(got[0]) == local::kInputToTaker && uint8_t(got[1]) == person && got.substr(2) == std::string(body.begin(), body.end());
    };

    in.Input(1, move);
    check("with no program to give it to the desktop, input goes nowhere and nothing counts as missed", next(a).empty() && in.missed() == 0);
    check("a program asks for the input, and a second one is told no: each would click", in.Add(at[0], len[0], 0) && in.Add(at[0], len[0], 1) && !in.Add(at[1], len[1], 1));
    in.Input(1, move), in.Input(2, press);
    proto::Act read;
    const std::string first = next(a);
    check("it gets an act as 0x85, whose it is, and the act's ten bytes", is(first, 1, move) && first.size() == 12 &&
                                                                            proto::Read(std::vector<uint8_t>(first.begin() + 2, first.end()), read) && read.a == 40000 && read.b == 7);
    check("...each in the order it came", is(next(a), 2, press) && next(a).empty() && next(b).empty());

    // a program that doesn't read misses some: whatever was held is let go before it gets more
    int sent = 0;
    for (; sent < 5000 && in.missed() == 0; ++sent) in.Input(1, move);
    const size_t missed = in.missed();
    in.Input(1, press);
    check("an act a full queue has no room for is missed, and so is what follows while it stays full", missed == 1 && in.missed() == 2);
    while (!next(a).empty()) {}
    press.b = 0;
    in.Input(2, press);
    proto::Act letGo;
    letGo.what = proto::kActLetGo;
    check("the next one it has room for comes after a let-go from nobody: a missed release leaves nothing down", is(next(a), local::Inputs::kNobody, letGo) && is(next(a), 2, press) &&
                                                                                                                   next(a).empty());
    in.Input(1, move);
    check("...and then they come as before", is(next(a), 1, move) && next(a).empty() && in.missed() == 2);

    in.At(6.5);   // the first program last asked more than five seconds ago
    in.Input(1, move);
    check("a program that stopped asking gets no more, and another may take its place", next(a).empty() && in.Add(at[1], len[1], 6.5) && (in.Input(1, move), is(next(b), 1, move)));
    close(b);
    in.At(7), in.Input(1, move);
    check("one that is gone is nobody's miss", in.missed() == 2 && in.Add(at[0], len[0], 7) && (in.Input(2, press), is(next(a), 2, press)));
    for (int fd : {from, a}) close(fd);
}

// 20 ms of someone's voice: the bytes are whatever, as far as ft-together goes.
static std::vector<uint8_t> voice(uint16_t count, size_t size = 60) {
    const std::vector<uint8_t> opus(size, uint8_t(count));
    return proto::PackVoice(count, opus.data(), size);
}

static void voices() {
    proto::Voice v;
    const std::vector<uint8_t> d = voice(0xabcd, 70);
    check("a voice datagram reads back: its count and its packet", d.size() == 73 && proto::ReadVoice(d.data(), d.size(), v) && v.count == 0xabcd && v.size == 70 &&
                                                                     v.opus == d.data() + 3 && v.opus[69] == 0xcd);
    const std::vector<uint8_t> most = voice(1, proto::kMostVoice), one = voice(2, 1);
    std::vector<uint8_t> over = most, none(d.begin(), d.begin() + 3), other = d;
    over.push_back(0), other[0] = proto::kPoseDatagram;
    check("the longest and the shortest packet read", proto::ReadVoice(most.data(), most.size(), v) && proto::ReadVoice(one.data(), one.size(), v) && v.size == 1);
    check("one with no packet, one too long and another kind don't, and aren't packed either",
          !proto::ReadVoice(none.data(), none.size(), v) && !proto::ReadVoice(over.data(), over.size(), v) && !proto::ReadVoice(other.data(), other.size(), v) &&
              !proto::ReadVoice(nullptr, 0, v) && voice(3, 0).empty() && voice(3, proto::kMostVoice + 1).empty());

    local::VoiceGate g;
    const auto take = [&g](int link, const std::vector<uint8_t> &datagram, double now) { return g.Take(link, datagram.data(), datagram.size(), now); };
    check("a link's voice is passed on in order, with gaps where nothing was said", take(1, voice(500), 0) && take(1, voice(501), 0.02) && take(1, voice(650), 3));
    check("one that comes after a newer one isn't, nor the same again", !take(1, voice(649), 3.01) && !take(1, voice(650), 3.02) && take(1, voice(651), 3.03));
    check("what isn't voice isn't passed on, and each link has its own count", !take(1, over, 3.1) && !take(1, pose(700), 3.1) && take(2, voice(3), 3.1));
    int taken = 0;
    uint16_t count = 1000;
    for (int i = 0; i < 2000; ++i) taken += take(3, voice(count++), 5 + i * 0.0005);   // two thousand in a second
    check("a link that floods has about a hundred and fifty a second passed on",
          taken >= int(local::VoiceGate::kASecond) && taken <= int(local::VoiceGate::kASecond + local::VoiceGate::kAtOnce) + 1);
    taken = 0;
    for (int i = 0; i < 250; ++i) taken += take(4, voice(count++), 7 + i * 0.02);
    check("fifty a second all pass, for as long as they come", taken == 250);
    g.Forget(1);
    check("a link that is gone is forgotten", take(1, voice(2), 20));

    local::Watchers w(local::kVoiceToListener);
    const int from = sock(name("vhub").c_str()), a = sock(name("va").c_str());
    sockaddr_un at;
    const socklen_t len = ft_sock_addr(name("va").c_str(), &at);
    w.Add(at, len, 0);
    w.Send(from, 3, d.data(), d.size(), 0.1);
    const std::string got = next(a);
    check("a listener gets a voice as 0x82, whose it is, and the datagram",
          got.size() == d.size() + 2 && uint8_t(got[0]) == local::kVoiceToListener && got[1] == 3 && got.substr(2) == std::string(d.begin(), d.end()));
    close(from), close(a);
}

static void floor() {
    local::Floor f;
    check("outside a room there is no floor to lay", f.Ask("floor", false).rfind("error", 0) == 0 && f.Ask("floor 1 2 3", false).rfind("error", 0) == 0 && !f.laid());
    check("in a room, none is laid until someone says", f.Ask("floor", true) == "ok none");
    check("what isn't three numbers isn't a floor", f.Ask("floor 1 2", true).rfind("error", 0) == 0 && f.Ask("floor 1 2 3 4", true).rfind("error", 0) == 0 &&
                                                          f.Ask("floor a b c", true).rfind("error", 0) == 0 && f.Ask("floor nan 0 0", true).rfind("error", 0) == 0 &&
                                                          f.Ask("floor 0 0 9", true).rfind("error", 0) == 0 && f.Ask("floor 1e9 0 0", true).rfind("error", 0) == 0 && !f.laid());
    check("the first to say lays it", f.Ask("floor 1.5 -2.25 0.5", true) == "ok 1.500000 -2.250000 0.500000" && f.laid());
    check("...and it stands: a second saying gets the first one's", f.Ask("floor 9 9 1", true) == "ok 1.500000 -2.250000 0.500000" && f.Ask("floor", true) == "ok 1.500000 -2.250000 0.500000");
    f.Lift();
    check("leaving the room lifts it, and the next room's is laid afresh", f.Ask("floor", true) == "ok none" && f.Ask("floor 9 9 1", true) == "ok 9.000000 9.000000 1.000000");
    check("asking outside a room lifts it too", f.Ask("floor", false).rfind("error", 0) == 0 && f.Ask("floor", true) == "ok none");
}

static void mediawire() {
    using Message = std::pair<uint8_t, std::vector<uint8_t>>;
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t> unit(3000, 0x5a);
    local::PutMedia(bytes, local::kMediaSource, 2);
    local::PutMediaUnit(bytes, true, unit.data(), unit.size());
    local::PutMediaRate(bytes, 1400000);
    local::PutMedia(bytes, local::kMediaKey);
    local::PutMedia(bytes, local::kMediaAttention, 2, 1);
    uint8_t kind = 0;
    std::vector<uint8_t> body;
    bool ok = true;
    for (size_t piece : {bytes.size(), size_t(1), size_t(7), size_t(1000)}) {
        local::MediaReader r;
        std::vector<Message> got;
        for (size_t at = 0; at < bytes.size(); at += piece) {
            ok = ok && r.Feed(bytes.data() + at, std::min(piece, bytes.size() - at));
            while (r.Next(kind, body)) got.push_back({kind, body});
        }
        ok = ok && got.size() == 5 && got[0] == Message{'S', {2}} && got[1].first == 'U' && got[1].second.size() == unit.size() + 1 && got[1].second[0] == 1 &&
             std::equal(unit.begin(), unit.end(), got[1].second.begin() + 1) && got[2].first == 'R' && got[2].second.size() == 4 &&
             local::MediaU32(got[2].second.data()) == 1400000 && got[3] == Message{'K', {}} && got[4] == Message{'A', {2, 1}} && r.held() == 0 && !r.broken();
    }
    check("the media socket's messages come out whole, however the socket cuts them up", ok);

    const uint8_t none[4] = {0, 0, 0, 0};
    local::MediaReader r;
    check("a message of no length breaks the reader for good", !r.Feed(none, 4) && r.broken() && !r.Feed(bytes.data(), bytes.size()) && !r.Next(kind, body) && r.held() == 0);
    const std::vector<uint8_t> longest(local::kMostMediaUnit, 1);
    std::vector<uint8_t> big;
    local::PutMediaUnit(big, false, longest.data(), longest.size());
    local::MediaReader r2;
    check("the longest picture there may be passes", r2.Feed(big.data(), big.size()) && r2.Next(kind, body) && kind == 'U' && body.size() == longest.size() + 1 && !r2.broken());
    const uint32_t over = uint32_t(local::kMostMedia + 1);
    const uint8_t head[4] = {uint8_t(over), uint8_t(over >> 8), uint8_t(over >> 16), uint8_t(over >> 24)};
    check("...and one a byte longer is refused before any of it is held", !r2.Feed(head, 4) && r2.broken() && r2.held() == 0);
    std::vector<uint8_t> two;
    local::PutMedia(two, local::kMediaKey);
    two.insert(two.end(), none, none + 4);
    local::MediaReader r3;
    check("what came whole before the rubbish still comes out", r3.Feed(two.data(), two.size()) && r3.Next(kind, body) && kind == 'K' && r3.broken() && !r3.Next(kind, body));

    uint64_t seed = 7;
    const auto roll = [&seed] { return seed = seed * 6364136223846793005ull + 1442695040888963407ull, seed >> 33; };
    ok = true;
    for (int round = 0; round < 300 && ok; ++round) {
        local::MediaReader noisy(4096);
        for (int i = 0; i < 200 && ok; ++i) {
            uint8_t chunk[64];
            const size_t n = 1 + roll() % sizeof chunk;
            for (size_t k = 0; k < n; ++k) chunk[k] = roll() % 4 ? 0 : uint8_t(roll());   // mostly short lengths, so messages do come
            noisy.Feed(chunk, n);
            while (noisy.Next(kind, body)) ok = ok && body.size() < 4096;
            ok = ok && noisy.held() <= 4096 + 4 + sizeof chunk;
        }
    }
    check("noise never makes it hold more than a message", ok);
}

int main() {
    gate();
    count();
    mouth();
    watchers();
    inputs();
    voices();
    floor();
    mediawire();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
