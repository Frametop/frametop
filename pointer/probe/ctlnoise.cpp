// Controller noise logger: probes P4 and P5 of the hand interaction design
// (frame-hands/notes/hand-interaction.md, section 7). For the handoff arbiter's thresholds
// (pointer/helper/handoff.h): how much a controller resting on a desk moves, against one held as
// still as possible, and what SteamVR says about it meanwhile.
//
// Every 11 ms it reads every controller's pose (standing universe). Each second it prints, per
// controller: tracking result, activity level, charging, position spread over that second (the
// largest axis' standard deviation, mm), the mean and largest speed (m/s) and spin (rad/s), and
// how many 100 ms runs would count as "moving" for the arbiter (speed > 0.15 m/s or spin >
// 1 rad/s for 100 ms in a row). At the end, per controller: the 1 s spreads' p50 and p99, and
// the speed and spin p99 over every sample.
// Runs as a background OpenVR client (in the dev container); it takes nothing from anyone.
// Usage: ctlnoise [--seconds N] [--quiet]   (--quiet: only the summary)
#include <openvr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stop = 0;

namespace {

const char *Activity(vr::EDeviceActivityLevel l) {
    switch (l) {
    case vr::k_EDeviceActivityLevel_Unknown: return "unknown";
    case vr::k_EDeviceActivityLevel_Idle: return "idle";
    case vr::k_EDeviceActivityLevel_UserInteraction: return "interaction";
    case vr::k_EDeviceActivityLevel_UserInteraction_Timeout: return "interaction-timeout";
    case vr::k_EDeviceActivityLevel_Standby: return "standby";
    case vr::k_EDeviceActivityLevel_Idle_Timeout: return "idle-timeout";
    }
    return "?";
}

const char *Result(vr::ETrackingResult r) {
    switch (r) {
    case vr::TrackingResult_Uninitialized: return "uninit";
    case vr::TrackingResult_Calibrating_InProgress: return "calibrating";
    case vr::TrackingResult_Calibrating_OutOfRange: return "cal-out";
    case vr::TrackingResult_Running_OK: return "ok";
    case vr::TrackingResult_Running_OutOfRange: return "out-of-range";
    case vr::TrackingResult_Fallback_RotationOnly: return "rotation-only";
    }
    return "?";
}

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

struct Window {
    double sum[3] = {}, sq[3] = {};
    int n = 0;
    double speedSum = 0, speedMax = 0, spinSum = 0, spinMax = 0;
    int movingRuns = 0;
};

struct Track {
    std::string name;
    Window w;
    std::vector<double> spreads, speeds, spins;
    Clock::time_point movingSince{};
    bool counted = false;  // this moving run has been counted
};

}  // namespace

int main(int argc, char **argv) {
    double seconds = 60;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else {
            std::fprintf(stderr, "usage: %s [--seconds N] [--quiet]\n", argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem *sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    static const char *roles[] = {"none", "left", "right", "optout", "treadmill", "stylus"};
    std::map<vr::TrackedDeviceIndex_t, Track> tracks;
    const auto start = Clock::now();
    auto lastPrint = start;
    while (!g_stop && std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
        vr::TrackedDevicePose_t all[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, all, vr::k_unMaxTrackedDeviceCount);
        const auto now = Clock::now();
        for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
            char type[64] = "";
            sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
            if (!std::strcmp(type, "ft_pointer")) continue;  // ours: not a real controller
            Track &t = tracks[i];
            const auto role = sys->GetControllerRoleForTrackedDeviceIndex(i);
            t.name = std::to_string(i) + " " + type + " " + (role < 6 ? roles[role] : "?");
            const auto &p = all[i];
            if (!p.bPoseIsValid || p.eTrackingResult != vr::TrackingResult_Running_OK) {
                t.movingSince = {};
                continue;
            }
            const auto &m = p.mDeviceToAbsoluteTracking.m;
            for (int k = 0; k < 3; ++k) t.w.sum[k] += m[k][3], t.w.sq[k] += double(m[k][3]) * m[k][3];
            ++t.w.n;
            const auto &v = p.vVelocity.v, &a = p.vAngularVelocity.v;
            const double speed = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
            const double spin = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
            t.w.speedSum += speed, t.w.speedMax = std::max(t.w.speedMax, speed);
            t.w.spinSum += spin, t.w.spinMax = std::max(t.w.spinMax, spin);
            t.speeds.push_back(speed), t.spins.push_back(spin);
            if (speed > 0.15 || spin > 1.0) {
                if (t.movingSince == Clock::time_point{}) t.movingSince = now, t.counted = false;
                if (!t.counted && now - t.movingSince >= std::chrono::milliseconds(100)) ++t.w.movingRuns, t.counted = true;
            } else {
                t.movingSince = {};
            }
        }
        if (now - lastPrint >= std::chrono::seconds(1)) {
            lastPrint = now;
            const double at = std::chrono::duration<double>(now - start).count();
            for (auto &[i, t] : tracks) {
                vr::ETrackedPropertyError pe;
                const bool charging = sys->GetBoolTrackedDeviceProperty(i, vr::Prop_DeviceIsCharging_Bool, &pe);
                const auto level = sys->GetTrackedDeviceActivityLevel(i);
                double spread = 0;
                if (t.w.n > 1)
                    for (int k = 0; k < 3; ++k) {
                        const double mean = t.w.sum[k] / t.w.n;
                        spread = std::max(spread, std::sqrt(std::max(0.0, t.w.sq[k] / t.w.n - mean * mean)));
                    }
                if (t.w.n > 1) t.spreads.push_back(spread * 1000);
                if (!quiet)
                    std::printf("%7.1f s  [%s] %s activity=%s charging=%d%s  spread %.2f mm  speed %.3f/%.3f m/s  "
                                "spin %.2f/%.2f rad/s  moving %d\n",
                                at, t.name.c_str(), Result(all[i].eTrackingResult), Activity(level), charging,
                                pe == vr::TrackedProp_Success ? "" : "(?)", spread * 1000,
                                t.w.n ? t.w.speedSum / t.w.n : 0, t.w.speedMax, t.w.n ? t.w.spinSum / t.w.n : 0,
                                t.w.spinMax, t.w.movingRuns);
                t.w = Window{};
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
    }
    std::printf("summary over %.0f s:\n", std::chrono::duration<double>(Clock::now() - start).count());
    for (auto &[i, t] : tracks)
        std::printf("  [%s] 1 s spread p50 %.2f mm p99 %.2f mm (%zu s)  speed p99 %.3f m/s  spin p99 %.2f rad/s\n",
                    t.name.c_str(), Pct(t.spreads, 0.5), Pct(t.spreads, 0.99), t.spreads.size(), Pct(t.speeds, 0.99),
                    Pct(t.spins, 0.99));
    vr::VR_Shutdown();
    return 0;
}
