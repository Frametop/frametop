// What ft-together does for this Frame's own programs (together/local): which poses it passes
// on, the count it sends its own with, who watches, and where the floor lies in the room.
#include "../../conf/ftsock.h"
#include "../local/floor.h"
#include "../local/posegate.h"
#include "../local/watchers.h"

#include <cstdio>
#include <string>
#include <unistd.h>

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
    std::vector<uint8_t> out;
    const std::vector<uint8_t> mine = pose(555, 0.5f);
    proto::Pose a, b;
    bool ok = c.Stamp(mine.data(), mine.size(), out) && proto::Read(out.data(), out.size(), a) && c.Stamp(mine.data(), mine.size(), out) &&
              proto::Read(out.data(), out.size(), b);
    check("this Frame's own poses go out with ft-together's count, one more each time", ok && a.turn == 1 && b.turn == 2 && a.eyes[0] == 0.5f && b.eyes[1] == 1.6f);
    std::vector<uint8_t> bad = mine;
    bad.push_back(0);
    out.clear();
    check("what isn't a pose doesn't go out", !c.Stamp(bad.data(), bad.size(), out) && !c.Stamp(nullptr, 0, out) && out.empty());
    local::PoseGate g;
    ok = true;
    for (int i = 0; i < 70000 && ok; ++i) ok = c.Stamp(mine.data(), mine.size(), out) && g.Take(1, out.data(), out.size(), i / 30.0);
    check("...and the far end takes every one of seventy thousand, round the count and on", ok);
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
    for (int i = 0; i < 5000; ++i) w.Send(from, 1, p.data(), p.size(), 9 + i * 1e-4), next(more[2]);
    check("a watcher that doesn't read holds nothing up, and still watches", w.count() == 2 && !next(a).empty());
    for (int fd : {from, a, more[0], more[1], more[2], more[3]}) close(fd);
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

int main() {
    gate();
    count();
    watchers();
    floor();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
