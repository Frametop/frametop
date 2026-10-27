// Tracking watch (DEE-206): screens that jump, often up, after walking away and coming back.
//
// Every Frametop panel sits in SteamVR's standing space (SetOverlayTransformAbsolute with
// TrackingUniverseStanding), so it stays put in the room only while standing stays put. When the
// Frame loses tracking, vrserver's log (2026-10-24) shows it switching to a temporary universe
// ("default floor height without bounds") for seconds to minutes, then restoring the saved one
// ("Standing origin changed. Sending reset standing zero pose event"). A recentered standing is
// saved per tracking world, a universe's own standing is the Room Setup origin, and the two
// can differ. Either step may move the panels in the room; the SteamVR dashboard follows the
// head and doesn't show it.
//
// Which step does it isn't known yet, so this only logs, to /tmp/frametop-screens.log with
// wall-clock times to line up with vrserver.txt:
//   - the standing and chaperone events (801, 804, 805, 808);
//   - raw->standing moving (GetRawZeroPoseToStandingAbsoluteTrackingPose), checked on those
//     events and once a second, with how far it moved and how much of that is height;
//   - the universe id changing (Prop_CurrentUniverseId_Uint64), checked on the events and
//     every 5 seconds;
//   - the head jumping in standing between two ticks, which is what a panel sees whichever
//     frame moved (a relocalized tracking world leaves raw->standing alone).
//
// Header only, with no OpenVR calls: the caller reads SteamVR and passes the values in.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <sys/time.h>

namespace trackwatch {

constexpr uint32_t kUniverseChanged = 801, kSeatedReset = 804, kFlushCache = 805, kStandingReset = 808;

inline const char *EventName(uint32_t type) {
    switch (type) {
    case kUniverseChanged: return "ChaperoneUniverseHasChanged";
    case kSeatedReset: return "SeatedZeroPoseReset";
    case kFlushCache: return "ChaperoneFlushCache";
    case kStandingReset: return "StandingZeroPoseReset";
    default: return nullptr;
    }
}

struct Pose {
    float m[3][4];  // vr::HmdMatrix34_t's layout
};

// Yaw of a pose's -Z (forward), in degrees.
inline double Yaw(const Pose &p) { return std::atan2(-p.m[0][2], -p.m[2][2]) * 180 / M_PI; }

class Watch {
public:
    // How far the head may move between two ticks before it counts as a jump (m). A head
    // moves a few centimetres per tick at most.
    double headJump = 0.25;
    // How far raw->standing may move before it counts (m, degrees).
    double standingMove = 0.02, standingTurn = 1.0;

    // A SteamVR event. True if it's one of ours (check the standing now).
    bool Event(uint32_t type) {
        const char *name = EventName(type);
        if (!name) return false;
        Log("event %s (%u)", name, type);
        return true;
    }

    // raw->standing now. `why`: what asked ("event", "poll").
    void Standing(const Pose &p, const char *why) {
        if (!haveStanding_) {
            standing_ = p, haveStanding_ = true;
            Log("raw->standing at start: %s", Describe(p).c_str());
            return;
        }
        const double dx = p.m[0][3] - standing_.m[0][3], dy = p.m[1][3] - standing_.m[1][3],
                     dz = p.m[2][3] - standing_.m[2][3];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        double turn = std::fmod(Yaw(p) - Yaw(standing_) + 540.0, 360.0) - 180.0;
        if (d < standingMove && std::fabs(turn) < standingTurn) return;
        Log("raw->standing moved (%s): %.3f m (height %+.3f m), yaw %+.1f deg; was %s, now %s", why, d, dy, turn,
            Describe(standing_).c_str(), Describe(p).c_str());
        standing_ = p;
    }

    void Universe(uint64_t id) {
        if (haveUniverse_ && id == universe_) return;
        if (haveUniverse_) Log("universe %llu -> %llu", (unsigned long long)universe_, (unsigned long long)id);
        else Log("universe %llu at start", (unsigned long long)id);
        universe_ = id, haveUniverse_ = true;
    }

    // The head's pose in standing this tick (valid: SteamVR has one). Jumps are logged once
    // per `quiet` ticks at most, so a flapping tracker can't fill the log.
    void Head(bool valid, const Pose &p, long tick, long quiet = 90) {
        if (!valid) {
            if (headValid_) Log("head pose lost");
            headValid_ = false;
            return;
        }
        if (!headValid_ && seenHead_) Log("head pose back after %ld ticks", tick - headTick_);
        if (headValid_ && tick - headTick_ <= 3) {
            const double dx = p.m[0][3] - head_.m[0][3], dy = p.m[1][3] - head_.m[1][3],
                         dz = p.m[2][3] - head_.m[2][3];
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > headJump && tick - lastJumpLog_ >= quiet) {
                Log("head jumped %.3f m (height %+.3f m) in standing in %ld ticks: from (%.3f %.3f %.3f) to (%.3f %.3f %.3f)",
                    d, dy, tick - headTick_, head_.m[0][3], head_.m[1][3], head_.m[2][3], p.m[0][3], p.m[1][3],
                    p.m[2][3]);
                lastJumpLog_ = tick;
            }
        }
        head_ = p, headValid_ = true, seenHead_ = true, headTick_ = tick;
    }

    // Where lines go (tests catch them). Default: stderr, which is the screens log.
    void (*sink)(const char *line) = [](const char *line) { std::fprintf(stderr, "%s\n", line); };

private:
    Pose standing_{}, head_{};
    bool haveStanding_ = false, haveUniverse_ = false, headValid_ = false, seenHead_ = false;
    uint64_t universe_ = 0;
    long headTick_ = 0, lastJumpLog_ = -1000000;

    static std::string Describe(const Pose &p) {
        char b[96];
        std::snprintf(b, sizeof b, "t=(%.3f %.3f %.3f) yaw %.1f", p.m[0][3], p.m[1][3], p.m[2][3], Yaw(p));
        return b;
    }

    template <typename... A> void Log(const char *fmt, A... a) {
        char msg[512], stamp[32];
        if constexpr (sizeof...(A) == 0) std::snprintf(msg, sizeof msg, "%s", fmt);
        else std::snprintf(msg, sizeof msg, fmt, a...);
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
        char line[600];
        std::snprintf(line, sizeof line, "%s.%03ld trackwatch: %s", stamp, long(tv.tv_usec / 1000), msg);
        sink(line);
    }
};

}  // namespace trackwatch
