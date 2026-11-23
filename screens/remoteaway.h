// Remote displays wait while the headset is off (docs/remote-displays.md, "While the headset is
// off"): nobody sees them, the PC's sound would play in a headset on the desk, and a PC whose
// monitors follow its panels (layout/ft_hostmonitors.py) is wanted back as it was by whoever sat
// down at it.
//
// SteamVR says the headset is idle the moment it comes off, and standby about five seconds
// later. Settling it on the face blinks the proximity sensor for half a second at a time, and a
// headset lifted to look at something goes back on within seconds, so away is standby or ten
// seconds of idle, whichever SteamVR says first. Back on is at once.
//
// vr.cpp reads SteamVR and passes it in. On a change it tells remote.c (ft_remote_away: no
// stream starts while away) and runs `ft-layout remote away on|off` (layout/ft_remoteaway.py),
// which stops the streams and puts the PCs' monitors back, or starts them again.
//
// Header only, with no OpenVR calls.
#pragma once

#include <cstdint>
#include <optional>

namespace remoteaway {

constexpr int64_t kAfterMs = 10'000;

struct Watch {
    // Every so often, with SteamVR's word on the headset: idle (off the head; standby counts)
    // and standby. The new state when it changes: true away, false back on. It starts worn, so
    // a headset that is on when ft-screens starts says nothing, and one that is off says away at
    // once (it has been off for who knows how long).
    std::optional<bool> Tick(int64_t nowMs, bool idle, bool standby) {
        if (!idle) idleSinceMs_ = kNever;
        else if (idleSinceMs_ == kNever) idleSinceMs_ = seen_ ? nowMs : nowMs - kAfterMs;
        seen_ = true;
        const bool away = idle && (standby || nowMs - idleSinceMs_ >= kAfterMs);
        if (away == away_) return std::nullopt;
        return away_ = away;
    }
    bool Away() const { return away_; }

private:
    static constexpr int64_t kNever = INT64_MIN;
    int64_t idleSinceMs_ = kNever;
    bool seen_ = false, away_ = false;
};

}  // namespace remoteaway
