// Steam's popups as overlays of ours (vrsteamfollow.h; steamwin.h has why, steamplace.h the math).
#include "vrsteamfollow.h"

#include "vrpanel.h"
#include "steamplace.h"
#include "../conf/ft_pinlock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ftvr {
namespace steamfollow {
namespace {
constexpr int64_t kPlaceMs = 50;     // placed again this often (Steam's popups and our tiles move)
constexpr uint32_t kSortOrder = 11;  // over the tiles, under the gaze ring
int64_t g_placeAt = 0;

using steamplace::Frame;
using steamplace::V3;

V3 PointOf(uint64_t overlay, double x, double y) {
    vr::HmdMatrix34_t m{};
    const vr::HmdVector2_t at = {{float(x), float(y)}};
    vr::VROverlay()->GetTransformForOverlayCoordinates(overlay, vr::TrackingUniverseStanding, at, &m);
    return {m.m[0][3], m.m[1][3], m.m[2][3]};
}
// An overlay's frame in a w x h window's pixels (its mouse scale may be another size: read in that).
bool FrameOf(uint64_t overlay, double w, double h, Frame *f) {
    if (!overlay || w <= 0 || h <= 0) return false;
    vr::HmdVector2_t scale{};
    if (vr::VROverlay()->GetOverlayMouseScale(overlay, &scale) != vr::VROverlayError_None || scale.v[0] <= 0 ||
        scale.v[1] <= 0)
        scale = {{float(w), float(h)}};
    const double sx = scale.v[0], sy = scale.v[1];
    return steamplace::FromPoints(PointOf(overlay, sx / 2, sy / 2), PointOf(overlay, sx, sy / 2),
                                  PointOf(overlay, sx / 2, sy), w, h, f);
}
bool Visible(uint64_t overlay) { return overlay && vr::VROverlay()->IsOverlayVisible(overlay); }

// Where one goes now, and whether it shows: false to hide it.
bool Place(const steamwin::Tracker &t, const steamwin::Fleet &fleet) {
    const gsforward::Window &w = t.Win();
    if (!t.Mapped() || !Visible(t.Steams()) || !ft_pin_allowed()) return false;
    Frame steams;
    if (!FrameOf(t.Steams(), w.w, w.h, &steams)) return false;
    std::vector<steamplace::Panel> panels;
    std::vector<int> slots;
    for (int slot = 0; slot < steamwin::kTiles; ++slot) {
        const steamwin::Tracker *tile = fleet.TileIn(slot);
        Frame f;
        if (tile && FrameOf(tile->Steams(), tile->Win().w, tile->Win().h, &f))
            panels.push_back({f, double(tile->Win().w), double(tile->Win().h)}), slots.push_back(slot);
    }
    Frame at = steams;
    const int on = steamplace::SitsOn(steamplace::Apply(steams, {w.w / 2.0, w.h / 2.0, 0}), panels);
    if (on >= 0) {
        const steamwin::Tracker *tile = fleet.TileIn(slots[on]);
        const auto it = g_screens.find(kTileIndex + slots[on]);
        if (tile->Panel() && it != g_screens.end()) {   // a tile of ours: with it, and shown with it
            Frame ours;
            if (!it->second.visible || !FrameOf(it->second.overlay, tile->Win().w, tile->Win().h, &ours)) return false;
            if (!steamplace::Remap(ours, panels[on].f, steams, &at)) return false;
        }
    }
    float m[3][4];
    steamplace::ToOverlay(at, w.w, w.h, m);
    vr::HmdMatrix34_t pose;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) pose.m[r][c] = m[r][c];
    vr::VROverlay()->SetOverlayTransformAbsolute(t.Panel(), vr::TrackingUniverseStanding, &pose);
    return true;
}

void Events(const steamwin::Tracker &t, steamcdp::Pages &pages) {
    vr::VREvent_t ev;
    const gsforward::Window &w = t.Win();
    while (vr::VROverlay()->PollNextOverlayEvent(t.Panel(), &ev, sizeof ev)) {
        const int x = int(std::lround(ev.data.mouse.x)), y = w.h - int(std::lround(ev.data.mouse.y));
        switch (ev.eventType) {
            case vr::VREvent_MouseMove: pages.Move(w.title, x, y); break;
            case vr::VREvent_MouseButtonDown:
            case vr::VREvent_MouseButtonUp: {
                const bool down = ev.eventType == vr::VREvent_MouseButtonDown;
                steamcdp::Button b;
                if (!steamcdp::FromLinux(LinuxButton(ev.data.mouse.button), &b)) break;
                if (down) std::printf("Steam popup 0x%x: a click at %d,%d\n", w.id, x, y);
                pages.Press(w.title, x, y, b, down);
                break;
            }
            case vr::VREvent_ScrollDiscrete: pages.Wheel(w.title, int(std::lround(ev.data.scroll.ydelta))); break;
            case vr::VREvent_FocusLeave: pages.LetGo(); break;
            default: break;
        }
    }
}
}  // namespace

uint64_t Make(const gsforward::Window &w, uint32_t gamescopePid) {
    if (!gamescopePid) return 0;
    char key[64];
    std::snprintf(key, sizeof key, "frametop.steampopup.%x", w.id);
    vr::VROverlayHandle_t o = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->CreateOverlay(key, "Steam", &o) != vr::VROverlayError_None) return 0;
    vr::VROverlay()->SetOverlayInputMethod(o, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(o, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    vr::VROverlay()->SetOverlayFlag(o, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    vr::VROverlay()->SetOverlaySortOrder(o, kSortOrder);
    vr::VROverlay()->SetOverlayWidthInMeters(o, 1.0f);   // (its size in its transform, as Steam's own are)
    vr::VROverlay()->SetOverlayRenderingPid(o, gamescopePid);
    AnnounceOverlay(key);
    Size(o, w);
    g_placeAt = 0;
    return o;
}

void Size(uint64_t overlay, const gsforward::Window &w) {
    const vr::HmdVector2_t scale = {{float(std::max(1, w.w)), float(std::max(1, w.h))}};
    vr::VROverlay()->SetOverlayMouseScale(overlay, &scale);
    g_placeAt = 0;
}

void Drop(uint64_t overlay) { vr::VROverlay()->DestroyOverlay(overlay); }

void Step(const steamwin::Fleet &fleet, steamcdp::Pages &pages) {
    const int64_t now = NowMs();
    const bool place = now >= g_placeAt;
    if (place) g_placeAt = now + kPlaceMs;
    for (const auto &[id, t] : fleet.Windows()) {
        if (t.What().role != steamwin::Spec::Follow || !t.Panel()) continue;
        if (place) {
            if (Place(t, fleet)) vr::VROverlay()->ShowOverlay(t.Panel());
            else vr::VROverlay()->HideOverlay(t.Panel());
        }
        Events(t, pages);
    }
}

}  // namespace steamfollow
}  // namespace ftvr
