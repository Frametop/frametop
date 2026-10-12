// Frametop's keyboard (keyboard.cpp): a panel of keys that ft-screens shows for a text
// field on the desktop. Any laser or the 3D mouse types on it, as on the screens.
#pragma once

#include <openvr.h>

#include <cstdint>

namespace keyboard {

// What happened on the panel since the last Poll.
struct Event {
    enum Type { Key, Closed, RawMouseButtonUp } type;
    uint32_t code;  // Key: linux KEY_*
    bool pressed;   // Key: down or up
    uint32_t device = vr::k_unTrackedDeviceIndexInvalid, button = 0; // RawMouseButtonUp
};

// Show the panel at `pose` (standing universe; its front faces +z), creating it the first
// time. False if SteamVR won't make the overlay.
bool Show(const vr::HmdMatrix34_t &pose);
void Hide();
bool Shown();
// Where it is now (it can be carried by its grab bar).
const vr::HmdMatrix34_t &Pose();
// A button came up on `device` somewhere else: stop carrying the panel if it was.
void EndDragBy(uint32_t device);
// Controllers' lasers work the panel with the dashboard closed, like the screens'.
void SetLasers(bool on);
// A laser (its pose, aiming along -z) points at the panel.
bool Aimed(const vr::HmdMatrix34_t &laser);
// The panel's keys, Close key, and raw releases for the shared drag/press owner.
void Poll(void (*handle)(const Event &, void *), void *data);
void Destroy();

}  // namespace keyboard
