// A Frame whose person has a split compute PC, the PC itself, and a room for them
// (pcpanels-test.cpp): on the network made in a test (fakenet.h), beside the seats of
// videoseats.h. The PC is the test's own end of a link to the Frame.
#pragma once

#include "../flow/pcpanels.h"
#include "videoseats.h"

inline const char *const kPcAt = "100.64.0.50";

// The Frame's own programs: what they were last told.
struct Own : flow::Programs {
    std::map<int, int> words;   // how many, by the panel's number
    std::map<int, bool> watched;
    std::vector<Shown> shown;
    std::vector<std::pair<uint8_t, bool>> decode;
    void Rate(int number, int) override { ++words[number]; }
    void Full(int number, int) override { ++words[number]; }
    void Pictures(int number, int) override { ++words[number]; }
    void Key(int number) override { ++words[number]; }
    void Watched(int number, bool w) override { ++words[number], watched[number] = w; }
    void Show(uint8_t panel, bool key, const Unit &unit) override { shown.push_back({panel, key, unit}); }
    void Decode(uint8_t panel, bool d) override { decode.push_back({panel, d}); }
};

// The Frame whose person has a PC: its video, with the PC's panels before the Frame's programs.
struct Desk : flow::Wires {
    Net &net;
    Headset &headset;
    std::map<Headset *, Seat *> &seats;
    Own own;
    flow::PcPanels pc;
    flow::Video video;
    int sent = 0;

    Desk(Net &n, Headset &h, std::map<Headset *, Seat *> &all) : net(n), headset(h), seats(all), pc(*h.s, h, h, own, roll), video(*h.s, *this, pc) {
        h.onWatch = [this](int link, const proto::Watch &w, double now) { video.Watch(link, w, now); };
        h.onDevice = [this](int link, const std::string &at, uint16_t kind, const std::vector<uint8_t> &body, double now) { return pc.Said(link, at, kind, body, video, now); };
    }
    bool Frame(int link, std::vector<uint8_t> bytes, uint32_t) override {
        ++sent;
        const auto far = net.far.find({&headset, link});
        if (far != net.far.end()) seats.at(far->second.frame)->video.Frame(far->second.link, bytes.data(), bytes.size(), net.now);
        return true;
    }
};

// The PC: the test's end of a link to the Frame.
struct Pc {
    Net &net;
    Desk &desk;
    Net::End end;
    size_t read = 0;
    uint32_t count[proto::kPanelNumbers] = {};

    Pc(Net &n, Desk &d, const char *from = kPcAt) : net(n), desk(d), end(n.stranger(d.headset, from)) {}
    void say(const std::vector<uint8_t> &bytes) { net.say(end, bytes); }
    bool calls(const std::string &code, uint16_t version = proto::kVersion) {
        proto::Sender s;
        s.version = version, s.code = code;
        say(proto::Pack(s));
        return !shut();
    }
    bool shut() const { return net.shut.count(end.link) != 0; }
    int refused() { return net.refused(end); }
    // What it was told since it was last asked: "3R2000000 3F30 ...".
    std::string heard() {
        const std::vector<uint8_t> &bytes = net.heard[end.link];
        proto::Parser parser;
        parser.Feed(bytes.data() + read, bytes.size() - read);
        read = bytes.size();
        uint16_t kind = 0;
        std::vector<uint8_t> body;
        std::string out;
        proto::Encode e;
        while (parser.Next(kind, body)) {
            if (kind != proto::kEncode || !proto::Read(body, e)) out += std::string(out.empty() ? "" : " ") + "?" + std::to_string(kind);
            else out += std::string(out.empty() ? "" : " ") + std::to_string(int(e.number)) + char(e.word) + std::to_string(e.value);
        }
        return out;
    }
    // A picture of panel `number`, numbered `n` (its next, when 0). The bytes of the picture.
    Unit unit(int number, bool key, uint32_t n = 0, size_t size = 500) {
        Unit u(size);
        for (uint8_t &b : u) b = uint8_t(roll());
        proto::FrameHead head;
        head.panel = uint8_t(number), head.number = n ? (count[number] = n) : ++count[number], head.key = key;
        const std::vector<uint8_t> bytes = proto::PackFrame(head, u.data(), u.size());
        taken = desk.pc.Frame(end.link, bytes.data(), bytes.size(), desk.video, net.now);
        net.pump();
        return u;
    }
    bool taken = false;   // whether its last picture was the PC's to the Frame
};

struct Home {
    Net net;
    Headset a{net, "100.64.0.1", 47193, "Ann"}, b{net, "100.64.0.2", 47193, "Bo"};
    std::map<Headset *, Seat *> seats;
    Desk desk{net, a, seats};
    Seat sb{net, b, seats};

    Home() { net.devices.insert(kPcAt); }
    bool sit() { return a.ask("open") == "ok" && join(a, b); }
    void tick(double seconds) {
        net.now += seconds;
        desk.pc.Tick(net.now), desk.video.Tick(net.now), sb.video.Tick(net.now);
        net.pump();
    }
    std::string every(const std::string &words) {   // "R1 W0" for each of the PC's numbers
        std::string out;
        for (int number = room::kFramePanels; number < room::kOwnPanels; ++number) {
            std::string one = words;
            for (size_t at = 0; at <= one.size(); at = one.find(' ', at) == std::string::npos ? one.size() + 1 : one.find(' ', at) + 1) one.insert(at, std::to_string(number)), ++at;
            out += (out.empty() ? "" : " ") + one;
        }
        return out;
    }
};
