// stream/hostinput.h, offline: the host's records, the relay id, the lease, when to hold it, the
// tests that stop it, and what the relay is told.
//   g++ -std=c++17 -Wall -Wextra -Werror -Istream -o /tmp/hostinput-test stream/test/hostinput-test.cpp && /tmp/hostinput-test
#undef NDEBUG
#include "hostinput.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

using namespace ftstream::hostinput;

namespace {

std::vector<unsigned char> Bytes(std::initializer_list<int> values) {
    std::vector<unsigned char> out;
    for (int v : values) out.push_back((unsigned char)v);
    return out;
}

Record P(const std::vector<unsigned char> &b) { return Parse(b.data(), int(b.size())); }

void Event(std::vector<unsigned char> &b, int type, int code, int32_t value) {
    const uint32_t v = uint32_t(value);
    for (int x : {type & 0xff, type >> 8, code & 0xff, code >> 8, int(v & 0xff), int(v >> 8 & 0xff), int(v >> 16 & 0xff), int(v >> 24)})
        b.push_back((unsigned char)x);
}

std::vector<unsigned char> Events(int kind, const std::vector<std::array<int, 3>> &events) {
    auto b = Bytes({kVersion, 1, kind, int(events.size() & 0xff), int(events.size() >> 8)});
    for (const auto &e : events) Event(b, e[0], e[1], e[2]);
    return b;
}

}  // namespace

int main() {
    // Events, as the relay's hostev wants them.
    Record r = P(Events(kMouse, {{2, 0, -5}, {1, 272, 1}, {0, 0, 0}}));
    assert(r.type == Record::kEvents && r.kinds == kMouse);
    assert(r.events == "2 0 -5 1 272 1 0 0 0");
    r = P(Events(kKeyboard, {{1, 30, 0}, {0, 0, 0}}));
    assert(r.type == Record::kEvents && r.kinds == kKeyboard && r.events == "1 30 0 0 0 0");
    // Anything the relay wouldn't take drops the whole record.
    assert(P(Events(kKeyboard, {{1, 30, 2}})).type == Record::kNone);   // a repeat
    assert(P(Events(kKeyboard, {{1, 0x300, 1}})).type == Record::kNone);  // past KEY_MAX
    assert(P(Events(kKeyboard, {{3, 0, 10}})).type == Record::kNone);   // absolute
    assert(P(Events(kKeyboard, {{4, 4, 30}})).type == Record::kNone);   // a scan code
    assert(P(Events(kMouse, {{2, 16, 1}})).type == Record::kNone);      // past REL_MAX
    // Each kind carries only its own.
    assert(P(Events(kKeyboard, {{1, 272, 1}})).type == Record::kNone);  // BTN_LEFT
    assert(P(Events(kKeyboard, {{1, 279, 1}})).type == Record::kNone);  // BTN_TASK
    assert(P(Events(kKeyboard, {{2, 0, 1}})).type == Record::kNone);
    assert(P(Events(kMouse, {{1, 30, 1}})).type == Record::kNone);
    assert(P(Events(kMouse, {{1, 271, 1}})).type == Record::kNone);
    assert(P(Events(kMouse, {{1, 280, 1}})).type == Record::kNone);
    assert(P(Events(kMouse, {{1, 279, 1}, {2, 8, -1}})).type == Record::kEvents);
    assert(P(Events(kKeyboard, {{1, 271, 1}, {1, 280, 0}})).type == Record::kEvents);
    assert(P(Events(3, {{1, 30, 1}})).type == Record::kNone);           // no such kind
    assert(P(Events(kMouse, {})).type == Record::kNone);
    auto shortened = Events(kMouse, {{2, 0, 1}});
    shortened.pop_back();
    assert(P(shortened).type == Record::kNone);
    auto longer = Events(kMouse, {{2, 0, 1}});
    longer.push_back(0);
    assert(P(longer).type == Record::kNone);
    std::vector<std::array<int, 3>> many(kMaxEvents, {2, 0, 1});
    assert(P(Events(kMouse, many)).type == Record::kEvents);
    many.push_back({0, 0, 0});
    assert(P(Events(kMouse, many)).type == Record::kNone);

    // State and hello.
    r = P(Bytes({kVersion, 2, 1}));
    assert(r.type == Record::kState && r.engaged && r.text.empty());
    r = P(Bytes({kVersion, 2, 0, 'e', 's', 'c', 'a', 'p', 'e'}));
    assert(r.type == Record::kState && !r.engaged && r.text == "escape");
    assert(P(Bytes({kVersion, 2, 2})).type == Record::kNone);
    r = P(Bytes({kVersion, 3, 3, 'o', 'm', 'a', '7', '\n'}));
    assert(r.type == Record::kHello && r.kinds == (kKeyboard | kMouse) && r.text == "oma7?");
    assert(P(Bytes({kVersion, 3, 0xff})).kinds == (kKeyboard | kMouse));
    std::vector<unsigned char> longReason = Bytes({kVersion, 2, 0}), longName = Bytes({kVersion, 3, 1});
    longReason.insert(longReason.end(), 40, 'a'), longName.insert(longName.end(), 80, 'n');
    assert(P(longReason).text.size() == kMaxReason && P(longName).text.size() == kMaxName);
    assert(P(Bytes({2, 2, 1})).type == Record::kNone);  // another version
    assert(P(Bytes({kVersion, 9, 1})).type == Record::kNone);
    assert(P(Bytes({kVersion, 2})).type == Record::kNone);
    assert(Parse(nullptr, 3).type == Record::kNone);

    // The relay id.
    assert(RelayId("192.168.1.50") == "192.168.1.50");
    assert(RelayId("fe80::1%wlan0") == "fe80--1-wlan0");
    assert(RelayId("") == "host");
    assert(RelayId(std::string(80, 'a')).size() == 64);

    // The lease.
    assert((Engage(0x01020304) == std::array<unsigned char, 8>{kVersion, 1, 4, 3, 2, 1, 0xdc, 0x05}));
    assert((Release() == std::array<unsigned char, 2>{kVersion, 2}));
    assert((Drop() == std::array<unsigned char, 2>{kVersion, 3}));

    // When to hold it, and how to give it back.
    const int64_t now = 100'000'000'000;
    const Answer worn{true, now - 1}, away{false, now - 1}, playing{false, now - 1}, paused{true, now - 1};
    assert(Decide(true, true, worn, playing, false, now) == Want::kHold);
    assert(Decide(false, true, worn, playing, false, now) == Want::kRelease);   // the switch went off
    assert(Decide(true, true, away, playing, false, now) == Want::kRelease);    // the headset came off
    assert(Decide(false, false, Answer{}, Answer{}, false, now) == Want::kRelease);
    assert(Decide(true, false, worn, playing, false, now) == Want::kDrop);      // the stream isn't live
    assert(Decide(true, true, worn, paused, false, now) == Want::kDrop);        // a VR game
    assert(Decide(true, false, away, playing, false, now) == Want::kDrop);
    // Answers that stopped coming: not the person's say, so drop.
    const Answer staleWorn{true, now - kStaleNs}, staleAway{false, now - kStaleNs}, stalePlaying{false, now - kStaleNs};
    assert(Decide(true, true, staleWorn, playing, false, now) == Want::kDrop);
    assert(Decide(true, true, staleAway, playing, false, now) == Want::kDrop);
    assert(Decide(true, true, worn, stalePlaying, false, now) == Want::kDrop);
    assert(Decide(true, true, Answer{}, Answer{}, false, now) == Want::kDrop);
    assert(Decide(true, true, Answer{true, now - kStaleNs + 1}, playing, false, now) == Want::kHold);
    // A test that may not take the host's input drops it, but the headset coming off still releases.
    assert(Decide(true, true, worn, playing, true, now) == Want::kDrop);
    assert(Decide(true, true, away, playing, true, now) == Want::kRelease);
    assert(Decide(false, true, worn, playing, true, now) == Want::kRelease);
    // Its file going away: the last answer may be from while it faked the wearer, so none until
    // the helper answers again.
    const std::string fakeWorn = "/run/user/1000/frametop-fake-worn.json";
    assert(Decide(true, true, AfterTest(worn, fakeWorn, ""), playing, false, now) == Want::kDrop);
    assert(AfterTest(worn, "", "").at == worn.at && AfterTest(worn, fakeWorn, fakeWorn).at == worn.at);
    assert(Decide(true, true, AfterTest(worn, "", ""), playing, false, now) == Want::kHold);

    // Where the tests keep their files.
    assert((TestDirs("/run/user/1000", 1000) == std::vector<std::string>{"/run/user/1000"}));
    assert((TestDirs("/run/user/1000/frametop", 1000) == std::vector<std::string>{"/run/user/1000/frametop", "/run/user/1000"}));
    assert((TestDirs(nullptr, 1000) == std::vector<std::string>{"/run/user/1000"}));
    assert((TestDirs("", 1000) == std::vector<std::string>{"/run/user/1000"}));
    assert((TestDirs("/tmp/xdg", 1000) == std::vector<std::string>{"/tmp/xdg"}));
    assert((TestDirs("/run/user/10000", 1000) == std::vector<std::string>{"/run/user/10000"}));

    // Whether they stop the leases. The parser here stands in for ft-stream's JSON one.
    char tmpl[] = "/tmp/hostinput-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl), nested = dir + "/frametop";
    assert(mkdir(nested.c_str(), 0700) == 0);
    const auto allows = [](const std::string &text) { return text == "allowed"; };
    const std::string flag = dir + "/frametop-fake-worn.json";
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t wall = int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    const auto put = [](const std::string &path, const std::string &text) {
        FILE *f = std::fopen(path.c_str(), "w");
        assert(f && std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fclose(f) == 0);
    };
    assert(TestForbidding({dir}, wall, allows).empty());  // no test
    put(flag, "allowed");
    assert(TestForbidding({dir}, wall, allows).empty());
    put(flag, "not allowed");
    assert(TestForbidding({dir}, wall, allows) == flag);
    put(flag, "");  // garbled, or caught mid-write
    assert(TestForbidding({dir}, wall, allows) == flag);
    put(flag, std::string(4096, ' '));
    assert(!Forbids(flag, 0, wall, [](const std::string &) { return true; }));
    put(flag, std::string(4097, ' '));  // too big to be the testbench's
    assert(Forbids(flag, 0, wall, [](const std::string &) { return true; }));
    assert(unlink(flag.c_str()) == 0 && mkdir(flag.c_str(), 0700) == 0);  // not a plain file
    assert(TestForbidding({dir}, wall, allows) == flag);
    assert(rmdir(flag.c_str()) == 0 && symlink("/dev/null", flag.c_str()) == 0);  // not followed
    assert(TestForbidding({dir}, wall, allows) == flag);
    assert(unlink(flag.c_str()) == 0);
    // The session's dir counts from a nested desktop.
    put(flag, "not allowed");
    assert(TestForbidding({nested}, wall, allows).empty());
    assert(TestForbidding({nested, dir}, wall, allows) == flag);
    assert(unlink(flag.c_str()) == 0);
    // A verification run's holds for 30 minutes from when it was last touched; the testbench's for good.
    const std::string run = dir + "/verify-frametop.json";
    const auto age = [&](const std::string &path, int64_t seconds) {
        const timespec t[2] = {{ts.tv_sec - seconds, ts.tv_nsec}, {ts.tv_sec - seconds, ts.tv_nsec}};
        assert(utimensat(AT_FDCWD, path.c_str(), t, 0) == 0);
    };
    put(run, "not allowed");
    assert(TestForbidding({dir}, wall, allows) == run);
    age(run, 30 * 60);
    assert(TestForbidding({dir}, wall, allows) == run);
    age(run, 30 * 60 + 1);
    assert(TestForbidding({dir}, wall, allows).empty());
    put(run, "allowed");
    assert(TestForbidding({dir}, wall, allows).empty());
    put(run, "");  // a launch between creating it and writing it
    assert(TestForbidding({dir}, wall, allows) == run);
    put(flag, "not allowed");
    age(flag, 24 * 60 * 60);
    assert(TestForbidding({dir}, wall, allows) == flag);  // the first that stops them
    assert(unlink(flag.c_str()) == 0 && unlink(run.c_str()) == 0);
    assert(rmdir(nested.c_str()) == 0 && rmdir(dir.c_str()) == 0);

    // The relay.
    std::vector<std::string> said;
    bool up = true;
    Relay relay([&](const std::string &msg) {
        if (up) said.push_back(msg);
        return up;
    });
    relay.Start("oma7", "192.168.1.50");
    const Record hello = P(Bytes({kVersion, 3, 3, 'o', 'm', 'a', '7'})), on = P(Bytes({kVersion, 2, 1})),
                 key = P(Events(kKeyboard, {{1, 30, 1}, {0, 0, 0}})), move = P(Events(kMouse, {{2, 0, 1}}));
    assert(relay.Take(hello).empty() && said.empty());  // not engaged yet: nothing to say
    assert(relay.Take(key).empty() && said.empty());
    assert(relay.Take(on) == "engaged: its keyboard and mouse are here");
    assert(relay.Take(key).empty() && said.empty());  // before a hello the relay has no device
    relay.Take(hello);
    assert((said == std::vector<std::string>{"hostdev oma7 keyboard oma7", "hostdev oma7 mouse oma7"}));
    said.clear();
    relay.Take(key), relay.Take(move);
    assert((said == std::vector<std::string>{"hostev oma7 keyboard 1 30 1 0 0 0", "hostev oma7 mouse 2 0 1"}));
    assert(relay.TakeEvents() == 2 && relay.TakeEvents() == 0);
    up = false;
    relay.Take(key);  // not sent: not counted
    up = true;
    assert(relay.TakeEvents() == 0);
    relay.Take(hello);
    said.clear();
    assert(relay.Take(on).empty());  // told every second: logged once
    said.clear();
    // A hello without the mouse takes it away; one with no name uses the host's address.
    relay.Take(P(Bytes({kVersion, 3, 1})));
    assert((said == std::vector<std::string>{"hostdev oma7 keyboard 192.168.1.50", "hostbye oma7 mouse"}));
    said.clear();
    relay.Take(move);
    assert(said.empty());
    // A lost event says hostbye, and the next hello brings the devices back.
    relay.Take(hello);
    said.clear();
    up = false;
    relay.Take(key);
    up = true;
    assert(relay.devices() == 0);
    relay.Take(key);
    assert(said.empty());
    relay.Take(hello);
    assert(relay.devices() == (kKeyboard | kMouse));
    said.clear();
    // A hostbye that couldn't go either is owed: no hostdev for that kind until it has gone.
    said.clear();
    up = false;
    relay.Take(key);  // hostev fails, then both hostbyes
    up = true;
    assert(relay.owed() == (kKeyboard | kMouse) && relay.devices() == 0);
    relay.Take(hello);  // pays them first, then the devices come back
    assert((said == std::vector<std::string>{"hostbye oma7 keyboard", "hostbye oma7 mouse", "hostdev oma7 keyboard oma7",
                                             "hostdev oma7 mouse oma7"}));
    assert(relay.owed() == 0);
    said.clear();
    // Still owed at the hello: that kind waits.
    int failures = 0;
    Relay flaky([&](const std::string &msg) {
        if (msg.rfind("hostbye oma7 keyboard", 0) == 0 && failures++ < 2) return false;
        said.push_back(msg);
        return true;
    });
    flaky.Start("oma7", "192.168.1.50");
    flaky.Take(on), flaky.Take(hello);
    said.clear();
    flaky.Stop();  // the keyboard's hostbye fails, the mouse's goes
    assert((said == std::vector<std::string>{"hostbye oma7 mouse"}));
    said.clear();
    flaky.Take(on);
    flaky.Take(hello);  // and fails again: no keyboard hostdev
    assert((said == std::vector<std::string>{"hostdev oma7 mouse oma7"}) && flaky.owed() == kKeyboard);
    said.clear();
    flaky.Take(hello);
    assert((said == std::vector<std::string>{"hostbye oma7 keyboard", "hostdev oma7 keyboard oma7", "hostdev oma7 mouse oma7"}));
    said.clear();

    // Released: hostbye for each, and events stop.
    assert(relay.Take(P(Bytes({kVersion, 2, 0, 'e', 's', 'c', 'a', 'p', 'e'}))) == "released: escape");
    assert((said == std::vector<std::string>{"hostbye oma7 keyboard", "hostbye oma7 mouse"}));
    assert(relay.reason() == "escape" && !relay.engaged());
    said.clear();
    relay.Take(key), relay.Take(hello);
    assert(said.empty());
    // Stopping from the Frame's side says hostbye too.
    relay.Take(on), relay.Take(hello);
    said.clear();
    relay.Stop();
    assert((said == std::vector<std::string>{"hostbye oma7 keyboard", "hostbye oma7 mouse"}) && !relay.engaged());

    std::puts("hostinput-test: ok");
}
