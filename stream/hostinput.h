// Host input (docs/host-input.md): the records a Vibepollo host with Frametop's build sends on
// the control stream (0x3004), and the lease the Frame sends back (0x3005). Header-only and free
// of moonlight-common-c, so stream/test/hostinput-test.cpp checks it on the PC:
//   g++ -std=c++17 -Wall -Wextra -Werror -Istream -o /tmp/hostinput-test stream/test/hostinput-test.cpp && /tmp/hostinput-test
//
// A record is a version (1), then:
//   events (1): kind (1 keyboard, 2 mouse), a little-endian u16 count, then that many evdev
//               events, each type (u16), code (u16) and value (s32), little-endian
//   state (2):  engaged (0 or 1), then the reason it isn't (ASCII)
//   hello (3):  the kinds it lends (bit 1 keyboard, bit 2 mouse), then the host's name
// A lease is engage (version, 1, a u32 sequence, a u16 lease in ms) or release (version, 2).
#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <string>

namespace ftstream::hostinput {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kKeyboard = 1, kMouse = 2;
constexpr int kMaxEvents = 64;
constexpr size_t kMaxReason = 32, kMaxName = 60;
// The Frame renews its lease this often, each for kLeaseMs: the host lets go 1.5 s after the last.
constexpr int64_t kLeaseEveryNs = 500'000'000;
constexpr uint16_t kLeaseMs = 1500;

struct Record {
    enum Type { kNone, kEvents, kState, kHello } type = kNone;
    uint8_t kinds = 0;     // events: the one kind; hello: the bits
    std::string events;    // events: "T C V T C V ...", for the relay's hostev
    bool engaged = false;  // state
    std::string text;      // state: the reason; hello: the name (printable ASCII)
};

inline uint16_t Le16(const unsigned char *p) { return uint16_t(p[0] | p[1] << 8); }

inline std::string Printable(const unsigned char *p, size_t n, size_t limit) {
    std::string out;
    for (size_t i = 0; i < n && i < limit; ++i) out += p[i] >= 0x20 && p[i] <= 0x7e ? char(p[i]) : '?';
    return out;
}

// One 0x3004 payload; kNone for anything else. Events keep only what the relay takes anyway
// (sync, keys and buttons 1..KEY_MAX, relative 0..REL_MAX); a record with any other is dropped.
inline Record Parse(const unsigned char *p, int length) {
    Record r;
    if (!p || length < 3 || p[0] != kVersion) return r;
    const size_t n = size_t(length);
    if (p[1] == 1) {
        if (n < 5 || (p[2] != kKeyboard && p[2] != kMouse)) return r;
        const int count = Le16(p + 3);
        if (count < 1 || count > kMaxEvents || n != 5 + size_t(count) * 8) return r;
        std::string out;
        for (int i = 0; i < count; ++i) {
            const unsigned char *e = p + 5 + i * 8;
            const unsigned type = Le16(e), code = Le16(e + 2);
            const int32_t value = int32_t(uint32_t(e[4]) | uint32_t(e[5]) << 8 | uint32_t(e[6]) << 16 | uint32_t(e[7]) << 24);
            const bool ok = (type == 0 && code == 0 && value == 0) || (type == 1 && code > 0 && code <= 0x2ff && (value == 0 || value == 1)) ||
                            (type == 2 && code <= 0x0f);
            if (!ok) return r;
            if (!out.empty()) out += ' ';
            out += std::to_string(type) + ' ' + std::to_string(code) + ' ' + std::to_string(value);
        }
        r.type = Record::kEvents, r.kinds = p[2], r.events = std::move(out);
    } else if (p[1] == 2) {
        if (p[2] > 1) return r;
        r.type = Record::kState, r.engaged = p[2] == 1, r.text = Printable(p + 3, n - 3, kMaxReason);
    } else if (p[1] == 3) {
        r.type = Record::kHello, r.kinds = uint8_t(p[2] & (kKeyboard | kMouse)), r.text = Printable(p + 3, n - 3, kMaxName);
    }
    return r;
}

// The host's id for the relay (its HOST_ID, [A-Za-z0-9._-]{1,64}), from its address.
inline std::string RelayId(const std::string &host) {
    std::string id;
    for (char c : host.substr(0, 64)) id += (std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '-';
    return id.empty() ? "host" : id;
}

inline std::array<unsigned char, 8> Engage(uint32_t seq, uint16_t ms = kLeaseMs) {
    return {kVersion, 1, (unsigned char)seq, (unsigned char)(seq >> 8), (unsigned char)(seq >> 16), (unsigned char)(seq >> 24),
            (unsigned char)ms, (unsigned char)(ms >> 8)};
}

inline std::array<unsigned char, 2> Release() { return {kVersion, 2}; }

// Whether the Frame should hold the host's input now (docs/host-input.md, "When the host's input
// goes to Frametop"). The escape is the host's to keep: it answers every lease "released escape"
// until a release.
struct Conditions {
    bool switchOn = false;  // "input": true on its layout entry
    bool live = false;      // this stream shows pictures
    bool lock = false;      // and holds the host's input lock
    bool worn = false;      // the pointer helper says the headset is worn (and said so lately)
    bool paused = true;     // Frametop is paused (a VR game), or the relay didn't say lately
};

inline bool Wanted(const Conditions &c) { return c.switchOn && c.live && c.lock && c.worn && !c.paused; }

}  // namespace ftstream::hostinput
