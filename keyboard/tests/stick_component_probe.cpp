#include "framekeyboard/grip.hpp"
#include "openvr.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
framekeyboard::Transform convert(const vr::HmdMatrix34_t& matrix) {
    framekeyboard::Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = matrix.m[r][c];
        }
    }
    return result;
}
framekeyboard::Transform component(const char* model, float x, float y,
                                   const char* name = "thumbstick") {
    vr::VRControllerState_t input{};
    input.rAxis[0].x = x;
    input.rAxis[0].y = y;
    if (std::string_view(name) == "trigger") {
        input.rAxis[1].x = x;
    }
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    if (!vr::VRRenderModels()->GetComponentState(model, name, &input, &mode, &state)) {
        throw std::runtime_error("render component unavailable");
    }
    return convert(state.mTrackingToComponentRenderModel);
}
} // namespace
int main() {
    vr::EVRInitError error{};
    vr::VR_Init(&error, vr::VRApplication_Utility);
    if (error) {
        return 1;
    }
    int result = 0;
    try {
        // Synthetic states animate a model calculation only. No input is sent to
        // hardware, overlays or applications, and no physical presses are read.
        for (const auto* model :
             {"{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right"}) {
            framekeyboard::ComponentAxis axis;
            if (!axis.calibrate(component(model, 0, 0), component(model, 0, 1))) {
                throw std::runtime_error("thumbstick calibration failed");
            }
            double maximum_error = 0;
            for (float x : {-1.f, -.5f, 0.f, .5f, 1.f}) {
                for (float y : {-1.f, -.5f, 0.f, .5f, 1.f}) {
                    maximum_error =
                        std::max(maximum_error, std::abs(axis.read(component(model, x, y)) - y));
                }
            }
            std::cout << model << " maximum axis error=" << maximum_error << '\n';
            if (maximum_error > .02) {
                throw std::runtime_error("thumbstick axis extraction failed");
            }
            const auto neutral = component(model, 0, 0, "trigger");
            maximum_error = 0;
            for (float pull : {0.f, .25f, .5f, 1.f}) {
                const auto angle = framekeyboard::component_rotation_degrees(
                    neutral, component(model, pull, 0, "trigger"));
                maximum_error = std::max(maximum_error, std::abs(angle - 12.5 * pull));
            }
            std::cout << model << " maximum trigger angle error=" << maximum_error << '\n';
            if (maximum_error > .1) {
                throw std::runtime_error("trigger release fallback model mapping failed");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    vr::VR_Shutdown();
    return result;
}
