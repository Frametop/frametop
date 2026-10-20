#pragma once
#include "feedback.hpp"
#include "openvr.h"
#include <array>
#include <filesystem>
#include <iostream>

namespace framekeyboard {
class VrHaptics {
  public:
    bool connect(const std::filesystem::path& manifest) {
        auto* input = vr::VRInput();
        if (!input || !std::filesystem::is_regular_file(manifest)) {
            std::cerr << "Key haptics unavailable: missing interface or action manifest.\n";
            return false;
        }
        ready_ = check(input->SetActionManifestPath(std::filesystem::absolute(manifest).c_str())) &&
                 check(input->GetActionSetHandle("/actions/feedback", &set_)) &&
                 check(input->GetActionHandle("/actions/feedback/out/key", &action_)) &&
                 check(input->GetInputSourceHandle("/user/hand/left", &hands_[0])) &&
                 check(input->GetInputSourceHandle("/user/hand/right", &hands_[1]));
        return ready_;
    }
    bool send(unsigned device, KeyFeedback kind) {
        if (!ready_ || !vr::VRSystem()->IsTrackedDeviceConnected(device) ||
            vr::VRSystem()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
            return false;
        }
        const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(device);
        if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) {
            return false;
        }
        // This set binds only outputs. It neither claims trigger/grip inputs nor
        // uses the overlay-wide API, whose laser selection ignores our pointer.
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = set_;
        if (!check(vr::VRInput()->UpdateActionState(&active, sizeof(active), 1))) {
            return false;
        }
        const auto hand = hands_[role == vr::TrackedControllerRole_LeftHand ? 0 : 1];
        const auto pulse = key_pulse(kind);
        return check(vr::VRInput()->TriggerHapticVibrationAction(
            action_, 0, pulse.duration, pulse.frequency, pulse.amplitude, hand));
    }

  private:
    bool check(vr::EVRInputError error) {
        if (error == vr::VRInputError_None) {
            return true;
        }
        if (!reported_) {
            std::cerr << "Key haptics unavailable: OpenVR input error " << error << '\n';
            reported_ = true;
        }
        return false;
    }
    vr::VRActionSetHandle_t set_{};
    vr::VRActionHandle_t action_{};
    std::array<vr::VRInputValueHandle_t, 2> hands_{};
    bool ready_{}, reported_{};
};
} // namespace framekeyboard
