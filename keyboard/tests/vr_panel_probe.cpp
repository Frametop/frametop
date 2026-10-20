#include "framekeyboard/app.hpp"
#include "framekeyboard/placement_store.hpp"
#include "openvr.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void wait_for_ui() {
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
}
void click(vr::VROverlayHandle_t panel, float x, float y) {
    vr::VREvent_t event{};
    event.data.mouse.x = x;
    event.data.mouse.y = framekeyboard::texture_height - framekeyboard::popup_margin - y;
    event.data.mouse.button = vr::VRMouseButton_Left;
    event.eventType = vr::VREvent_MouseButtonDown;
    vr::VROverlayView()->PostOverlayEvent(panel, &event);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    event.eventType = vr::VREvent_MouseButtonUp;
    vr::VROverlayView()->PostOverlayEvent(panel, &event);
    wait_for_ui();
}
vr::HmdMatrix34_t transform(vr::VROverlayHandle_t panel) {
    vr::HmdMatrix34_t result{};
    vr::ETrackingUniverseOrigin origin{};
    require(vr::VROverlay()->GetOverlayTransformAbsolute(panel, &origin, &result) ==
                vr::VROverlayError_None,
            "cannot read overlay transform");
    return result;
}
} // namespace
int main(int argc, char** argv) {
    bool connected = false;
    try {
        require(argc == 3 || argc == 4, "usage: vr-panel-probe MODE EXPECTED_PID [SNAPSHOT_FILE]");
        const std::string mode = argv[1];
        require(mode == "--exercise" || mode == "--check-centered" || mode == "--snapshot" ||
                    mode == "--check-snapshot" || mode == "--watch-close" || mode == "--close",
                "unknown probe mode");
        require((mode != "--snapshot" && mode != "--check-snapshot" && mode != "--watch-close") ||
                    argc == 4,
                "snapshot path required");
        const auto pid = static_cast<std::uint32_t>(std::stoul(argv[2]));
        // Synthetic clicks are restricted to a deliberately input-disabled test
        // instance. The probe never injects into Steam's keyboard or a browser.
        std::ifstream command("/proc/" + std::to_string(pid) + "/cmdline");
        const std::string arguments((std::istreambuf_iterator<char>(command)), {});
        require(!arguments.empty() &&
                    (mode == "--snapshot" || (arguments.find("--input") == std::string::npos &&
                                              arguments.find("--start-enabled") == std::string::npos)),
                "probe requires an input-disabled test instance");
        vr::EVRInitError error{};
        vr::VR_Init(&error, vr::VRApplication_Background);
        require(error == vr::VRInitError_None, "SteamVR is not available");
        connected = true;
        vr::VROverlayHandle_t panel{};
        require(vr::VROverlay()->FindOverlay("org.framekeyboard.panel", &panel) ==
                    vr::VROverlayError_None,
                "keyboard overlay does not exist");
        require(vr::VROverlay()->GetOverlayRenderingPid(panel) == pid, "unexpected overlay owner");
        require(vr::VROverlay()->IsOverlayVisible(panel), "keyboard overlay is hidden");
        if (mode == "--watch-close") {
            framekeyboard::SavedPlacement last{};
            bool observed = false, closed = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline) {
                vr::HmdMatrix34_t pose{};
                vr::ETrackingUniverseOrigin origin{};
                if (vr::VROverlay()->GetOverlayRenderingPid(panel) != pid ||
                    vr::VROverlay()->GetOverlayTransformAbsolute(panel, &origin, &pose) !=
                        vr::VROverlayError_None) {
                    closed = true;
                    break;
                }
                float width = 0;
                vr::VROverlay()->GetOverlayWidthInMeters(panel, &width);
                last.width = width;
                last.universe = vr::VRSystem()->GetUint64TrackedDeviceProperty(
                    vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64);
                for (std::size_t r = 0; r < 3; ++r) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        last.transform[r][c] = pose.m[r][c];
                    }
                }
                observed = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            require(observed && closed, "close not observed within 60 seconds");
            framekeyboard::save_placement(argv[3], last);
            std::cout << "Captured final live pose before the keyboard closed.\n";
        } else if (mode == "--snapshot" || mode == "--check-snapshot") {
            const auto current = transform(panel);
            float width = 0;
            vr::VROverlay()->GetOverlayWidthInMeters(panel, &width);
            framekeyboard::SavedPlacement snapshot{};
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 4; ++c) {
                    snapshot.transform[r][c] = current.m[r][c];
                }
            }
            snapshot.width = width;
            snapshot.universe = vr::VRSystem()->GetUint64TrackedDeviceProperty(
                vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64);
            if (mode == "--snapshot") {
                framekeyboard::save_placement(argv[3], snapshot);
                std::cout << "Captured current overlay pose and width.\n";
            } else {
                const auto saved = framekeyboard::load_placement(argv[3]);
                require(saved.has_value(), "snapshot missing");
                require(std::abs(saved->width - width) < .00001, "restored width differs");
                for (std::size_t r = 0; r < 3; ++r) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        require(std::abs(saved->transform[r][c] - current.m[r][c]) < .00001,
                                "restored pose differs");
                    }
                }
                std::cout << "Restored live pose and width match the pre-close snapshot.\n";
            }
        } else if (mode == "--close") {
            click(panel, 1552, 33);
            std::cout << "Clicked Close on the input-disabled keyboard.\n";
        } else if (mode == "--exercise") {
            require(vr::VROverlayView() != nullptr, "overlay event interface unavailable");
            const auto original = transform(panel);
            float width = 0, resized = 0;
            vr::VROverlay()->GetOverlayWidthInMeters(panel, &width);
            // Reuse actual toolbar geometry, including the optional language
            // button. This App only reads the test instance's profiles/config;
            // its NullSink cannot deliver input to any application.
            framekeyboard::Options options;
            options.config_dir = framekeyboard::default_config_dir();
            std::vector<std::string> args;
            for (std::size_t start = 0; start < arguments.size();) {
                const auto end = arguments.find('\0', start);
                args.push_back(arguments.substr(start, end - start));
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            for (std::size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == "--config-dir") {
                    options.config_dir = args[i + 1];
                } else if (args[i] == "--data-dir") {
                    options.data_dir = args[i + 1];
                }
            }
            framekeyboard::NullSink sink;
            framekeyboard::App ui(options, sink);
            auto click_control = [&](const std::string& id) {
                for (const auto& control : ui.view().controls) {
                    if (control.id == id) {
                        click(panel, static_cast<float>(control.bounds.x + control.bounds.width / 2),
                              static_cast<float>(control.bounds.y + control.bounds.height / 2));
                        return;
                    }
                }
                throw std::runtime_error("missing control " + id);
            };
            click_control("size-larger");
            vr::VROverlay()->GetOverlayWidthInMeters(panel, &resized);
            require(std::abs(resized - width - .05) < .005, "larger icon adds 5 cm");
            click_control("size-smaller");
            vr::VROverlay()->GetOverlayWidthInMeters(panel, &resized);
            require(std::abs(resized - width) < .005, "smaller icon restores width");
            const auto after = transform(panel);
            for (int row = 0; row < 3; ++row) {
                require(std::abs(after.m[row][3] - original.m[row][3]) < .00001,
                        "resize icons preserve position");
            }
            std::cout << "Live main-view resize icons changed width without moving the panel.\n";
        } else {
            const auto current = transform(panel);
            require(std::abs(current.m[1][0]) < .001 &&
                        std::abs(current.m[1][1] - std::cos(50 * std::numbers::pi / 180)) < .001 &&
                        std::abs(current.m[1][2] - std::sin(50 * std::numbers::pi / 180)) < .001,
                    "relaunch did not clear rotation offsets");
            std::cout << "Overlay still visible with expected owner and recentered desk tilt.\n";
        }
        vr::VR_Shutdown();
        return 0;
    } catch (const std::exception& error) {
        if (connected) {
            vr::VR_Shutdown();
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
