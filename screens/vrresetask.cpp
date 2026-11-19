// Reset Layout... in a panel's bar menu asks first (user, 2026-10-13: "add a confirmation dialogue you have to
// tap to reset the displays"; then for a hand's tap only, now for a laser's click too: the reset button became
// a menu row, 2026-11-18, and one click there is as easy to make by mistake as a stray poke was): Steam's
// dialog as the Option C design draws it (user, 2026-11-19: "Use that as the design"; barmenuart::ResetAsk),
// "Reset the layout?" with Cancel and Reset, shows over the panel beside its bar for kResetAskMs. Focus starts
// on Cancel; a click or a fingertip's tap on Reset resets, on Cancel it goes.
// The question's geometry is barside.h's (MenuAt, as the bar's menu); its tap, tiptap.h's.
#include "vrpanel.h"
#include "barmenuart.h"
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
    barmenuart::Ask art;                  // its pixels and buttons
    int drawnLit = -2;                    // the button lit in them
    int hover = -1, handHover = -1;       // 0 Cancel, 1 Reset under a laser, a fingertip near; -1 neither
    bool lasers = false;                  // MakeOverlaysInteractiveIfVisible is set
} g_resetAsk;
constexpr int64_t kResetAskMs = 5000, kResetAskLeastMs = 300;   // (the least: the press that opened it isn't the answer)
constexpr const char *kResetAskKey = "frametop.resetask";
constexpr const char *kResetAskBody = "Your screens go back to their layout around where you are now. Windows and sizes stay as they are.";

// The button under a texture point (y down), -1 none.
int ButtonAt(double x, double y) {
    return g_resetAsk.art.reset.Has(x, y) ? 1 : g_resetAsk.art.cancel.Has(x, y) ? 0 : -1;
}
// ...under a point on its face (metres from its centre, y up).
int ButtonAtFace(double x, double y) {
    if (g_resetAsk.w <= 0 || g_resetAsk.h <= 0) return -1;
    return ButtonAt((x / g_resetAsk.w + 0.5) * g_resetAsk.art.w, (0.5 - y / g_resetAsk.h) * g_resetAsk.art.h);
}

void Draw() {
    const int lit = g_resetAsk.hover >= 0 ? g_resetAsk.hover : g_resetAsk.handHover;
    if (lit == g_resetAsk.drawnLit) return;
    g_resetAsk.art = barmenuart::ResetAsk(kResetAskBody, lit);
    const barmenuart::Ask &a = g_resetAsk.art;
    overlaytex::Set(g_resetAsk.overlay, a.px.data(), a.w, a.h);
    const vr::HmdVector2_t scale = {{float(a.w), float(a.h)}};
    vr::VROverlay()->SetOverlayMouseScale(g_resetAsk.overlay, &scale);
    g_resetAsk.drawnLit = lit;
}

// Beside the bar's middle, toward the panel's middle (where its menu was); its buttons at least a finger's size.
bool PlaceResetAsk(Screen &s) {
    Mat p, head;
    if (!ScreenPose(s, &p)) return false;
    const barmenuart::Ask &a = g_resetAsk.art;
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    const double buttonPx = a.cancel.y1 - a.cancel.y0, mpp = std::max(0.0004 * d, 1.4 * s.grip / buttonPx);
    g_resetAsk.w = a.w * mpp, g_resetAsk.h = a.h * mpp;
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

// A button pressed: Reset resets, Cancel closes it.
void Answer(int button, const std::string &by) {
    const int panel = g_resetAsk.panel;
    if (button == 1) return DoReset(panel, ("confirmed by the " + by).c_str());
    if (button == 0) {
        std::printf("screen %d: reset the layout? cancelled by the %s: no reset\n", panel + 1, by.c_str());
        HideResetAsk();
    }
}

}  // namespace

void HideResetAsk() {
    if (g_resetAsk.overlay != vr::k_ulOverlayHandleInvalid) PinHide(g_resetAsk.overlay);
    g_resetAsk.panel = -1, g_resetAsk.shownMs = -1;
}

void ShowResetAsk(int panel) {
    const auto it = g_screens.find(panel);
    if (it == g_screens.end()) return;
    if (g_resetAsk.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kResetAskKey, "Frametop: reset the layout?", &g_resetAsk.overlay) != vr::VROverlayError_None) {
            g_resetAsk.overlay = vr::k_ulOverlayHandleInvalid;
            return;
        }
        vr::VROverlay()->SetOverlayInputMethod(g_resetAsk.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlaySortOrder(g_resetAsk.overlay, 10);
        AnnounceOverlay(kResetAskKey);   // (the 3D mouse's ray finds it)
    }
    g_resetAsk.panel = panel, g_resetAsk.shownMs = NowMs();
    g_resetAsk.hover = g_resetAsk.handHover = -1, g_resetAsk.drawnLit = -2;
    for (tiptap::Tap &t : g_resetAsk.tip) t.Reset();
    Draw();
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    PinShow(g_resetAsk.overlay);
    std::printf("screen %d: reset the layout? (asks, %.0f s)\n", panel + 1, kResetAskMs / 1000.0);
}

// Each tick: it follows its panel, and goes after kResetAskMs; each hands frame, a fingertip near a button
// lights it, and one coming through it from the front presses it.
void UpdateResetAsk() {
    if (g_resetAsk.panel < 0) return;
    const auto it = g_screens.find(g_resetAsk.panel);
    if (it == g_screens.end() || !it->second.visible || NowMs() - g_resetAsk.shownMs >= kResetAskMs) {
        if (it != g_screens.end()) std::printf("screen %d: reset the layout? not confirmed: no reset\n", g_resetAsk.panel + 1);
        return HideResetAsk();
    }
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    if (HandsFrame()) {
        g_resetAsk.handHover = -1;
        for (int k = 0; k < 2; ++k) {
            double l[3];
            const bool seen = IndexTipIn(k, g_resetAsk.pose, l);
            const bool through = g_resetAsk.tip[k].Step(seen ? l : nullptr, g_resetAsk.w / 2, g_resetAsk.h / 2);
            if (seen && g_resetAsk.tip[k].Near()) g_resetAsk.handHover = ButtonAtFace(l[0], l[1]);
            if (through && NowMs() - g_resetAsk.shownMs >= kResetAskLeastMs) {
                const int b = ButtonAtFace(l[0], l[1]);
                Answer(b, k ? "right hand" : "left hand");
                if (b >= 0) return;
            }
        }
    }
    Draw();
}

// Its input: a laser on a button lights it; a click on Reset resets, on Cancel closes it (not the press that
// asked, if it lands here: kResetAskLeastMs).
void PollResetAsk(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (PollOverlay(g_resetAsk.overlay, &ev)) {
        // (OpenVR's mouse y is up from the bottom; FocusEnter's data isn't a point, the move after it lights)
        const double x = ev.data.mouse.x, y = g_resetAsk.art.h - ev.data.mouse.y;
        if (ev.eventType == vr::VREvent_MouseMove) {
            g_resetAsk.hover = ButtonAt(x, y);
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_resetAsk.hover = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left && g_resetAsk.panel >= 0 &&
                   NowMs() - g_resetAsk.shownMs >= kResetAskLeastMs) {
            Answer(ButtonAt(x, y), DeviceLabel(ev.trackedDeviceIndex));
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
    if (g_resetAsk.panel >= 0) Draw();
}

}  // namespace ftvr
