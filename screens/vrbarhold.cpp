// The grab bar's press (user, 2026-11-19: "add a bit of a delay when tapping on the grab bar before the
// display actually moves so that the display doesn't jump when you try to just tap on it"): a press on a
// bar waits; held long enough or moved past a tap, the move starts (panelmenu::HoldStep), let go before
// that it was a tap, which opens the bar's menu (vrbarmenu.cpp).
//   The press's laser may drift off the bar while it waits (the panel doesn't follow yet), and the carry
// starts from wherever the hand is then, so its release and its wheel come on whatever the laser is on:
// another of our panels, a control, the menu, or the catcher (vr.cpp's UpdateCatcher). Every one of our
// overlays reads its events through PollOverlay, which hands those to the bar first.
#include "vrpanel.h"
#include "panelmenu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftvr {

// The bar pressed by `dev`, `along` m from its middle: nothing moves yet (BarHold decides).
void BarPressed(Screen &s, vr::TrackedDeviceIndex_t dev, double along) {
    s.barPressMs = -1;
    if (!LaserPose(dev, &s.barPressFrom)) return;
    s.barPressMs = NowMs(), s.barPressDevice = dev, s.barPressAlong = along;
}

namespace {

// How far the pressing device's laser has moved and turned since the press (false: no pose now).
// The laser's own turn decides, not the controller's -Z: the Frame's laser points 40 degrees below it.
bool BarPressDrift(const Screen &s, double *moved, double *turned) {
    Mat now;
    if (!LaserPose(s.barPressDevice, &now)) return false;
    const Mat &was = s.barPressFrom;
    *moved = std::sqrt(std::pow(now.m[0][3] - was.m[0][3], 2) + std::pow(now.m[1][3] - was.m[1][3], 2) +
                       std::pow(now.m[2][3] - was.m[2][3], 2));
    // (the angle between its pointing directions, -z)
    const double dot = -now.m[0][2] * -was.m[0][2] + -now.m[1][2] * -was.m[1][2] + -now.m[2][2] * -was.m[2][2];
    *turned = std::acos(std::clamp(dot, -1.0, 1.0));
    return true;
}

// The bar let go by `dev`: was it a tap (panelmenu::IsTap)? The press ends either way.
bool BarTap(Screen &s, vr::TrackedDeviceIndex_t dev) {
    if (s.barPressMs < 0 || s.barPressDevice != dev) return false;
    const int64_t at = s.barPressMs;
    s.barPressMs = -1;
    double moved, turned;
    return s.drag == Drag::None && !s.layoutHeld && BarPressDrift(s, &moved, &turned) && panelmenu::IsTap(NowMs() - at, moved, turned);
}

// The bar's press let go by `dev`, wherever its laser is: a tap opens the menu where it was pressed; a
// move under way ends (EndDragsBy).
void BarLetGo(vr::TrackedDeviceIndex_t dev) {
    for (auto &[i, s] : g_screens)
        if (s.barPressMs >= 0 && s.barPressDevice == dev && BarTap(s, dev)) OpenBarMenu(i, s.barPressAlong, false);
    EndDragsBy(dev);
}

// Its bar's press by `dev`, waiting or carrying.
bool PressBy(const Screen &s, vr::TrackedDeviceIndex_t dev) {
    return (s.barPressMs >= 0 && s.barPressDevice == dev) || (s.barCarry && s.drag == Drag::Move && s.dragDevice == dev);
}

// A left press by a device whose bar press waits or carries: its release was lost where we don't see
// (another app's overlay, outside VR games), so the wait is dropped and the carry let go where it is.
// Only the bar's: a carry_toggle carry by the same laser is CarryClick's to end. The press itself is its own.
void BarLost(vr::TrackedDeviceIndex_t dev) {
    bool carried = false;
    for (auto &[i, s] : g_screens) {
        if (!PressBy(s, dev)) continue;
        const bool waiting = s.barPressMs >= 0;
        std::printf("screen %d: its bar's release was lost (a new press): %s\n", i + 1, waiting ? "no tap, no move" : "let go here");
        if (waiting) s.barPressMs = -1;
        else carried = true;
    }
    if (carried) EndDragsBy(dev);
}

// A laser's button, seen first for a bar's press: a left release by a device whose bar press waits or
// carries is the bar's (true: taken; what's under the laser never had the press). A left press by one:
// its release was lost (BarLost); the press goes on to what's under the laser.
bool BarClaims(const vr::VREvent_t &ev) {
    if (ev.eventType != vr::VREvent_MouseButtonDown && ev.eventType != vr::VREvent_MouseButtonUp) return false;
    if (ev.data.mouse.button != vr::VRMouseButton_Left) return false;
    const vr::TrackedDeviceIndex_t dev = ev.trackedDeviceIndex;
    if (ev.eventType == vr::VREvent_MouseButtonDown) return BarLost(dev), false;
    if (!BarHeldBy(dev)) return false;
    BarLetGo(dev);
    return true;
}

}  // namespace

bool BarHeldBy(vr::TrackedDeviceIndex_t dev) {
    return std::any_of(g_screens.begin(), g_screens.end(), [dev](const auto &e) { return PressBy(e.second, dev); });
}

// The waiting press becomes a move, from where the panel is now (StartDrag takes the device's pose now:
// no catching up, so nothing jumps). A pulse says it's held.
void BarCarry(Screen &s, const char *why) {
    const vr::TrackedDeviceIndex_t dev = s.barPressDevice;
    s.barPressMs = -1;
    if (s.drag != Drag::None) return;
    StartDrag(s, Drag::Move, dev);
    if (s.drag != Drag::Move) return;
    s.barCarry = true;
    std::printf("screen %d: the bar held (%s)\n", ScreenIndexOf(s) + 1, why);
    if (IsHandController(dev)) vr::VRSystem()->TriggerHapticPulse(dev, 0, 2500);
}

// Every tick, a press waiting on the bar: held long enough, or moved past a tap, the move starts
// (panelmenu::HoldStep). Its release, wherever the laser drifted, ends it (PollOverlay). A release on
// another app's overlay can't be seen: the controller's buttons say so where SteamVR tells (in VR games),
// and the next press by that laser drops the wait (BarLost).
void BarHold(Screen &s) {
    if (s.barPressMs < 0) return;
    double moved = 0, turned = 0;
    const bool posed = BarPressDrift(s, &moved, &turned);
    const int64_t held = NowMs() - s.barPressMs;
    const int holds = IsHandController(s.barPressDevice) ? ControllerHolds(s.barPressDevice) : -1;
    switch (panelmenu::HoldStep(held, moved, turned, s.drag != Drag::None, posed, s.visible && !g_paused, holds)) {
        case panelmenu::Wait::Keep: return;
        case panelmenu::Wait::Drop:
            if (holds == 0) std::printf("screen %d: its bar's release was lost (the controller holds nothing): no move\n", ScreenIndexOf(s) + 1);
            s.barPressMs = -1;
            return;
        case panelmenu::Wait::Carry: return BarCarry(s, held >= panelmenu::TapLimits{}.ms ? "held" : "moved off a tap");
    }
}

// A bar's press, waiting or carrying, by a laser: its device (kNone: none). Its release comes on
// whatever the laser is on, and the laser may be off the bar: drifted while the press waited (the
// panel didn't follow), and kept off by the carry that starts from there.
vr::TrackedDeviceIndex_t BarHolder() {
    for (auto &[i, s] : g_screens) {
        if (s.barPressMs >= 0) return s.barPressDevice;
        if (s.barCarry && s.drag == Drag::Move) return s.dragDevice;
    }
    return kNone;
}

bool PollOverlay(vr::VROverlayHandle_t overlay, vr::VREvent_t *ev) {
    while (overlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->PollNextOverlayEvent(overlay, ev, sizeof *ev))
        if (!CarryScroll(*ev) && !BarClaims(*ev)) return true;
    return false;
}

}  // namespace ftvr
