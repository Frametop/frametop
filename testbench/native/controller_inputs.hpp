#pragma once
#include "openvr_driver.h"
#include <map>
#include <string>

namespace frame {
// Original component schema for the installed Frame profile. No runtime resource
// files are distributed here. Skeleton, haptics and pose components are not faked.
struct ControllerInput {
    bool boolean = true;
    bool two_sided = false;
    vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
    vr::EVRInputError creation_error = vr::VRInputError_None;
    float value = 0;
    bool published = false;
};
using ControllerInputs = std::map<std::string, ControllerInput>;
inline ControllerInputs controller_input_schema(unsigned hand) {
    ControllerInputs result;
    auto button = [&](const char *name) {
        for (const char *suffix : {"/click", "/touch"})
            result.emplace(std::string("/input/") + name + suffix, ControllerInput{});
    };
    for (const char *name : {"system", "bumper", "trigger", "grip", "thumbstick"}) button(name);
    if (hand == 0) for (const char *name : {"view", "dpad_up", "dpad_right", "dpad_down", "dpad_left"}) button(name);
    else for (const char *name : {"menu", "a", "b", "x", "y"}) button(name);
    result.emplace("/input/thumbrest/touch", ControllerInput{});
    for (const char *name : {"/input/trigger/value", "/input/grip/value"})
        result.emplace(name, ControllerInput{false, false});
    for (const char *name : {"/input/thumbstick/x", "/input/thumbstick/y"})
        result.emplace(name, ControllerInput{false, true});
    return result;
}
} // namespace frame
