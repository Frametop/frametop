#!/usr/bin/env python3
"""Apply the FrameHands dashboard, adaptive pointer and pinch improvements.

Run at the root of AlistairBishop06/frametop-hands-only. Refuses unknown
source layouts and avoids writing anything until every replacement validates.
"""
from pathlib import Path
import sys

root = Path.cwd()
if not (root / 'framehands').is_file() or not (root / 'renderer/handview.cpp').is_file():
    sys.exit('Run this from the root of frametop-hands-only.')

changes = {}

def edit(path, old, new):
    path = str(path)
    original = changes.get(path, (root / path).read_text())
    if old == new:
        return
    occurrences = original.count(old)
    if occurrences == 0 and path == 'renderer/handview.cpp' and "Ignore FrameHands' virtual pointer." in original:
        # This fix was already committed to the fork in November 2026.
        return
    if occurrences != 1:
        raise RuntimeError(f'{path}: expected one matching block, found {occurrences}; your checkout may differ. Nothing written.')
    changes[path] = original.replace(old, new, 1)

def append(path, text):
    path = str(path)
    original = changes.get(path, (root / path).read_text())
    if text in original:
        raise RuntimeError(f'{path}: already patched? Nothing written.')
    changes[path] = original.rstrip() + '\n\n' + text.strip() + '\n'

def new(path, text):
    path = str(path)
    if (root / path).exists():
        raise RuntimeError(f'{path} already exists; refusing to overwrite')
    changes[path] = text

# Match the already-existing virtual controller exclusion used by fh-input.
edit('renderer/handview.cpp',
'''            if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
            const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);''',
'''            if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
            // fh_pointer follows the bare hand. It is not a physical controller.
            char type[64] = {};
            vr::VRSystem()->GetStringTrackedDeviceProperty(
                i, vr::Prop_ControllerType_String, type, sizeof type);
            if (!std::strcmp(type, "fh_pointer")) continue;
            const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);''')

# Let deliberate, slightly imperfect thumb/index contact register without
# turning on the more false-positive-prone dip detector automatically.
edit('hands/track/pinch.h',
'''    double begin_m = 0.020;   // thumb and index tips closer than this: the pinch begins''',
'''    double begin_m = 0.025;   // allow slightly imperfect contact (tunable with HANDS_PINCH_BEGIN)''')
# Slightly stronger still-hand filtering, without increasing drag during movement.
edit('hands/track/tracker.cpp',
'''constexpr double kMinCutoff = 2.0;     // Hz, a still hand
constexpr double kBeta = 30.0;         // Hz more per m/s of palm speed''',
'''constexpr double kMinCutoff = 1.7;     // Hz, steadier fingers while stationary
constexpr double kBeta = 35.0;         // responds quickly when deliberately moving''')
edit('hands/track/skeleton.h',
'''    double pos_min = 2.0, pos_beta = 30.0;   // palm centre: + beta per m/s
    double rot_min = 2.0, rot_beta = 10.0;   // palm rotation: + beta per rad/s
    double ang_min = 2.0, ang_beta = 3.0;    // a finger's joint angles: + beta per rad/s of that finger''',
'''    double pos_min = 1.7, pos_beta = 34.0;   // slightly steadier still palm
    double rot_min = 1.8, rot_beta = 12.0;   // less rotational shimmer
    double ang_min = 1.8, ang_beta = 3.3;    // reduce finger jitter, retain quick pinches''')
edit('hands/track/main.cpp',
'''    num("HANDS_PINCH_END", pinch_params.end_m, 0.025, 0.08);''',
'''    num("HANDS_PINCH_BEGIN", pinch_params.begin_m, 0.012, 0.035);
    num("HANDS_PINCH_END", pinch_params.end_m, 0.025, 0.08);
    pinch_params.begin_m = std::min(pinch_params.begin_m, pinch_params.end_m - 0.008);''')
edit('config/default.conf',
'''GESTURE_INPUT=0
''',
'''GESTURE_INPUT=0
HANDS_POINTER_SMOOTH=1
# Optional pinch sensitivity (metres); 0.025 is more forgiving than the old 0.020.
HANDS_PINCH_BEGIN=0.025
# Optional near-contact dip detection: can catch fast taps but risks false clicks.
HANDS_PINCH_DIP=0
''')

# A One Euro-like filter on the actual dashboard ray. The tracker and visual hand
# overlay were already filtered; fh-input was sending the ray without any filter.
edit('pointer/helper/fh-input.cpp',
'''double Length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }''',
'''double Length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
// Adaptively smooth the dashboard pointer. Small movements are steadied;
// fast deliberate motion raises the cut-off to keep the ray responsive.
struct AimSmoother {
    V origin{}, direction{};
    double originSpeed = 0, directionSpeed = 0;
    int64_t last = 0;
    static double Alpha(double hz, double dt) { return 1.0 - std::exp(-2.0 * M_PI * hz * dt); }
    static V Smooth(V raw, V filtered, double &speed, double dt, double gain) {
        const V delta = raw - filtered;
        speed += (Length(delta) / dt - speed) * Alpha(1.5, dt);
        return filtered + delta * Alpha(std::clamp(2.5 + gain * speed, 2.5, 40.0), dt);
    }
    void Reset() { last = 0; originSpeed = directionSpeed = 0; }
    void Step(V p, V d, int64_t now) {
        if (!last || now <= last || now - last > 250'000'000ll) {
            origin = p; direction = d; originSpeed = directionSpeed = 0;
        } else {
            const double dt = std::clamp((now - last) / 1e9, 0.002, 0.10);
            origin = Smooth(p, origin, originSpeed, dt, 25.0);
            direction = Smooth(d, direction, directionSpeed, dt, 12.0);
            const double n = Length(direction);
            direction = n > 1e-6 ? direction * (1.0 / n) : d;
        }
        last = now;
    }
};''')
edit('pointer/helper/fh-input.cpp',
'''    Controller controllers[vr::k_unMaxTrackedDeviceCount]{};
    int owner = -1;''',
'''    Controller controllers[vr::k_unMaxTrackedDeviceCount]{};
    AimSmoother aim;
    const bool smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";
    int owner = -1;''')
edit('pointer/helper/fh-input.cpp',
'''        if (pick != owner || pick < 0) {
            if (pressed) Send(fd, "btn trigger 0"), pressed = false;''',
'''        if (pick != owner || pick < 0) {
            aim.Reset();  // never interpolate a ray between different hands
            if (pressed) Send(fd, "btn trigger 0"), pressed = false;''')
edit('pointer/helper/fh-input.cpp',
'''                const V origin = knuckle + dir*0.03;
                char pose[160];
                std::snprintf(pose, sizeof pose, "pose %.5f %.5f %.5f %.3f %.3f", origin.x, origin.y, origin.z,
                              std::atan2(-dir.x,-dir.z)*180/M_PI, std::asin(std::clamp(dir.y,-1.0,1.0))*180/M_PI);''',
'''                const V origin = knuckle + dir*0.03;
                if (smoothPointer) aim.Step(origin, dir, now);
                const V p = smoothPointer ? aim.origin : origin;
                const V d = smoothPointer ? aim.direction : dir;
                char pose[160];
                std::snprintf(pose, sizeof pose, "pose %.5f %.5f %.5f %.3f %.3f", p.x, p.y, p.z,
                              std::atan2(-d.x,-d.z)*180/M_PI, std::asin(std::clamp(d.y,-1.0,1.0))*180/M_PI);''')

# The dashboard process is separate from the four tracking services. Thus it
# survives disabling the tracker and can be launched from SteamVR independently.
edit('framehands',
'''UNITS = ("framehands-camd.service", "framehands-track.service", "framehands-view.service", "framehands-input.service")''',
'''UNITS = ("framehands-camd.service", "framehands-track.service", "framehands-view.service", "framehands-input.service")
DASHBOARD_UNIT = "framehands-dashboard.service"''')
edit('framehands',
'''    for unit in UNITS:
        cmd = "enable" if unit in wanted else "disable"''',
'''    # Dashboard remains available even when tracking/autostart is switched off.
    for unit in (*UNITS, DASHBOARD_UNIT):
        cmd = "enable" if (unit == DASHBOARD_UNIT or unit in wanted) else "disable"''')
# An optional doctor check. Avoid depending on the user's exact driver-list format:
# some checkouts have customised this list or changed the final tuple.
current_cli = changes.get('framehands', (root / 'framehands').read_text())
doctor_anchor = '    if shutil.which("getcap") and (ROOT / "hands/build/fh-camd").exists():'
if current_cli.count(doctor_anchor) == 1:
    edit('framehands', doctor_anchor,
         '    if not (ROOT / "dashboard/build/framehands-dashboard").is_file():\n'
         '        issues.append("dashboard controller binary missing")\n'
         + doctor_anchor)
else:
    print('Note: skipped optional dashboard doctor check (customised CLI layout).')
edit('framehands',
'''            binary = str(ROOT / "pointer/build/fh-input")
            os.execv(binary, (binary,))
        elif cmd == "enable":''',
'''            binary = str(ROOT / "pointer/build/fh-input")
            os.execv(binary, (binary,))
        elif cmd == "run-dashboard":
            binary = str(ROOT / "dashboard/build/framehands-dashboard")
            os.execv(binary, (binary,))
        elif cmd == "enable":''')
edit('framehands',
'''                                            "gestures", "logs", "doctor", "sync", "run-tracker", "run-camd", "run-view", "run-input"))''',
'''                                            "gestures", "logs", "doctor", "sync", "run-tracker", "run-camd", "run-view", "run-input", "run-dashboard"))''')
edit('framehands',
'''            if active("steamvr.service"):
                ctl("start", *selected(), check=True)
            print("FrameHands will start with SteamVR.")''',
'''            if active("steamvr.service"):
                ctl("start", *selected(), check=True)
                ctl("start", DASHBOARD_UNIT, check=True)
            print("FrameHands will start with SteamVR.")''')
edit('framehands',
'''            if active("steamvr.service") and config().get("AUTO_START", "1") != "0" and selected():
                ctl("start", *selected(), check=True)''',
'''            if active("steamvr.service"):
                ctl("start", DASHBOARD_UNIT, check=True)
                if config().get("AUTO_START", "1") != "0" and selected():
                    ctl("start", *selected(), check=True)''')
edit('framehands',
'''            for unit in UNITS:
                print(f"{unit}: {'running' if active(unit) else 'stopped'}, {'enabled' if enabled(unit) else 'disabled'}")''',
'''            for unit in (*UNITS, DASHBOARD_UNIT):
                print(f"{unit}: {'running' if active(unit) else 'stopped'}, {'enabled' if enabled(unit) else 'disabled'}")''')
edit('framehands',
'''return subprocess.call(("journalctl", "--user", *sum((("-u", u) for u in UNITS), ()),''',
'''return subprocess.call(("journalctl", "--user", *sum((("-u", u) for u in (*UNITS, DASHBOARD_UNIT)), ()),''')

# We keep the dashboard separate from CLI 'stop'/'disable' and from selected()
# so the physical controller (or CLI over SSH) can always turn tracking back on.

new('services/framehands-dashboard.service', '''[Unit]
Description=FrameHands SteamVR dashboard controls
After=steamvr.service
Requisite=steamvr.service
PartOf=steamvr.service

[Service]
ExecStart=@ROOT@/framehands run-dashboard
Restart=on-failure
RestartSec=3
TimeoutStopSec=5

[Install]
WantedBy=steamvr.service
''')

new('dashboard/build.sh', '''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/include
. ../scripts/openvr.sh
read -r -a openvr_libs <<< "$OPENVR_LIBS"
${CXX:-g++} -std=c++17 -O2 -Wall -Ibuild/include -o build/framehands-dashboard \\
    framehands-dashboard.cpp "${openvr_libs[@]}"
strip --strip-unneeded build/framehands-dashboard
''')

new('dashboard/framehands-dashboard.cpp', r'''// Small standalone SteamVR dashboard overlay; no Qt, browser or desktop needed.
// Its systemd unit survives disabling the four hand-tracking services.
#include <openvr.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr int W = 640, H = 360;
struct RGB { uint8_t r, g, b; };
std::vector<uint8_t> pixels(W * H * 4);

// Five-wide bitmap capitals keep this app entirely self-contained on ARM64.
const uint8_t *Glyph(char c) {
    static const uint8_t font[39][7] = {
        {14,17,17,31,17,17,17}, // A
        {30,17,17,30,17,17,30}, // B
        {15,16,16,16,16,16,15}, // C
        {30,17,17,17,17,17,30}, // D
        {31,16,16,30,16,16,31}, // E
        {31,16,16,30,16,16,16}, // F
        {15,16,16,23,17,17,15}, // G
        {17,17,17,31,17,17,17}, // H
        {31,4,4,4,4,4,31},       // I
        {7,2,2,2,18,18,12},     // J
        {17,18,20,24,20,18,17}, // K
        {16,16,16,16,16,16,31}, // L
        {17,27,21,21,17,17,17}, // M
        {17,25,21,19,17,17,17}, // N
        {14,17,17,17,17,17,14}, // O
        {30,17,17,30,16,16,16}, // P
        {14,17,17,17,21,18,13}, // Q
        {30,17,17,30,20,18,17}, // R
        {15,16,16,14,1,1,30},   // S
        {31,4,4,4,4,4,4},       // T
        {17,17,17,17,17,17,14}, // U
        {17,17,17,17,17,10,4},  // V
        {17,17,17,21,21,21,10}, // W
        {17,17,10,4,10,17,17},  // X
        {17,17,10,4,4,4,4},     // Y
        {31,1,2,4,8,16,31},    // Z
        {14,17,19,21,25,17,14}, // 0
        {4,12,4,4,4,4,14},     // 1
        {14,17,1,2,4,8,31},    // 2
        {30,1,1,14,1,1,30},    // 3
        {2,6,10,18,31,2,2},    // 4
        {31,16,16,30,1,1,30},  // 5
        {14,16,16,30,17,17,14},// 6
        {31,1,2,4,8,8,8},      // 7
        {14,17,17,14,17,17,14},// 8
        {14,17,17,15,1,1,14},  // 9
        {0,4,4,0,4,4,0},      // :
        {0,0,0,31,0,0,0},     // -
        {0,0,0,0,0,0,0},      // space
    };
    if (c >= 'A' && c <= 'Z') return font[c - 'A'];
    if (c >= '0' && c <= '9') return font[26 + c - '0'];
    return font[c == ':' ? 36 : c == '-' ? 37 : 38];
}
void Rect(int x0, int y0, int x1, int y1, RGB c) {
    for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
            auto i = size_t((y * W + x) * 4);
            pixels[i] = c.r; pixels[i+1] = c.g; pixels[i+2] = c.b; pixels[i+3] = 255;
        }
}
void Text(const std::string &s, int y, int size, RGB c) {
    const int width = int(s.size()) * 6 * size;
    const int x0 = (W - width) / 2;
    for (size_t k = 0; k < s.size(); ++k) {
        const uint8_t *p = Glyph(s[k]);
        for (int r = 0; r < 7; ++r)
            for (int col = 0; col < 5; ++col)
                if (p[r] & (16 >> col))
                    Rect(x0 + int(k)*6*size + col*size, y+r*size,
                         x0 + int(k)*6*size + (col+1)*size, y+(r+1)*size, c);
    }
}

std::filesystem::path AppRoot() {
    // $app/dashboard/build/framehands-dashboard -> $app
    return std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path().parent_path();
}
std::filesystem::path Manifest() { return AppRoot() / "dashboard/framehands.vrmanifest"; }
std::filesystem::path Cli() { return AppRoot() / "framehands"; }

bool Enabled() {
    const char *home = std::getenv("HOME");
    if (!home) return false;
    std::ifstream in(std::filesystem::path(home) / ".config/framehands/config");
    std::string line;
    bool track = true, autostart = true;
    while (std::getline(in, line)) {
        if (line == "TRACKING=0") track = false;
        if (line == "TRACKING=1") track = true;
        if (line == "AUTO_START=0") autostart = false;
        if (line == "AUTO_START=1") autostart = true;
    }
    return track && autostart;
}
void Paint(bool enabled, bool busy, bool failed) {
    Rect(0, 0, W, H, {13, 22, 38});
    Rect(0, 0, W, 10, {73, 175, 235});
    Text("FRAMEHANDS", 39, 5, {232, 244, 255});
    Text("HAND TRACKING", 116, 3, {176, 198, 219});
    const RGB action = enabled ? RGB{30, 130, 98} : RGB{45, 123, 195};
    Rect(76, 165, 564, 277, action);
    if (busy) Text("UPDATING", 188, 5, {255, 255, 255});
    else Text(enabled ? "ON" : "OFF", 188, 6, {255, 255, 255});
    Text(failed ? "FAILED - CHECK LOGS" : enabled ? "CLICK TO TURN OFF" : "CLICK TO TURN ON",
         293, 2, {204, 220, 233});
}

int RegisterManifest(bool remove) {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Utility);
    if (error != vr::VRInitError_None) return 1;
    const std::string path = Manifest().string();
    const vr::EVRApplicationError result = remove
        ? vr::VRApplications()->RemoveApplicationManifest(path.c_str())
        : vr::VRApplications()->AddApplicationManifest(path.c_str(), false);
    vr::VR_Shutdown();
    return result == vr::VRApplicationError_None ? 0 : 1;
}
}

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--unregister")) return RegisterManifest(true);
    if (argc == 2 && !std::strcmp(argv[1], "--register")) return RegisterManifest(false);
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        std::cerr << "framehands-dashboard: " << vr::VR_GetVRInitErrorAsEnglishDescription(error) << '\n';
        return 1;
    }
    const std::string manifest = Manifest().string();
    const auto registered = vr::VRApplications()->AddApplicationManifest(manifest.c_str(), false);
    if (registered != vr::VRApplicationError_None)
        std::cerr << "framehands-dashboard: registration returned " << int(registered) << '\n';
    vr::VROverlayHandle_t mainOverlay = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumb = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError created = vr::VROverlay()->CreateDashboardOverlay(
        "framehands.dashboard", "FrameHands", &mainOverlay, &thumb);
    if (created != vr::VROverlayError_None) {
        std::cerr << "framehands-dashboard: CreateDashboardOverlay failed: " << int(created) << '\n';
        vr::VR_Shutdown();
        return 1;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(mainOverlay, 1.8f);
    vr::VROverlay()->SetOverlayInputMethod(mainOverlay, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t mouseScale{}; mouseScale.v[0] = W; mouseScale.v[1] = H;
    vr::VROverlay()->SetOverlayMouseScale(mainOverlay, &mouseScale);
    // The thumbnail needs no external PNG dependencies.
    std::array<uint8_t, 64*64*4> thumbnail{};
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const int i = (y*64+x)*4;
        const bool mid = x > 20 && x < 44 && y > 13 && y < 51;
        thumbnail[i] = mid ? 88 : 17;
        thumbnail[i+1] = mid ? 198 : 38;
        thumbnail[i+2] = mid ? 238 : 66;
        thumbnail[i+3] = 255;
    }
    vr::VROverlay()->SetOverlayRaw(thumb, thumbnail.data(), 64, 64, 4);

    bool enabled = Enabled(), busy = false, failed = false;
    pid_t child = -1;
    auto next = std::chrono::steady_clock::now();
    Paint(enabled, busy, failed);
    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    bool running = true;
    while (running) {
        vr::VREvent_t event{};
        while (vr::VROverlay()->PollNextOverlayEvent(mainOverlay, &event, sizeof event)) {
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const bool toEnable = !enabled;
                child = fork();
                if (child == 0) {
                    const std::string cli = Cli().string();
                    execl(cli.c_str(), cli.c_str(), toEnable ? "enable" : "disable", (char *)nullptr);
                    _exit(127);
                }
                busy = child > 0;
                failed = child < 0;
                Paint(enabled, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit) running = false;
        if (child > 0) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child || done < 0) {
                child = -1;
                busy = false;
                failed = done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                enabled = Enabled();
                Paint(enabled, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        if (std::chrono::steady_clock::now() >= next && !busy) {
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            const bool current = Enabled();
            if (current != enabled) {
                enabled = current;
                Paint(enabled, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    vr::VROverlay()->DestroyOverlay(mainOverlay);
    vr::VROverlay()->DestroyOverlay(thumb);
    vr::VR_Shutdown();
    return 0;
}
''')

# Build, package, test and install this dashboard binary like the renderer.
edit('scripts/build.sh',
'''bash "$root/pointer/build.sh"''',
'''bash "$root/pointer/build.sh"
bash "$root/dashboard/build.sh"''')
edit('scripts/package.sh',
'''"$tmp/config" "$tmp/services" "$tmp/licenses"''',
'''"$tmp/config" "$tmp/services" "$tmp/licenses" "$tmp/dashboard/build"''')
edit('scripts/package.sh',
'''cp "$root/renderer/build/framehands-view" "$tmp/renderer/build/"''',
'''cp "$root/renderer/build/framehands-view" "$tmp/renderer/build/"
cp "$root/dashboard/build/framehands-dashboard" "$tmp/dashboard/build/"''')
edit('scripts/package.sh',
'''    renderer/build/framehands-view pointer/build/fh-input pointer/build/driver_fh_pointer.so \\''',
'''    renderer/build/framehands-view dashboard/build/framehands-dashboard \\
    pointer/build/fh-input pointer/build/driver_fh_pointer.so \\''')
edit('.github/workflows/release.yml',
'''file hands/build/fh-camd hands/build/fh-hands renderer/build/framehands-view pointer/build/fh-input pointer/build/driver_fh_pointer.so''',
'''file hands/build/fh-camd hands/build/fh-hands renderer/build/framehands-view dashboard/build/framehands-dashboard pointer/build/fh-input pointer/build/driver_fh_pointer.so''')
# The second occurrence in CI is updated below.

edit('install.sh',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do
  file=$units/$unit''',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service framehands-dashboard.service; do
  file=$units/$unit''')
edit('install.sh',
'''! -f $root/pointer/build/driver_fh_pointer.so ]]; then''',
'''! -f $root/pointer/build/driver_fh_pointer.so || ! -x $root/dashboard/build/framehands-dashboard ]]; then''')
# validate dashboard in the installed source before we touch the old installation
edit('install.sh',
'''[[ -f $source_dir/hands/models/student/hand.ncnn.bin ]] ||''',
'''[[ -x $source_dir/dashboard/build/framehands-dashboard ]] || fail 'the dashboard controller is missing'
[[ -f $source_dir/hands/models/student/hand.ncnn.bin ]] ||''')
edit('install.sh',
'''"$stage/config" "$stage/services" "$stage/licenses"''',
'''"$stage/config" "$stage/services" "$stage/licenses" "$stage/dashboard/build"''')
edit('install.sh',
'''cp -a "$source_dir/renderer/build/framehands-view" "$stage/renderer/build/"''',
'''cp -a "$source_dir/renderer/build/framehands-view" "$stage/renderer/build/"
cp -a "$source_dir/dashboard/build/framehands-dashboard" "$stage/dashboard/build/"''')
edit('install.sh',
'''# From here, only FrameHands units are changed. A failed upgrade restores the old app.
systemctl --user stop framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true''',
'''# From here, only FrameHands units are changed. A failed upgrade restores the old app.
systemctl --user stop framehands-dashboard.service framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true''')
edit('install.sh',
'''mv "$stage" "$app"
for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do''',
'''mkdir -p "$stage/dashboard"
# SteamVR's ARM64 application launcher requires binary_path_linux_arm.
# This app can be launched from SteamVR's overlay app menu as well as its own tab.
cat > "$stage/dashboard/launch.sh" <<EOF
#!/bin/sh
exec "$app/framehands" run-dashboard
EOF
chmod 755 "$stage/dashboard/launch.sh"
FRAMEHANDS_APP="$app" FRAMEHANDS_STAGE="$stage" python3 - <<'PYMANIFEST'
import json, os
from pathlib import Path
app = Path(os.environ['FRAMEHANDS_APP'])
manifest = {'source': 'builtin', 'applications': [{
    'app_key': 'framehands.dashboard', 'launch_type': 'binary',
    'binary_path_linux_arm': str(app / 'dashboard/launch.sh'),
    'binary_path_linux': str(app / 'dashboard/launch.sh'),
    'is_dashboard_overlay': True,
    'strings': {'en_us': {'name': 'FrameHands', 'description': 'Hand tracking controls'}}}]}
(Path(os.environ['FRAMEHANDS_STAGE']) / 'dashboard/framehands.vrmanifest').write_text(json.dumps(manifest, indent=2) + '\\n')
PYMANIFEST
mv "$stage" "$app"
for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service framehands-dashboard.service; do''')
edit('install.sh',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do
      sed "s|@ROOT@|$app|g" "$app/services/$unit" > "$units/$unit"
    done''',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service framehands-dashboard.service; do
      if [[ -f $app/services/$unit ]]; then
        sed "s|@ROOT@|$app|g" "$app/services/$unit" > "$units/$unit"
      else
        rm -f "$units/$unit"
      fi
    done''')
edit('install.sh',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do rm -f "$units/$unit"; done''',
'''for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service framehands-dashboard.service; do rm -f "$units/$unit"; done''')
edit('install.sh',
'''echo 'FrameHands install: service activation failed; restoring the previous install' >&2
  systemctl --user stop framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true''',
'''echo 'FrameHands install: service activation failed; restoring the previous install' >&2
  systemctl --user stop framehands-dashboard.service framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true''')
# Keep the separate dashboard unit off the original uninstall's stop list only
# until it unregisters from SteamVR.
edit('uninstall.sh',
'''for unit in framehands-input.service framehands-view.service framehands-track.service framehands-camd.service; do''',
'''for unit in framehands-dashboard.service framehands-input.service framehands-view.service framehands-track.service framehands-camd.service; do''')
edit('uninstall.sh',
'''systemctl --user daemon-reload 2>/dev/null || true
reg=/opt/steamvr/bin/linuxarm64/vrpathreg''',
'''systemctl --user daemon-reload 2>/dev/null || true
if [[ -x $app/dashboard/build/framehands-dashboard ]]; then
  "$app/dashboard/build/framehands-dashboard" --unregister 2>/dev/null || true
fi
reg=/opt/steamvr/bin/linuxarm64/vrpathreg''')
edit('tests/test_framehands.py',
'''if __name__ == "__main__":
    unittest.main()''',
'''    def test_dashboard_stays_enabled_when_tracking_is_off(self):
        from types import SimpleNamespace
        actions = []
        def fake_ctl(*args, **_kwargs):
            actions.append(args)
            return SimpleNamespace(returncode=0, stderr="")
        with patch.object(cli, "config", return_value={"TRACKING": "0", "AUTO_START": "0"}), \
             patch.object(cli, "ctl", side_effect=fake_ctl):
            cli.sync_enabled()
        self.assertIn(("enable", cli.DASHBOARD_UNIT), actions)
        self.assertIn(("disable", cli.UNITS[1]), actions)


if __name__ == "__main__":
    unittest.main()''')

append('README.md', '''## SteamVR dashboard control and steadier pointer

FrameHands now installs a small **FrameHands** SteamVR dashboard overlay (no desktop or
browser required). Open SteamVR's dashboard and select the FrameHands tab; on
Steam Frame builds that expose registered overlay applications under **+**, it
can also be launched from that menu. Click the large ON/OFF panel to enable or
disable tracking. The dashboard app runs separately from the tracker so it does
not shut itself down when tracking is disabled. **Note:** disabling tracking also
removes the hand-pointer ray; to turn it back on without SSH, you need another
working SteamVR input device (for example a physical controller). As a fallback,
run `~/.local/bin/framehands enable` through SSH or a terminal.

The tracker uses slightly stronger adaptive palm and finger filtering, and the
dashboard pointer has its own adaptive low-pass filtering: it steadies while
your hand is relatively still and responds more quickly when you make deliberate
movements. The index pinch begins at 25 mm instead of 20 mm by default. You can
set `HANDS_POINTER_SMOOTH=0` to turn off ray filtering, or adjust
`HANDS_PINCH_BEGIN=0.025` in `~/.config/framehands/config` (try 0.028
if contact is still missed) and run `framehands restart` to apply it. Optional
`HANDS_PINCH_DIP=1` can catch additional quick taps near contact, but may create
accidental clicks; it is deliberately off by default. These thresholds affect
gesture classification, not camera or model accuracy. Hardware validation and
personal calibration are still recommended.

After publishing an ARM64 release, update the Frame using `git pull` and
`./install.sh` from a source-only checkout, then restart SteamVR normally. The
installer registers the overlay with SteamVR on its next launch.
''')

# Ensure the workflow architecture checks cover the new binary in both loops.
workflow = changes.get('.github/workflows/release.yml', (root / '.github/workflows/release.yml').read_text())
needle = 'renderer/build/framehands-view pointer/build/fh-input pointer/build/driver_fh_pointer.so'
if workflow.count(needle) != 1:
    raise RuntimeError('release.yml: expected the readelf loop not yet patched')
changes['.github/workflows/release.yml'] = workflow.replace(needle, 'renderer/build/framehands-view dashboard/build/framehands-dashboard pointer/build/fh-input pointer/build/driver_fh_pointer.so')

# All preconditions passed: only now write the modified files.
for path, content in changes.items():
    dest = root / path
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(content.encode('utf-8'))  # preserve LF on Windows checkouts
    if path == 'dashboard/build.sh':
        dest.chmod(0o755)
print(f'Applied {len(changes)} file changes. Review git diff, then build/publish your ARM64 release.')
