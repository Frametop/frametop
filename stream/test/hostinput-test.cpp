// stream/hostinput.h, offline: the host's records, the relay id, the lease and when to hold it.
//   g++ -std=c++17 -Wall -Wextra -Werror -Istream -o /tmp/hostinput-test stream/test/hostinput-test.cpp && /tmp/hostinput-test
#undef NDEBUG
#include "hostinput.h"

#include <cassert>
#include <cstdio>
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

    // When to hold it.
    Conditions c{true, true, true, true, false};
    assert(Wanted(c));
    for (bool Conditions::*off : {&Conditions::switchOn, &Conditions::live, &Conditions::lock, &Conditions::worn}) {
        Conditions d = c;
        d.*off = false;
        assert(!Wanted(d));
    }
    c.paused = true;
    assert(!Wanted(c));
    assert(!Wanted(Conditions{}));

    std::puts("hostinput-test: ok");
}
