// The headset's "worn" signal, written from this driver when someone asks on the control socket
// ("worn <0|1>"; ft-powerd does, power/standby.h says when and why).
//
// SteamVR's standby (displays off, tracking and passthrough cameras paused, about 5 W instead of
// 14-18 W) follows one input component of the headset: /proximity, which the headset's own driver
// (cv) writes from its proximity sensor. Nothing in OpenVR's client API moves it. A driver can:
// IVRDriverInput hands out one handle per container and name, so asking for /proximity on the
// headset's container gives back cv's component, and one UpdateBooleanComponent on it reads to
// vrserver exactly like the sensor changing. False: standby 5 s later
// (power.turnOffScreensTimeout). True: awake at once.
//
// Undocumented, so here is what was measured (cv 2.19.3, SteamOS 0.5.9, 2026-11-24):
// - cv writes when its sensor changes, and once more 15-20 s after SteamVR wakes, with what the
//   sensor says then. A wrong "worn" from here lasts that long at most.
// - In standby cv stays quiet for as long as the sensor doesn't change (an hour, checked).
// - Every write wakes vrserver for about 6 s, even false over false. So one write per request,
//   never one per frame, and no "not worn" to a SteamVR that is already in standby.
// - Two other ways were tried and must stay out: IVRServerDriverHost_004::ProximitySensorState
//   (vrserver still hands the old interface out, and the call crashes it), and
//   Prop_ContainsProximitySensor_Bool = false on the headset (takes, changes nothing).
// scripts/update-check.py points here when a SteamVR update could have changed any of it.
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <cstdio>

namespace ft_pointer {

class HeadsetWorn {
public:
    // From any thread: the next frame writes it. A second request before that frame replaces
    // the first.
    void Ask(bool worn) { pending_.store(worn ? 1 : 0); }

    // From vrserver's frame (IServerTrackedDeviceProvider::RunFrame).
    void RunFrame() {
        const int v = pending_.exchange(-1);
        if (v < 0) return;
        const vr::PropertyContainerHandle_t hmd =
            vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
        vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
        vr::EVRInputError err = hmd == vr::k_ulInvalidPropertyContainer
                                    ? vr::VRInputError_InvalidDevice
                                    : vr::VRDriverInput()->CreateBooleanComponent(hmd, "/proximity", &handle);
        if (err == vr::VRInputError_None) err = vr::VRDriverInput()->UpdateBooleanComponent(handle, v != 0, 0);
        char line[96];
        if (err == vr::VRInputError_None) std::snprintf(line, sizeof line, "ft_pointer: headset worn = %d, as asked", v);
        else std::snprintf(line, sizeof line, "ft_pointer: couldn't write headset worn = %d (input error %d)", v, int(err));
        vr::VRDriverLog()->Log(line);
    }

private:
    std::atomic<int> pending_{-1};  // 0 or 1: to write; -1: nothing
};

}  // namespace ft_pointer
