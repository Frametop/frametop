// Gamepad mode for a Frametop panel (vrgamepad.cpp): when it's on, for which panel, and what ends it.
// Pure, tested by screens/tests/padmode-test.cpp.
//
// It is Frametop's own (user, 2026-11-27: "Pressing both grip buttons does not take us out of gamepad
// mode", and the card SteamVR's dimmed dashboard showed "floats in front of us"). Until then the pill had
// SteamVR's dashboard dim on a tab of ours, which bought nothing: through all of that Gamepad mode SteamVR's
// input focus was the panel clicked last, exactly as with the dashboard closed (headset, 2026-11-27: mode
// 2, no pid for VR input, Steam Input 769, frametop.app.200 on top, both ways). What makes the controllers
// a pad is ft-screens leaving the laser alone on every panel (Holds) while the input relay reads them raw.
//
// A click on the pill starts it. With the Steam menu open, SteamVR lasers everything whatever our panels
// ask, so the mode waits for the menu to close (ft-dashpatch closes it) and gives up after kCloseWaitMs.
// It ends when the relay says so (both grips squeezed, or the mouse used), when the Steam menu opens, when
// its panel goes, and when Frametop pauses or a VR game starts.
#pragma once

#include <cstdint>

namespace padmode {

constexpr int64_t kCloseWaitMs = 3000;   // the Steam menu open at the click: it closes this soon, or no mode
constexpr int64_t kLaserGraceMs = 3000;  // after it ends: our lasers on whatever SteamVR's mode is (Grace)

class Mode {
public:
    // The pill clicked over `panel`. True when the Steam menu has to close first.
    bool Click(int panel, bool menuOpen, int64_t nowMs) {
        panel_ = panel, waiting_ = menuOpen, sinceMs_ = nowMs, endedMs_ = -1;
        return menuOpen;
    }
    // The relay's word, or a command: over now. False when it wasn't on.
    bool Leave(int64_t nowMs) { return End(nowMs); }
    // Each tick. `panelThere`: its panel is still shown. `blocked`: paused, or a VR game runs. Why it
    // ended just now, or null.
    const char *Tick(bool menuOpen, bool panelThere, bool blocked, int64_t nowMs) {
        if (panel_ < 0) return nullptr;
        const char *why = nullptr;
        if (blocked) why = "Frametop paused or a VR game started";
        else if (!panelThere) why = "its panel is gone";
        else if (waiting_ && !menuOpen) waiting_ = false;
        else if (waiting_ && nowMs - sinceMs_ >= kCloseWaitMs) why = "the Steam menu stayed open";
        else if (!waiting_ && menuOpen) why = "the Steam menu opened";
        if (why) End(nowMs);
        return why;
    }
    // The panel with the pad, -1 for none (and while the Steam menu still closes).
    int Panel() const { return waiting_ ? -1 : panel_; }
    // The panel it is on or starting for, -1 for none.
    int For() const { return panel_; }
    // No laser of ours on any panel: the controllers are the pad's, or about to be.
    bool Holds() const { return panel_ >= 0; }
    // Just over: our lasers come on though SteamVR may still say Gamepad mode. SteamVR goes back to its
    // laser when a controller points at a panel that takes one, and ours only take one outside its
    // Gamepad mode (vr.cpp's LasersByMode), so without this the two would wait on each other.
    bool Grace(int64_t nowMs) const { return endedMs_ >= 0 && nowMs - endedMs_ < kLaserGraceMs; }

private:
    bool End(int64_t nowMs) {
        if (panel_ < 0) return false;
        panel_ = -1, waiting_ = false, endedMs_ = nowMs;
        return true;
    }
    int panel_ = -1;
    bool waiting_ = false;  // for the Steam menu to close
    int64_t sinceMs_ = 0;   // the click
    int64_t endedMs_ = -1;  // -1: never, or on again since
};

}  // namespace padmode
