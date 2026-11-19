// Reset Layout... in a panel's bar menu asks first (user, 2026-10-13: "add a confirmation dialogue you have to
// tap to reset the displays"; then for a hand's tap only, now for a laser's click too: the reset button became
// a menu row, 2026-11-18, and one click there is as easy to make by mistake as a stray poke was): "Reset the
// layout?" shows over the panel beside its bar for kResetAskMs, and a click or a fingertip's tap on it resets.
// The question's geometry is barside.h's (MenuAt, as the bar's menu); its tap, tiptap.h's.
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"
#include "tiptap.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ftvr {
namespace {

struct ResetAsk {
    int panel = -1;
    int64_t shownMs = -1;
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    double w = 0, h = 0;                  // its size now (m)
    Mat pose{};                           // ...and where it is
    tiptap::Tap tip[2];                   // each index tip's way through it
    int px = 0;                           // its width in pixels
    bool lasers = false;                  // MakeOverlaysInteractiveIfVisible is set
} g_resetAsk;
constexpr int64_t kResetAskMs = 5000, kResetAskLeastMs = 300;   // (the least: the press that opened it isn't the answer)
constexpr int kResetAskH = 84;
constexpr const char *kResetAskKey = "frametop.resetask", *kResetAskText = "Reset the layout? Tap to confirm";

// Beside the bar's middle, toward the panel's middle (where its menu was), and at least a finger's size.
bool PlaceResetAsk(Screen &s) {
    Mat p, head;
    if (!ScreenPose(s, &p)) return false;
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    g_resetAsk.h = std::max(0.00045 * d * kResetAskH, 1.4 * s.grip);
    g_resetAsk.w = g_resetAsk.h * g_resetAsk.px / kResetAskH;
    const barside::UV at = barside::MenuAt(s.barSide, BarSpot(s, 0), barhit::Height(s.chrome, g_barPaddingY) / 2,
                                           std::max(s.grip, s.chrome * 0.06), g_resetAsk.w, g_resetAsk.h);
    g_resetAsk.pose = Mul(p, OnSurface(s, at.u, at.v, 0.004));
    const vr::VROverlayHandle_t o = g_resetAsk.overlay;
    vr::VROverlay()->SetOverlayWidthInMeters(o, float(g_resetAsk.w));
    vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &g_resetAsk.pose);
    if (g_resetAsk.lasers != s.lasers) {
        g_resetAsk.lasers = s.lasers;
        vr::VROverlay()->SetOverlayFlag(o, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
    }
    return true;
}

}  // namespace

void HideResetAsk() {
    if (g_resetAsk.overlay != vr::k_ulOverlayHandleInvalid) PinHide(g_resetAsk.overlay);
    g_resetAsk.panel = -1, g_resetAsk.shownMs = -1;
}

void ShowResetAsk(int panel) {
    const auto it = g_screens.find(panel);
    if (it == g_screens.end()) return;
    const int H = kResetAskH, W = label::Width(kResetAskText, 32) + 2 * 40;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    const uint8_t bg[4] = {196, 52, 48, 240}, fg[3] = {255, 255, 255};
    FillRound(px, W, H, 2, 2, W - 2, H - 2, (H - 4) / 2, bg);
    label::Draw(px.data(), W, H, kResetAskText, 32, W / 2, H / 2, 255, fg);
    if (g_resetAsk.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kResetAskKey, "Frametop: reset the layout?", &g_resetAsk.overlay) != vr::VROverlayError_None) {
            g_resetAsk.overlay = vr::k_ulOverlayHandleInvalid;
            return;
        }
        vr::VROverlay()->SetOverlayInputMethod(g_resetAsk.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlaySortOrder(g_resetAsk.overlay, 10);
        AnnounceOverlay(kResetAskKey);   // (the 3D mouse's ray finds it)
    }
    overlaytex::Set(g_resetAsk.overlay, px.data(), W, H);
    const vr::HmdVector2_t scale = {{float(W), float(H)}};
    vr::VROverlay()->SetOverlayMouseScale(g_resetAsk.overlay, &scale);
    g_resetAsk.panel = panel, g_resetAsk.shownMs = NowMs(), g_resetAsk.px = W;
    for (tiptap::Tap &t : g_resetAsk.tip) t.Reset();
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    PinShow(g_resetAsk.overlay);
    std::printf("screen %d: reset the layout? (asks, %.0f s)\n", panel + 1, kResetAskMs / 1000.0);
}

// Each tick: it follows its panel, and goes after kResetAskMs; each hands frame, an index tip coming through
// it from the front is the tap.
void UpdateResetAsk() {
    if (g_resetAsk.panel < 0) return;
    const auto it = g_screens.find(g_resetAsk.panel);
    if (it == g_screens.end() || !it->second.visible || NowMs() - g_resetAsk.shownMs >= kResetAskMs) {
        if (it != g_screens.end()) std::printf("screen %d: reset the layout? not confirmed: no reset\n", g_resetAsk.panel + 1);
        return HideResetAsk();
    }
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    if (!HandsFrame()) return;
    for (int k = 0; k < 2; ++k) {
        double l[3];
        const bool seen = IndexTipIn(k, g_resetAsk.pose, l);
        if (g_resetAsk.tip[k].Step(seen ? l : nullptr, g_resetAsk.w / 2, g_resetAsk.h / 2) &&
            NowMs() - g_resetAsk.shownMs >= kResetAskLeastMs) {
            const int panel = g_resetAsk.panel;
            return DoReset(panel, k ? "the right hand, confirmed" : "the left hand, confirmed");
        }
    }
}

// Its input: a click on it resets (not the press that asked, if it lands here: kResetAskLeastMs).
void PollResetAsk(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (g_resetAsk.overlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->PollNextOverlayEvent(g_resetAsk.overlay, &ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left && g_resetAsk.panel >= 0 &&
            NowMs() - g_resetAsk.shownMs >= kResetAskLeastMs) {
            DoReset(g_resetAsk.panel, ("confirmed by the " + DeviceLabel(ev.trackedDeviceIndex)).c_str());
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
}

}  // namespace ftvr
