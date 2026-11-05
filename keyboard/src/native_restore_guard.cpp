// App-scoped recovery for minimal Frame-local Gamescope requests. No input is read or sent.
#include "openvr.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <json-c/json.h>
#include <poll.h>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool reply(int fd, bool ok) {
    const std::string data = ok ? "{\"ok\":true}" : "{\"ok\":false}";
    return send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size());
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--fd") {
        return 2;
    }
    const int fd = std::atoi(argv[2]);
    vr::EVRInitError init;
    vr::VR_Init(&init, vr::VRApplication_Background);
    if (init != vr::VRInitError_None || !vr::VROverlay()) {
        reply(fd, false);
        return 3;
    }
    bool connected = reply(fd, true), armed = false, restored = false, saw_open = false;
    bool require_active = false;
    bool quitting = false;
    vr::VROverlayHandle_t target = vr::k_ulOverlayHandleInvalid;
    std::uint64_t progress = 0;
    double deadline = 0;
    auto events = [&] {
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                quitting = true;
                break;
            }
            if (event.eventType == vr::VREvent_KeyboardOpened_Global && restored) {
                saw_open = true;
            }
            if (event.eventType == vr::VREvent_KeyboardClosed_Global && saw_open) {
                restored = saw_open = false;
            }
        }
    };
    auto restore = [&] {
        if (!armed || quitting) {
            return;
        }
        armed = false;
        events();
        if (quitting) {
            return; // SteamVR is exiting; reopening its keyboard cannot recover anything.
        }
        if (require_active && !vr::VROverlay()->IsActiveDashboardOverlay(target)) {
            return; // A selected-frame handoff cannot restore an app left behind.
        }
        const auto result = vr::VROverlay()->ShowKeyboardForOverlay(
            target, vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine,
            vr::KeyboardFlag_Minimal | vr::KeyboardFlag_HideDoneKey | vr::KeyboardFlag_ShowArrowKeys, "",
            1024, "", 0);
        // An already-open request belongs to its current provider and must survive.
        if (result == vr::VROverlayError_None) {
            restored = true;
            saw_open = false;
        }
    };
    while (!quitting && (connected || armed || restored)) {
        events();
        if (quitting) {
            break;
        }
        if (armed && now() >= deadline) {
            restore();
        }
        pollfd item{connected ? fd : -1, POLLIN, 0};
        const int ready = poll(&item, 1, 25);
        if (ready <= 0) {
            continue;
        }
        char buffer[2049];
        const auto count = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (count <= 0 || count >= static_cast<ssize_t>(sizeof(buffer) - 1)) {
            connected = false;
            // Let Steam's 900 ms UI lease remove its hooks before reopening native.
            // Restoring earlier could be mistaken for another toggle by a live hook.
            continue;
        }
        buffer[count] = 0;
        json_object* message = json_tokener_parse(buffer);
        auto field = [&](const char* key) { return json_object_object_get(message, key); };
        const char* raw = message ? json_object_get_string(field("op")) : nullptr;
        const std::string op = raw ? raw : "";
        const double sent = json_object_get_double(field("sent"));
        const auto next = json_object_get_uint64(field("progress"));
        const bool fresh = sent <= now() && now() - sent < .7;
        bool ok = true;
        if (op == "prepare") {
            const char* key = json_object_get_string(field("key"));
            const std::regex local("gamescope\\.gamescope-[0-9]+(\\.window\\.[0-9]+)?");
            ok = !armed && key && std::regex_match(key, local) && fresh &&
                 json_object_get_boolean(field("ready")) && json_object_get_int(field("flags")) == 13 &&
                 vr::VROverlay()->FindOverlay(key, &target) == vr::VROverlayError_None &&
                 (!json_object_get_boolean(field("requireActive")) ||
                  vr::VROverlay()->IsActiveDashboardOverlay(target));
            if (ok) {
                require_active = json_object_get_boolean(field("requireActive"));
                armed = true;
                deadline = sent + 1.2;
                progress = next;
            }
        } else if (op == "renew") {
            if (armed && next > progress) {
                progress = next;
                if (fresh && json_object_get_boolean(field("ready"))) {
                    deadline = sent + 1.2;
                }
            }
        } else if (op == "cancel") {
            armed = false;
        } else if (op == "restore") {
            restore();
        } else {
            ok = false;
        }
        if (message) {
            json_object_put(message);
        }
        if (!reply(fd, ok)) {
            connected = false;
        }
    }
    close(fd);
    vr::VR_Shutdown();
    return 0;
}
