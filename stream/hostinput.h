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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

// When an answer was given, on NOW's clock: SENT is the wall-clock time the kernel stamped on it
// as it was sent (SO_TIMESTAMPNS), WALL the wall clock as it's read. An answer that waited unread
// (the switch off, the lock let go) keeps its age, so it can't pass for fresh later. A wall clock
// set back in between makes it now.
inline int64_t GivenAt(int64_t now, int64_t wall, int64_t sent) { return wall > sent ? now - (wall - sent) : now; }

// What the Frame does about the host's input now (docs/host-input.md, "When the host's input goes
// to Frametop"). Only the person's own "give it back", the switch going off or the headset coming
// off, releases, which lifts the host's escape; anything else drops and keeps it.
enum class Want { kHold, kDrop, kRelease };

// A test that hasn't been allowed the host's input (TestForbidding) drops it: the wearer may be
// faked, so the headset being worn says nothing about anyone sitting here.
inline Want Decide(bool switchOn, bool live, const Answer &worn, const Answer &paused, bool test, int64_t now) {
    if (!switchOn) return Want::kRelease;
    if (!live) return Want::kDrop;
    if (Fresh(worn, now) && !worn.yes) return Want::kRelease;
    if (test || !Fresh(worn, now) || !Fresh(paused, now) || paused.yes) return Want::kDrop;
    return Want::kHold;
}

// The host's escape outlives the stream that heard it: the host keeps one escape, for every
// client, until a release lifts it, and a drop, a session's end and a lease running out keep it.
// Only a switch off or a headset that came off, as the pointer helper said, sends that release,
// and the stream that heard the escape may not be there to see either. So the escape is kept in
// a marker in the session's runtime dir (EscapeMark) until a release goes, the next stream that
// holds the host's input lock picks it up, and the helper says when the headset last went back on
// ("gaze ? headset on-at"): an off and on since the escape owes a release. A Wi-Fi drop, a crash,
// the relay's pause or a helper that doesn't answer change neither, and the escape stands.
inline std::string EscapeMark(const char *xdg, const std::string &id) {
    return std::string(xdg && *xdg ? xdg : "/tmp") + "/frametop-escape-" + id;
}

// When the escape MARK keeps was heard (CLOCK_BOOTTIME ns), or 0: none, or not a marker of ours.
inline int64_t ReadMark(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    char buf[32];
    struct stat st;
    const ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, buf, sizeof buf - 1) : -1;
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *end;
    const long long at = std::strtoll(buf, &end, 10);
    return end != buf && (*end == 0 || *end == '\n') && at > 0 ? int64_t(at) : 0;
}

// Keep the escape heard at AT (CLOCK_BOOTTIME ns), or with AT 0 forget it.
inline void WriteMark(const std::string &path, int64_t at) {
    if (!at) {
        unlink(path.c_str());
        return;
    }
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    const std::string text = std::to_string(at) + "\n";
    if (write(fd, text.data(), text.size()) < 0) {}
    close(fd);
}

// When the headset last went back on after it was off, from the pointer helper's answer to
// "gaze ? headset on-at" ("ok on worn on-at NS", CLOCK_BOOTTIME), or 0: not since the helper
// started, or a helper that doesn't say.
inline int64_t OnAt(const std::string &reply) {
    const size_t k = reply.find(" on-at ");
    if (k == std::string::npos) return 0;
    const long long at = std::strtoll(reply.c_str() + k + 7, nullptr, 10);
    return at > 0 ? int64_t(at) : 0;
}

// Whether the escape heard at ESCAPEDAT is owed a release: the headset came off and went back on
// at ONAT, after it.
inline bool OffOnSince(int64_t escapedAt, int64_t onAt) { return escapedAt && onAt > escapedAt; }

// Tests that may not take a host's input (docs/host-input.md, "Tests and a faked wearer"): the
// pointer helper can't tell the testbench's faked wearer from a real head. While the testbench
// fakes the wearer it keeps frametop-fake-worn.json in the session's runtime dir, and a
// verification run (verify-frametop launch) keeps verify-frametop.json there, its lease renewed by
// touching it. Such a file stops the leases unless it's JSON with "allow_host_input": true: the
// testbench's for as long as it's there, a run's while its mtime is no more than maxAgeNs old
// (the run lease's 30 minutes, after which verify-frametop lets another run take it over).
struct TestFile {
    const char *name;
    int64_t maxAgeNs;  // 0: however old
};
constexpr TestFile kTestFiles[] = {{"frametop-fake-worn.json", 0}, {"verify-frametop.json", 30 * 60 * 1'000'000'000LL}};

// Where the tests keep them: $XDG_RUNTIME_DIR, and the session's /run/user/UID too when that is a
// nested desktop's (/run/user/UID/frametop): the testbench writes to the session's.
inline std::vector<std::string> TestDirs(const char *xdg, unsigned uid) {
    const std::string session = "/run/user/" + std::to_string(uid);
    std::vector<std::string> dirs{xdg && *xdg ? xdg : session};
    if (dirs[0].rfind(session + "/", 0) == 0) dirs.push_back(session);
    return dirs;
}

// Whether the test file at PATH stops the leases at wall-clock time NOW, in ns (allows: whether its
// text says "allow_host_input": true). Only a missing or a stale file doesn't: one that is garbled,
// too big, not a plain file or can't be read does.
inline bool Forbids(const std::string &path, int64_t maxAgeNs, int64_t now,
                    const std::function<bool(const std::string &)> &allows) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno != ENOENT;
    struct stat st;
    char buf[4097];
    ssize_t n = -1;
    const bool plain = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (plain) n = read(fd, buf, sizeof buf);
    close(fd);
    if (plain && maxAgeNs && now - (int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec) > maxAgeNs)
        return false;
    return n < 0 || size_t(n) == sizeof buf || !allows(std::string(buf, size_t(n)));
}

// The first test file in DIRS that stops the leases, or "".
inline std::string TestForbidding(const std::vector<std::string> &dirs, int64_t now,
                                  const std::function<bool(const std::string &)> &allows) {
    for (const std::string &dir : dirs)
        for (const TestFile &f : kTestFiles)
            if (const std::string path = dir + "/" + f.name; Forbids(path, f.maxAgeNs, now, allows)) return path;
    return "";
}

// The pointer helper's last answer once the test files say TEST, after the last tick found SAID.
// A test's file going away means none: the answer may be from while that test faked the wearer,
// and it would hold the lease until the next one. Tick also drops the answers still waiting.
inline Answer AfterTest(const Answer &worn, const std::string &said, const std::string &test) {
    return test.empty() && !said.empty() ? Answer{} : worn;
}

// The relay's view of the host's devices, from its records: "hostdev" with each hello while
// engaged (every second), "hostev" for events of a kind it has a device for, "hostbye" when the
// host lets go. A message the relay didn't get says hostbye, so it lets go of every key it holds
// rather than miss a key-up. A hostbye it didn't get either is owed: the kind gets no hostdev
// (which would keep its held keys alive) until the hostbye has gone, and with no word the relay
// lets go of it by itself. The next hello brings the device back. Not thread-safe.
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
                PayOwed();
                if (!engaged_) break;
                for (uint8_t kind : {kKeyboard, kMouse}) {
                    if (owed_ & kind) continue;
                    if (r.kinds & kind) {
                        if (send_("hostdev " + id_ + " " + Kind(kind) + " " + (r.text.empty() ? host_ : r.text))) devices_ |= kind;
                        else Bye();
                    } else if (devices_ & kind) {
                        devices_ &= uint8_t(~kind);
                        if (!send_("hostbye " + id_ + " " + Kind(kind))) owed_ |= kind;
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
        owed_ |= devices_;
        devices_ = 0;
        PayOwed();
    }

    void Stop() { Bye(), engaged_ = false; }

    bool engaged() const { return engaged_; }
    const std::string &reason() const { return reason_; }
    uint8_t devices() const { return devices_; }
    uint8_t owed() const { return owed_; }
    // Event records sent since the last call.
    int TakeEvents() { return std::exchange(events_, 0); }

private:
    static const char *Kind(uint8_t kind) { return kind == kMouse ? "mouse" : "keyboard"; }

    void PayOwed() {
        for (uint8_t kind : {kKeyboard, kMouse})
            if ((owed_ & kind) && send_("hostbye " + id_ + " " + Kind(kind))) owed_ &= uint8_t(~kind);
    }

    Sender send_;
    std::string id_, host_, reason_;
    bool engaged_ = false;
    uint8_t devices_ = 0, owed_ = 0;
    int events_ = 0;
};

}  // namespace ftstream::hostinput
