// Steam's own overlays out of the way (vrsteamhit.h): shrunk (steamshrink.h).
#include "vrsteamhit.h"
#include "steamshrink.h"
#include "vrpanel.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace ftvr {
namespace steamhit {
namespace {

std::set<uint64_t> g_gone;   // Steam's overlays shrunk now

void Width(uint64_t steams, float w, const char *why) {
    if (w <= 0) return;   // (already as wanted)
    const vr::EVROverlayError e = vr::VROverlay()->SetOverlayWidthInMeters(steams, w);
    if (e != vr::VROverlayError_None && e != vr::VROverlayError_UnknownOverlay)
        std::printf("Steam's overlay 0x%llx: not %s (%s)\n", (unsigned long long)steams, why,
                    vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
}
float WidthOf(uint64_t steams) {
    float w = 0;
    return vr::VROverlay()->GetOverlayWidthInMeters(steams, &w) == vr::VROverlayError_None ? w : 0;
}
void Away(uint64_t steams) { Width(steams, steamshrink::Away(WidthOf(steams)), "shrunk"); }
void Back(uint64_t steams) { Width(steams, steamshrink::Back(WidthOf(steams)), "given its width back"); }

void Become(std::set<uint64_t> want, bool all) {
    for (const uint64_t h : want)
        if (all || !g_gone.count(h)) Away(h);
    for (const uint64_t h : g_gone)
        if (!want.count(h)) Back(h);
    g_gone = std::move(want);
}

}  // namespace

void Step(const steamwin::Fleet &fleet, bool all) {
    std::set<uint64_t> want;
    for (const auto &[id, t] : fleet.Windows()) {
        if (!t.Steams()) continue;
        if (t.Panel()) want.insert(t.Steams());
        else if (all) Back(t.Steams());
    }
    Become(std::move(want), all);
}

void Clear() { Become({}, false); }

void Sweep() {
    // (OpenVR can't list other apps' overlays: SteamVR's vrcmd can, as ft-pointer's OverlayList)
    FILE *p = popen("LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null", "r");
    if (!p) return;
    char line[1024];
    int back = 0;
    while (std::fgets(line, sizeof line, p)) {   // 'key' -- 'name', ...
        static const char kSteams[] = "'valve.steam.";
        if (std::strncmp(line, kSteams, sizeof kSteams - 1) != 0) continue;
        const char *end = std::strchr(line + 1, '\'');
        if (!end) continue;
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
        if (vr::VROverlay()->FindOverlay(std::string(line + 1, size_t(end - (line + 1))).c_str(), &h) != vr::VROverlayError_None) continue;
        if (steamshrink::Shrunk(WidthOf(h))) Back(h), ++back;
    }
    pclose(p);
    if (back) std::printf("Steam's overlays: %d left shrunk (an ft-screens that went?) given their widths back\n", back);
}

}  // namespace steamhit
}  // namespace ftvr
