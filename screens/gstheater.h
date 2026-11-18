// View in Theater for a gamescope app's panel (vrgsapps.cpp; docs/design.md "Gamescope apps"):
// when a window lent to SteamVR's theater comes back to its panel. Pure, tested by
// screens/tests/gstheater-test.cpp.
//
// Lending is untagging (gscontrol.h's Control::Lend): gamescope then gives the window an overlay
// of its own, valve.steam.desktopgame.<app id>, and SteamVR shows that in its theater at once
// (testbench, 2026-11-18: in its theater stack within a second), with its own controls and way
// out. ft-dashpatch says which game SteamVR's theater shows (steamvrmode's fifth number). The
// window comes back once SteamVR has shown it there and then doesn't (the user docked it, floated
// it, or closed the game), or if SteamVR never shows it there within kTheaterWaitMs (an
// ft-dashpatch too old to say, or none running): never left untagged with nothing to bring it back.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace gsapps {

constexpr int64_t kTheaterWaitMs = 5000;

class Theater {
public:
    struct Back {
        uint32_t win;
        const char *why;
    };

    // `win`, Steam game `app`'s, lent at nowMs.
    void Lent(uint32_t win, uint32_t app, int64_t nowMs) { lent_[win] = {app, nowMs, false, false}; }
    bool IsLent(uint32_t win) const { return lent_.count(win) != 0; }
    // ft-dashpatch's word: the game SteamVR's theater shows now (0: none).
    void Heard(uint32_t theaterApp) {
        for (auto &[win, l] : lent_) {
            if (theaterApp == l.app) l.seen = true;
            else if (l.seen) l.left = true;
        }
    }
    // The windows to take back now (each once), and why.
    std::vector<Back> Due(int64_t nowMs) {
        std::vector<Back> out;
        for (auto it = lent_.begin(); it != lent_.end();) {
            const Lending &l = it->second;
            if (l.left) out.push_back({it->first, "out of SteamVR's theater"});
            else if (!l.seen && nowMs - l.sinceMs >= kTheaterWaitMs)
                out.push_back({it->first, "SteamVR's theater didn't show it (is ft-dashpatch running?)"});
            else {
                ++it;
                continue;
            }
            it = lent_.erase(it);
        }
        return out;
    }
    // A lent window SteamVR's theater shows now (the dashboard tiles step aside then).
    bool Showing() const {
        for (const auto &[win, l] : lent_)
            if (l.seen && !l.left) return true;
        return false;
    }
    void Clear() { lent_.clear(); }

private:
    struct Lending {
        uint32_t app;
        int64_t sinceMs;
        bool seen, left;   // SteamVR's theater has shown it; and since stopped
    };
    std::map<uint32_t, Lending> lent_;
};

}  // namespace gsapps
