// Offline replay of the HostInputLease model's paths against ft-stream's real HostInput (copied
// verbatim by extract.py, socket names swapped). Plays the pointer helper and the relay on test
// sockets, records every lease ft-stream sends, and plays the host's side as docs/host-input.md says.
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <time.h>
#include <vector>
#include <nlohmann/json.hpp>
#include "ftsock.h"
#include "hostinput.h"
using namespace ftstream;

static bool g_switch = true;
static std::vector<std::string> g_sent;  // leases ft-stream sent
struct Host { bool engaged = false, esc = false; } g_hostside;
int LiSendFrametopHostInputLease(const unsigned char *p, int n) {
    const char *k = n >= 2 && p[1] == 1 ? "engage" : n >= 2 && p[1] == 2 ? "release" : "drop";
    g_sent.push_back(k);
    if (p[1] == 1) g_hostside.engaged = !g_hostside.esc;
    else if (p[1] == 2) g_hostside.engaged = false, g_hostside.esc = false;
    else g_hostside.engaged = false;
    return 0;
}
bool HostFlag(const std::string &, const char *key) { return !strcmp(key, "input") && g_switch; }
bool AllowsHostInput(const std::string &) { return false; }
#include "hostin_struct.inc"

static std::string TAG;
static int Bind(const std::string &name) {
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{}; a.sun_family = AF_UNIX; memcpy(a.sun_path + 1, name.data(), name.size());
    if (bind(s, (sockaddr *)&a, socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size())) < 0) { perror("bind"); _exit(2); }
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
// The fake helper and relay answer whatever was asked: the helper with the headset as it is now,
// and, asked "on-at", when it last went back on after an off of 1.5 s or more (CLOCK_BOOTTIME).
static int helperSock, relaySock;
static int64_t Boot() { timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return int64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec; }
static int64_t g_offAt, g_onAt;
static void Wear(bool &worn, bool on) {
    if (!on && worn) g_offAt = Boot();
    if (on && !worn && Boot() - g_offAt >= 1'500'000'000) g_onAt = Boot();
    worn = on;
}
static int Answer(bool worn) {
    int n = 0;
    for (int s : {helperSock, relaySock}) {
        char buf[512]; sockaddr_un from{}; socklen_t fl = sizeof from;
        for (ssize_t r; (r = recvfrom(s, buf, sizeof buf, 0, (sockaddr *)&from, &fl)) > 0; fl = sizeof from) {
            std::string q(buf, size_t(r)), a;
            if (s == helperSock && q.rfind("gaze ? headset", 0) == 0)
                a = std::string(worn ? "ok on worn" : "ok on away") + (q.find("on-at") != std::string::npos ? " on-at " + std::to_string(g_onAt) : "");
            else if (s == relaySock && q == "pause ?") a = R"({"t":"pause","paused":false})";
            else continue;
            sendto(s, a.data(), a.size(), 0, (sockaddr *)&from, fl), ++n;
        }
    }
    return n;
}
static void Say(const char *what) {
    std::string s; for (auto &x : g_sent) s += x + " ";
    std::printf("  %-58s sent: [%s] host engaged=%d escaped=%d\n", what, s.c_str(), g_hostside.engaged, g_hostside.esc);
    g_sent.clear();
}
static constexpr int64_t S = 1'000'000'000;
// Steps happen at their real times: the answers carry the kernel's send time (SO_TIMESTAMPNS), so
// a "3 s pass" step has to take 3 s. AT(t): sleep until t after the scenario's start, then the time.
static int64_t g_t0;
static int64_t Mono() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return int64_t(t.tv_sec) * S + t.tv_nsec; }
static int64_t At(int64_t t) {
    for (int64_t left; (left = g_t0 + t - Mono()) > 0;) { timespec d{time_t(left / S), long(left % S)}; nanosleep(&d, nullptr); }
    return Mono();
}
// What the process's exit does to the lock and the socket.
static void Exit(HostInput &h) { h.Unlock(); if (h.lock >= 0) close(h.lock); if (h.sock >= 0) close(h.sock); }

// Each scenario starts with no escape kept from the last one, and a helper that has seen no off.
static void Fresh() { unlink(hostinput::EscapeMark(getenv("XDG_RUNTIME_DIR"), hostinput::RelayId("192.0.2.7")).c_str()); g_offAt = g_onAt = 0; }

static bool StaleLease() {
    std::printf("NoStaleLease, TLC's shortest path (11 steps), one step per action\n");
    g_switch = true, g_hostside = {}, g_t0 = Mono(), Fresh();
    bool worn = true;
    HostInput h; h.Open("192.0.2.7");
    h.Tick(At(0), false);                               Say("1 a stream starts (reads the switch: on)");
    g_switch = false;                               Say("2 you turn the switch off");
    h.Tick(At(S / 10), true);                           Say("3+4 the stream goes live; ft-stream takes the lock and asks");
    Answer(worn);                                   Say("5 the pointer helper answers \"worn\"");
    Wear(worn, false);                              Say("6 you take the headset off");
    h.Tick(At(3 * S + S / 10), true);                   Say("7 ft-stream sees the switch off (releases, unlocks)");
    h.Tick(At(6 * S), true);                            Say("8 3 s pass (ft-stream ticks, switch still off)");
    g_switch = true;                                Say("9 you turn the switch on");
    h.Tick(At(9 * S + S / 5), true);                    Say("10+11 ft-stream sees the switch on, locks, asks, reads the waiting answer");
    const bool bug = g_hostside.engaged && !worn;
    Answer(worn); h.Tick(At(9 * S + S / 4), true);      Say("(after) the helper's answer to the new ask (away) is read");
    Exit(h);
    std::printf("  => %s\n\n", bug ? "REPRODUCED: an engage went with nobody wearing the headset, on a 9 s old \"worn\" answer"
                                   : "not reproduced");
    return bug;
}

static bool EscapeKept() {
    std::printf("OffOnLiftsEscape, TLC's shortest path (15 steps), one step per action\n");
    g_switch = true, g_hostside = {}, g_t0 = Mono(), Fresh();
    bool worn = true;
    {
        HostInput h; h.Open("192.0.2.7");
        h.Tick(At(0), false);                           Say("1 a stream starts");
        h.Tick(At(S / 10), true);                       Say("2+3 it goes live; ft-stream takes the lock and asks");
        Answer(worn);                               Say("4 the pointer helper answers \"worn\"");
        Wear(worn, false);                          Say("5 you take the headset off");
        h.Tick(At(S / 10 + S / 20), true);              Say("6 ft-stream reads the answer and leases");
        g_hostside.engaged = false, g_hostside.esc = true;
        const unsigned char rec[] = {1, 2, 0, 'e', 's', 'c', 'a', 'p', 'e'};
        h.Got(rec, sizeof rec);                     Say("7 someone at the computer presses the escape hotkey");
        h.Stop(hostinput::Want::kDrop);             Say("8 the stream ends (ft-stream's exit: Stop(kDrop))");
        Exit(h);
    }
    HostInput h; h.Open("192.0.2.7");
    h.Tick(At(3 * S), false);                           Say("9 a stream starts (headset off)");
    h.Tick(At(6 * S), false);                           Say("10 3 s pass (still connecting)");
    Wear(worn, true);                               Say("11 you put the headset on");
    h.Tick(At(6 * S + S / 10), true);                   Say("12+13 it goes live; ft-stream takes the lock and asks");
    Answer(worn);                                   Say("14 the pointer helper answers \"worn\"");
    h.Tick(At(6 * S + S / 5), true);                    Say("15 ft-stream reads the answer and leases");
    const bool bug = g_hostside.esc && h.leasing;
    Exit(h);
    std::printf("  => %s\n\n", bug ? "REPRODUCED: no release went across the off-on, only a drop and then engages"
                                   : "not reproduced");
    return bug;
}

// The control: the same two streams with the headset on throughout (the first one ended on a
// Wi-Fi drop). Nothing came off, so the escape must stand: no release, only engages it refuses.
static bool WifiLiftsEscape() {
    std::printf("H2-control: a Wi-Fi drop with the headset on keeps the escape\n");
    g_switch = true, g_hostside = {}, g_t0 = Mono(), Fresh();
    bool worn = true;
    bool released = false;
    {
        HostInput h; h.Open("192.0.2.7");
        h.Tick(At(0), false);                           Say("1 a stream starts");
        h.Tick(At(S / 10), true);                       Say("2+3 it goes live; ft-stream takes the lock and asks");
        Answer(worn); h.Tick(At(S / 10 + S / 20), true); Say("4+5 the helper answers \"worn\"; ft-stream leases");
        g_hostside.engaged = false, g_hostside.esc = true;
        const unsigned char rec[] = {1, 2, 0, 'e', 's', 'c', 'a', 'p', 'e'};
        h.Got(rec, sizeof rec);                     Say("6 someone at the computer presses the escape hotkey");
        h.Stop(hostinput::Want::kDrop);             Say("7 the Wi-Fi drops and the stream ends (Stop(kDrop))");
        Exit(h);
    }
    HostInput h; h.Open("192.0.2.7");
    h.Tick(At(2 * S), false);                           Say("8 ft-screens starts it again");
    for (int64_t t = 2 * S + S / 10; t < 5 * S; t += S / 20) {
        Answer(worn), h.Tick(At(t), true);
        for (const std::string &x : g_sent) released |= x == "release";
    }
    Say("9 it goes live and ticks for 3 s, the helper saying \"worn\"");
    released |= !g_hostside.esc;
    Exit(h);
    std::printf("  => %s\n\n", released ? "REPRODUCED: a release lifted the escape with the headset on all along"
                                        : "not reproduced");
    return released;
}

int main() {
    TAG = "@TAG@";
    char dir[] = "@WORK@/runXXXXXX";
    setenv("XDG_RUNTIME_DIR", mkdtemp(dir), 1);
    helperSock = Bind("fttest_helper_" + TAG), relaySock = Bind("fttest_relay_" + TAG);
    const bool a = StaleLease(), b = EscapeKept(), c = WifiLiftsEscape();
    return a && b && c ? 0 : 1;
}
