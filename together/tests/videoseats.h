// What the tests of the shared panels' video sit at (video-test.cpp, quality-test.cpp): several
// Frames in a room on the network made in the test (fakenet.h), each with its video rules
// (together/flow/video.h), a wire for pictures that the test can lose, hold back and refuse, and
// programs that only remember what they were told.
#pragma once

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
    std::map<int, int> rate, rateWords, keys, full, fullWords, pictures, pictureWords;
    std::map<int, bool> watched;
    std::vector<Shown> shown;
    std::vector<std::pair<uint8_t, bool>> decode;   // each word to a program that shows a panel: whether to decode

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
    void Rate(int number, int bitrate) override { rate[number] = bitrate, ++rateWords[number]; }
    void Full(int number, int aSecond) override { full[number] = aSecond, ++fullWords[number]; }
    void Pictures(int number, int aSecond) override { pictures[number] = aSecond, ++pictureWords[number]; }
    void Key(int number) override { ++keys[number]; }
    void Watched(int number, bool w) override { watched[number] = w; }
    void Show(uint8_t panel, bool key, const Unit &unit) override { shown.push_back({panel, key, unit}); }
    void Decode(uint8_t panel, bool d) override { decode.push_back({panel, d}); }

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

static const char *const kScreen = " 1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen";
