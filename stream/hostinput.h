// Host input (docs/host-input.md): the records a Vibepollo host with Frametop's build sends on
// the control stream (0x3004), and the lease the Frame sends back (0x3005). Header-only and free
// of moonlight-common-c, so stream/test/hostinput-test.cpp checks it on the PC:
//   g++ -std=c++17 -Wall -Wextra -Werror -Istream -o /tmp/hostinput-test stream/test/hostinput-test.cpp && /tmp/hostinput-test
//
// A record is a version (1), then:
//   events (1): kind (1 keyboard, 2 mouse), a little-endian u16 count, then that many evdev
//               events, each type (u16), code (u16) and value (s32), little-endian. A mouse
//               record carries the mouse buttons (BTN_MOUSE to BTN_TASK) and motion, a keyboard
//               record the other keys.
//   state (2):  engaged (0 or 1), then the reason it isn't (ASCII)
//   hello (3):  the kinds it lends (bit 1 keyboard, bit 2 mouse), then the host's name
// A lease is engage (version, 1, a u32 sequence, a u16 lease in ms), release (version, 2), which
// also lifts the host's escape, or drop (version, 3), which gives the input back but keeps one.
#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace ftstream::hostinput {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kKeyboard = 1, kMouse = 2;
constexpr int kMaxEvents = 64;
constexpr size_t kMaxReason = 32, kMaxName = 60;
// The Frame renews its lease this often, each for kLeaseMs: the host lets go 1.5 s after the last.
constexpr int64_t kLeaseEveryNs = 500'000'000;
constexpr uint16_t kLeaseMs = 1500;
// The pointer helper and the relay are asked every second; an answer older than this is none.
constexpr int64_t kStaleNs = 3'000'000'000;

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
            const bool mouse = p[2] == kMouse, button = code >= 0x110 && code <= 0x117;  // BTN_MOUSE..BTN_TASK
            const bool ok = (type == 0 && code == 0 && value == 0) ||
                            (type == 1 && code > 0 && code <= 0x2ff && button == mouse && (value == 0 || value == 1)) ||
                            (type == 2 && code <= 0x0f && mouse);
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
inline std::array<unsigned char, 2> Drop() { return {kVersion, 3}; }

// An answer from the pointer helper (worn) or the relay (paused), and when it came (0: never).
struct Answer {
    bool yes = false;
    int64_t at = 0;
};

inline bool Fresh(const Answer &a, int64_t now) { return a.at && now - a.at < kStaleNs; }

// What the Frame does about the host's input now (docs/host-input.md, "When the host's input goes
// to Frametop"). Only the person's own "give it back", the switch going off or the headset coming
// off, releases, which lifts the host's escape; anything else drops and keeps it.
enum class Want { kHold, kDrop, kRelease };

inline Want Decide(bool switchOn, bool live, const Answer &worn, const Answer &paused, int64_t now) {
    if (!switchOn) return Want::kRelease;
    if (!live) return Want::kDrop;
    if (Fresh(worn, now) && !worn.yes) return Want::kRelease;
    if (!Fresh(worn, now) || !Fresh(paused, now) || paused.yes) return Want::kDrop;
    return Want::kHold;
}

// The relay's view of the host's devices, from its records: "hostdev" with each hello while
// engaged (every second), "hostev" for events of a kind it has a device for, "hostbye" when the
// host lets go. A message the relay didn't get says hostbye, so it lets go of every key it holds
// rather than miss a key-up; the next hello brings the device back. Not thread-safe.
class Relay {
public:
    using Sender = std::function<bool(const std::string &)>;

    Relay(Sender send) : send_(std::move(send)) {}

    void Start(const std::string &id, const std::string &host) { id_ = id, host_ = host, engaged_ = false, reason_.clear(); }

    // A line to log when the host's state changed, or empty.
    std::string Take(const Record &r) {
        std::string said;
        switch (r.type) {
            case Record::kState:
                if (r.engaged != engaged_ || r.text != reason_)
                    said = r.engaged ? "engaged: its keyboard and mouse are here" : "released: " + r.text;
                engaged_ = r.engaged, reason_ = r.text;
                if (!engaged_) Bye();
                break;
            case Record::kHello:
                if (!engaged_) break;
                for (uint8_t kind : {kKeyboard, kMouse}) {
                    if (r.kinds & kind) {
                        if (send_("hostdev " + id_ + " " + Kind(kind) + " " + (r.text.empty() ? host_ : r.text))) devices_ |= kind;
                        else Bye();
                    } else if (devices_ & kind) {
                        send_("hostbye " + id_ + " " + Kind(kind)), devices_ &= uint8_t(~kind);
                    }
                }
                break;
            case Record::kEvents:
                // Before a hello the relay has no device for them.
                if (!engaged_ || !(devices_ & r.kinds)) break;
                if (send_("hostev " + id_ + " " + Kind(r.kinds) + " " + r.events)) ++events_;
                else Bye();
                break;
            case Record::kNone:
                break;
        }
        return said;
    }

    void Bye() {
        for (uint8_t kind : {kKeyboard, kMouse})
            if (devices_ & kind) send_("hostbye " + id_ + " " + Kind(kind));
        devices_ = 0;
    }

    void Stop() { Bye(), engaged_ = false; }

    bool engaged() const { return engaged_; }
    const std::string &reason() const { return reason_; }
    uint8_t devices() const { return devices_; }
    // Event records sent since the last call.
    int TakeEvents() { return std::exchange(events_, 0); }

private:
    static const char *Kind(uint8_t kind) { return kind == kMouse ? "mouse" : "keyboard"; }

    Sender send_;
    std::string id_, host_, reason_;
    bool engaged_ = false;
    uint8_t devices_ = 0;
    int events_ = 0;
};

}  // namespace ftstream::hostinput
