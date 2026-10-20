#include "framekeyboard/vr_haptics.hpp"
#include <chrono>
#include <thread>

// Opt-in physical output check: press/release clicks per hand, left then right.
// No keyboard input, overlay events or global input settings are changed.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: haptics-probe ACTION_MANIFEST\n";
        return 1;
    }
    vr::EVRInitError error{};
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        return 1;
    }
    framekeyboard::VrHaptics haptics;
    bool ok = haptics.connect(argv[1]);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (const auto role : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand}) {
        const auto device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
        for (int i = 0; i < 2 && ok; ++i) {
            ok = haptics.send(device, i == 0 ? framekeyboard::KeyFeedback::Press
                                             : framekeyboard::KeyFeedback::Release);
            std::cout << "Haptic request: role=" << role << " device=" << device << " ok=" << ok << '\n'
                      << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
    }
    vr::VR_Shutdown();
    return ok ? 0 : 1;
}
