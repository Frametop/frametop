// ft-powerd's forced standby: when the displays are due to go dark and SteamVR still has the
// headset as worn, ask SteamVR for its standby as well as turning the backlight off.
//
// Why: the backlight alone leaves tracking, the cameras and the compositor running (about 17 W on
// a stand that covers the proximity sensor). SteamVR's standby pauses them (about 5 W) and keeps
// everything else up: the desktop session, services, remote access. The way in is the headset's
// "worn" signal, written by our driver (pointer/driver/worn.h). ft-powerd's main loop does the
// writing; this is only the bookkeeping, so it can be tested without SteamVR.
//
// The rules:
// - One "not worn" per attempt, and only to a SteamVR that has the headset as worn. If SteamVR
//   still has it as worn kAnswerSecs later, the attempt failed (an older driver that doesn't
//   know the command): the caller writes "worn" back and the backlight is all there is.
// - It holds once SteamVR has the headset as off, which is within a second or two. SteamVR's
//   standby proper (tracking paused) follows 5 s after the write, unless something keeps SteamVR
//   awake; the caller tells the two apart by the activity level.
// - Any use (a key, a button, a mouse, "on") ends it: the caller writes "worn". Movement does
//   too in the seconds before tracking pauses, and can't after.
// - SteamVR waking by itself ends it too (the sensor changed: the headset was picked up, or the
//   testbench faked a wearer). If nobody then uses the headset before the displays are due to go
//   dark again, the wake wasn't a person, and standby wouldn't hold next time either: until the
//   next use the backlight is used instead, as before this existed. Use in the first kSettleSecs
//   after the wake doesn't count for that: tracking coming back can move the pose by itself.
// - Not while the testbench fakes a wearer: its tests need SteamVR awake.
#pragma once

namespace ft_power {

class Standby {
public:
    static constexpr double kAnswerSecs = 15;  // measured: idle 1 s after the write, plus ft-powerd's 1 s
    static constexpr double kSettleSecs = 15;

    enum class Begin {
        Ask,       // write "not worn"
        Disabled,  // DISPLAY_OFF_STANDBY=0
        Testing,   // the testbench fakes a wearer
        Unheld,    // SteamVR woke by itself last time and nobody used the headset since
        Busy,      // already asked or holding
    };
    enum class Step { None, Holds, Failed, Woke };

    // Asked and waiting for SteamVR, or holding.
    bool Active() const { return phase_ != Phase::Idle; }
    bool Holding() const { return phase_ == Phase::Held; }

    // The displays are due to go dark and SteamVR has the headset as worn. Ask: the caller writes
    // "not worn" (and calls Abort() if it couldn't). Anything else: only the backlight.
    Begin Start(double now, bool enabled, bool testing) {
        if (phase_ != Phase::Idle) return Begin::Busy;
        if (!enabled) return Begin::Disabled;
        if (testing) return Begin::Testing;
        if (unheld_) return Begin::Unheld;
        phase_ = Phase::Asked;
        askedAt_ = now;
        return Begin::Ask;
    }

    // The "not worn" couldn't be sent.
    void Abort() { phase_ = Phase::Idle; }

    // With every reading of whether SteamVR has the headset as away.
    //   Holds:  SteamVR has the headset as off, once.
    //   Failed: it didn't in time. The caller writes "worn" back.
    //   Woke:   SteamVR left the standby by itself. Nothing to write.
    Step Tick(double now, bool away) {
        if (phase_ == Phase::Asked) {
            if (away) {
                phase_ = Phase::Held;
                return Step::Holds;
            }
            if (now - askedAt_ >= kAnswerSecs) {
                phase_ = Phase::Idle;
                return Step::Failed;
            }
        } else if (phase_ == Phase::Held && !away) {
            phase_ = Phase::Idle;
            unheld_ = true;
            wokeAt_ = now;
            return Step::Woke;
        }
        return Step::None;
    }

    // Someone used the headset. True: the caller writes "worn".
    bool Use(double now) {
        if (unheld_ && now - wokeAt_ >= kSettleSecs) unheld_ = false;
        if (phase_ == Phase::Idle) return false;
        phase_ = Phase::Idle;
        return true;
    }

private:
    enum class Phase { Idle, Asked, Held };
    Phase phase_ = Phase::Idle;
    bool unheld_ = false;
    double askedAt_ = 0, wokeAt_ = 0;
};

}  // namespace ft_power
