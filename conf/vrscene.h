// What SteamVR's scene app is: none, a VR game on the Frame, or Steam Link's client streaming a
// PC's SteamVR (DEE-346).
//
// On the Frame, Steam Link's client (`vrlink`, app key "vrlink.client") is the one scene app while
// a PC streams, whether or not the PC runs a game. Treating it as a game hid and paused Frametop
// for the whole session. So a Steam Link session counts as a game only while the PC runs one and
// Frametop should give way to it: the input relay decides that (input/pc_game.py, from the PC
// host's answer and the "pc_games" setting) and says so in $XDG_RUNTIME_DIR/frametop-pcgame,
// whose first byte is '1'. The file is rewritten every few seconds while it holds; one older
// than kPcGameStale seconds (a relay that stopped) is ignored.
//
// Header only. ftscene::Classify needs <openvr.h> and an initialized VRApplications();
// the rest is plain C++ so tests can use it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace ftscene {

enum class Kind { None, Game, Link };

constexpr const char *kLinkKey = "vrlink.client";
constexpr double kPcGameStale = 15.0;

inline const char *Name(Kind k) { return k == Kind::None ? "none" : k == Kind::Game ? "game" : "link"; }

// Whether a scene app with this app key, or (when SteamVR doesn't know the key) this process
// name from /proc/PID/comm, is Steam Link's client.
inline bool IsLink(const char *appKey, const char *comm) {
    if (appKey && *appKey) return !std::strcmp(appKey, kLinkKey);
    return comm && !std::strncmp(comm, "vrlink", 6);
}

inline std::string ProcComm(uint32_t pid) {
    char path[64], buf[64] = {};
    std::snprintf(path, sizeof path, "/proc/%u/comm", pid);
    FILE *f = std::fopen(path, "r");
    if (!f) return {};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

inline std::string PcGamePath() {
    const char *run = std::getenv("XDG_RUNTIME_DIR");
    return std::string(run && *run ? run : "/tmp") + "/frametop-pcgame";
}

// The relay's word, read from `path` at wall time `now` (seconds): a fresh file starting with '1'.
inline bool PcGameAt(const std::string &path, double now) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    const double mtime = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    if (now - mtime > kPcGameStale) return false;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    const int c = std::fgetc(f);
    std::fclose(f);
    return c == '1';
}

inline bool PcGame() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return PcGameAt(PcGamePath(), ts.tv_sec + ts.tv_nsec / 1e9);
}

// Whether Frametop gives way as it does for a game: a game on the Frame, or a PC game over Steam
// Link that the relay says to give way to.
inline bool GivesWay(Kind k, bool pcGame) { return k == Kind::Game || (k == Kind::Link && pcGame); }

}  // namespace ftscene

// Include after <openvr.h> to get Classify().
#if defined(_OPENVR_API)
namespace ftscene {
// The scene app now (one IVRApplications call, plus one more when there is a scene).
inline Kind Classify() {
    const uint32_t pid = vr::VRApplications()->GetCurrentSceneProcessId();
    if (!pid) return Kind::None;
    char key[vr::k_unMaxApplicationKeyLength] = {};
    if (vr::VRApplications()->GetApplicationKeyByProcessId(pid, key, sizeof key) != vr::VRApplicationError_None)
        key[0] = 0;
    return IsLink(key, ProcComm(pid).c_str()) ? Kind::Link : Kind::Game;
}
}  // namespace ftscene
#endif
