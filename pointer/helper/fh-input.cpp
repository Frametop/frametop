// Dashboard-only hand ray and pinch input. The SteamVR driver remains disconnected
// until a bare hand is pointing at the dashboard; no game input is synthesized.
#include "../../config/ftsock.h"
#include "../../renderer/handcut.h"

#include <openvr.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
struct V { double x, y, z; };
V Position(const vr::HmdMatrix34_t &m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }
V operator-(V a, V b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V operator+(V a, V b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V operator*(V a, double k) { return {a.x*k, a.y*k, a.z*k}; }
double Length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
V Point(const float p[3]) { return {p[0], p[1], p[2]}; }
V Local(const vr::HmdMatrix34_t &m, V p) {
    return {m.m[0][3]+m.m[0][0]*p.x+m.m[0][1]*p.y+m.m[0][2]*p.z,
            m.m[1][3]+m.m[1][0]*p.x+m.m[1][1]*p.y+m.m[1][2]*p.z,
            m.m[2][3]+m.m[2][0]*p.x+m.m[2][1]*p.y+m.m[2][2]*p.z};
}
double Dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
bool Send(int fd, const char *message) {
    sockaddr_un addr;
    return sendto(fd, message, std::strlen(message), MSG_NOSIGNAL,
                  reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(FT_SOCK_POINTER, &addr)) >= 0;
}
struct Controller {
    V last{};
    int64_t at = 0, busyUntil = 0;
};
bool PhysicalControllerBusy(const vr::TrackedDevicePose_t *poses, const handcut::Hand &hand,
                            Controller *state, int64_t now) {
    V palm; double p[3]; hand.Palm(p); palm = {p[0], p[1], p[2]};
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
        char type[64] = "";
        vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
        if (!std::strcmp(type, "fh_pointer")) continue;
        const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);
        if (role != (hand.right ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand)) continue;
        if (!poses[i].bPoseIsValid || poses[i].eTrackingResult != vr::TrackingResult_Running_OK) continue;
        V pos = Position(poses[i].mDeviceToAbsoluteTracking);
        if (Length(pos-palm) < 0.15) state[i].busyUntil = now + 3'000'000'000ll;
        if (state[i].at && now-state[i].at < 250'000'000ll && Length(pos-state[i].last) > 0.025)
            state[i].busyUntil = now + 3'000'000'000ll;
        state[i].last = pos; state[i].at = now;
        if (now < state[i].busyUntil) return true;
    }
    return false;
}
}

int main() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) return std::fprintf(stderr, "fh-input: %s\n",
        vr::VR_GetVRInitErrorAsEnglishDescription(error)), 1;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::perror("fh-input socket"), 1;
    handcut::Hands hands;
    Controller controllers[vr::k_unMaxTrackedDeviceCount]{};
    int owner = -1;
    bool showing = false, pressed = false, initialized[2]{};
    uint32_t begins[2]{};
    int64_t pressAt = 0, pending[2]{};
    while (vr::VRSystem()->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) {
        const int64_t now = NowNs();
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0,
                                                        poses, vr::k_unMaxTrackedDeviceCount);
        const bool dashboard = vr::VROverlay()->IsDashboardVisible();
        const bool fresh = poses[0].bPoseIsValid && hands.Update(poses[0].mDeviceToAbsoluteTracking, now);
        const handcut::Hand *side[2]{};
        if (fresh) for (const auto &h : hands.shown()) side[h.right ? 1 : 0] = &h;
        bool ready[2]{};
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (!initialized[s]) begins[s] = g.begins, initialized[s] = true;
            if (dashboard && side[s] && side[s]->stereo && !hands.grip(s).down) {
                const auto &headPose = poses[0].mDeviceToAbsoluteTracking;
                const V knuckle = (Point(side[s]->p[5])+Point(side[s]->p[9]))*0.5;
                const V fromHead = knuckle-Position(headPose);
                const V forward{-headPose.m[0][2],-headPose.m[1][2],-headPose.m[2][2]};
                ready[s] = Dot(fromHead, forward) > 0.22 && fromHead.y > -0.45 &&
                           !PhysicalControllerBusy(poses, *side[s], controllers, now);
            }
        }
        int pick = owner >= 0 && ready[owner] ? owner : ready[1] ? 1 : ready[0] ? 0 : -1;
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (g.begins != begins[s]) {
                begins[s] = g.begins;
                if (ready[s] && !pressed) pick = s, pending[s] = now;
            }
        }
        if (pick != owner || pick < 0) {
            if (pressed) Send(fd, "btn trigger 0"), pressed = false;
            if (showing) Send(fd, "hide"), showing = false;
            owner = pick;
        }
        if (owner >= 0) {
            const auto &h = *side[owner];
            const auto &g = hands.pinch(owner);
            const V shoulder = Local(poses[0].mDeviceToAbsoluteTracking,
                                     {owner ? 0.17 : -0.17, -0.20, 0.08});
            const V knuckle = (Point(h.p[5])+Point(h.p[9])) * 0.5;
            V dir = knuckle-shoulder;
            const double distance = Length(dir);
            if (distance > 0.15) {
                dir = dir * (1.0/distance);
                const V origin = knuckle + dir*0.03;
                char pose[160];
                std::snprintf(pose, sizeof pose, "pose %.5f %.5f %.5f %.3f %.3f", origin.x, origin.y, origin.z,
                              std::atan2(-dir.x,-dir.z)*180/M_PI, std::asin(std::clamp(dir.y,-1.0,1.0))*180/M_PI);
                Send(fd, pose);
                if (!showing) {
                    Send(fd, owner ? "role right" : "role left");
                    showing = Send(fd, "show");
                    if (showing) Send(fd, "btn a 1"), Send(fd, "btn a 0"); // claim the dashboard laser
                }
                if (showing && !pressed && pending[owner] && now-pending[owner] < 200'000'000ll) {
                    Send(fd, "btn trigger 1"); pressed = true; pressAt = now; pending[owner] = 0;
                }
                if (pressed && (!g.down || now-g.beginNs > 4'000'000'000ll) && now-pressAt > 50'000'000ll)
                    Send(fd, "btn trigger 0"), pressed = false;
            }
        }
        vr::VREvent_t event;
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit) goto done;
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }
done:
    Send(fd, "btn trigger 0"); Send(fd, "hide");
    close(fd);
    vr::VR_Shutdown();
    return 0;
}
