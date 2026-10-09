// Room View (SteamVR's passthrough): on or off. With it on, the hand cutouts (handcut.h) show
// the room through Frametop's panels; with it off, a cutout would show the void or the game
// behind them, so ft-screens doesn't cut, and ft-handview (handview.cpp) draws the hands instead.
//
// SteamVR has no public "is Room View showing". What a program can read:
//   - VREvent_RoomViewShown / VREvent_RoomViewHidden (526, 527): the compositor's, "for scene
//     apps only - not for construct or transient bounds" (openvr.h). Edges, during a VR game.
//   - The passthrough shared state, VR_CameraPassthroughState: /dev/shm/u<uid>-Shm_*, 0x6200
//     bytes, its config at +2..+6: enabled, stereo, rgbSource, disableDevignette, sharpen
//     (frame-hands/notes/passthrough-internal-re.md). Read only, never through the private
//     interface. On 2026-10-09 it read enabled 1 (stereo 1, rgb 1, sharpen 1) with the headset
//     off; whether `enabled` follows the dashboard's Room View button isn't known yet, so only
//     its 0 counts (passthrough off altogether).
//   - With no VR game (no scene app), SteamVR shows the construct, where Room View is on when
//     camera/enableConstructRoomView is (on, by default on the Frame).
// So: a forced setting first (HANDS_ROOMVIEW=on|off in ~/.config/frametop.conf, or a command),
// then the shared state's enabled 0, then the last event since the game started or ended, then
// "off in a game, the construct's setting outside one". The dashboard's button seen in the
// construct sends no event that we know of (an open question for the headset test).
//
// Header only, with no OpenVR calls: callers poll the events and the scene app and pass them in.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace roomview {

constexpr uint32_t kEventShown = 526, kEventHidden = 527;   // vr::VREvent_RoomViewShown, _Hidden
constexpr size_t kStateSize = 0x6200;

enum class Force { Auto, On, Off };

inline bool ParseForce(const std::string &s, Force *out) {
    if (s == "auto" || s.empty()) *out = Force::Auto;
    else if (s == "on" || s == "1") *out = Force::On;
    else if (s == "off" || s == "0") *out = Force::Off;
    else return false;
    return true;
}
inline const char *Name(Force f) { return f == Force::On ? "on" : f == Force::Off ? "off" : "auto"; }

// The passthrough shared state's 5 config bytes. `path` caches the file found (empty: look in
// `dir` for one of ours of the right size whose config bytes are all 0 or 1). False if none.
inline bool ReadShared(const std::string &dir, std::string &path, uint8_t cfg[5]) {
    auto read = [&](const std::string &p) {
        const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        struct stat st;
        uint8_t b[7];
        const bool ok = fstat(fd, &st) == 0 && st.st_uid == getuid() && size_t(st.st_size) == kStateSize &&
                        pread(fd, b, sizeof b, 0) == ssize_t(sizeof b) && b[2] <= 1 && b[3] <= 1 && b[4] <= 1 &&
                        b[5] <= 1 && b[6] <= 1;
        close(fd);
        if (ok) std::memcpy(cfg, b + 2, 5);
        return ok;
    };
    if (!path.empty() && read(path)) return true;
    path.clear();
    const std::string prefix = "u" + std::to_string(getuid()) + "-Shm_";
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    while (dirent *e = readdir(d)) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) != 0) continue;
        const std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0 || size_t(st.st_size) != kStateSize) continue;
        if (read(p)) {
            path = p;
            break;
        }
    }
    closedir(d);
    return !path.empty();
}

class Detector {
public:
    void SetForce(Force f) { force_ = f; }
    Force force() const { return force_; }
    void SetDir(const std::string &dir) { dir_ = dir, path_.clear(), nextRead_ = 0; }   // tests

    // A SteamVR event (any; only Room View's count).
    void Event(uint32_t type) {
        if (type != kEventShown && type != kEventHidden) return;
        event_ = type == kEventShown ? 1 : 0;
        if (log_) std::printf("room view: %s event\n", event_ ? "RoomViewShown" : "RoomViewHidden"), std::fflush(stdout);
    }
    // Each tick: a scene app (VR game) running, and camera/enableConstructRoomView. Reads the
    // shared state at most twice a second. Returns whether Room View is on.
    bool Update(int64_t nowNs, bool sceneApp, bool constructRoomView) {
        if (nowNs >= nextRead_) {
            nextRead_ = nowNs + 500'000'000;
            uint8_t cfg[5];
            const int enabled = ReadShared(dir_, path_, cfg) ? cfg[0] : -1;
            if (enabled != enabled_ && log_)
                std::printf("room view: passthrough state %s\n",
                            enabled < 0 ? "not found" : enabled ? "enabled" : "disabled"), std::fflush(stdout);
            if (enabled_ == 0 && enabled == 1) event_ = -1;   // turned back on: start over
            enabled_ = enabled;
        }
        if (sceneApp != scene_) scene_ = sceneApp, event_ = -1;   // a game started or ended: its events are old
        const bool was = on_;
        if (force_ != Force::Auto) on_ = force_ == Force::On, why_ = "forced";
        else if (enabled_ == 0) on_ = false, why_ = "passthrough disabled";
        else if (event_ >= 0) on_ = event_ == 1, why_ = on_ ? "shown event" : "hidden event";
        else if (sceneApp) on_ = false, why_ = "VR game";
        else on_ = constructRoomView, why_ = constructRoomView ? "no game, construct room view" : "no game, construct room view off";
        if (log_ && (on_ != was || !logged_)) std::printf("room view %s (%s)\n", on_ ? "on" : "off", why_), std::fflush(stdout);
        logged_ = true;
        return on_;
    }
    bool on() const { return on_; }
    const char *why() const { return why_; }
    int sharedEnabled() const { return enabled_; }   // -1: not found
    void SetLog(bool on) { log_ = on; }

private:
    Force force_ = Force::Auto;
    std::string dir_ = "/dev/shm", path_;
    int64_t nextRead_ = 0;
    int enabled_ = -1, event_ = -1;   // event: -1 none since the game state changed, else shown 1 / hidden 0
    bool scene_ = false, on_ = true, log_ = false, logged_ = false;
    const char *why_ = "not checked";
};

}  // namespace roomview
