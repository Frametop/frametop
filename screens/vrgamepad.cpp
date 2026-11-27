// Gamepad mode for a panel (user, 2026-10-24: "there will be an 'Enter Gamepad Mode' above the Frametop window.
// When you click on it, interactions with that display should then be done through the gamepad"): its pill, and
// what the mode does here. When it's on and what ends it is padmode.h's.
//
// The pill shows over the panel typing goes to (compositor.c's keys_update: the desktop screen or floating
// window KWin has keyboard focus on, a remote display, or a gamescope app): over a gamescope app's panel
// whenever it has the typing, so a click on a game is enough (user, 2026-11-27: "without having to open the
// Steam menu"), over the others while the Steam menu is open. A laser's click on it starts the mode for that
// panel: no laser of ours on any panel (vr.cpp's LasersByMode), so SteamVR leaves the controllers alone, and the
// input relay hears "gamepad desktop|game|remote" every second (the panel's kind; "gamepad off" as it ends) and
// reads the controllers raw into a pad (input/vrpad.py). A gamescope app has a pad of its own there ("game"): a
// game Steam started opens no other (2026-11-27). Steam's own page reads the desktop's. With the Steam menu
// open at the click, ft-dashpatch closes it first ("gamepad"): an open dashboard lasers everything.
//
// While it's on the pill says how to leave: both grips squeezed. The relay sees them and says "padmode off"
// here, as it does when the mouse is used; ft-dashpatch then has SteamVR bring its laser back ("laser"). The
// Steam menu opening ends it too, and so do its panel going, a pause and a VR game.
//
// Until 2026-11-27 the mode was SteamVR's own, for a dashboard overlay of ours (frametop.gamepad) with the
// dashboard dimmed on its tab: a card floated in front of the wearer for as long as it lasted, the grips did
// nothing, and SteamVR's input focus was the panel clicked last all the same (padmode.h).
#include "vr.h"
#include "vrpanel.h"
#include "fillround.h"
#include "label.h"
#include "overlaytex.h"
#include "padmode.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ftvr {
namespace {

struct GamepadPill {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;  // frametop.gamepad.pill
    int target = -1;          // the panel typing goes to, -1 Steam
    int pad = -1;             // the panel the relay was told has the pad, -1 none
    int64_t padSaidMs = 0;    // ...then
    int shown = -1;           // the panel it shows over now, -1 hidden
    std::string drawn;        // the words it shows ("" none drawn)
    bool drawnLit = false;
    bool hover = false;       // a laser is on it
    bool lasers = false;      // MakeOverlaysInteractiveIfVisible is set
    int px = 0;               // its texture's width
} g_pill;
padmode::Mode g_mode;
constexpr int kPillH = 64, kPillText = 30;
constexpr const char *kPillKey = "frametop.gamepad.pill";
constexpr const char *kEnterLabel = "Enter Gamepad Mode", *kLeaveLabel = "Gamepad mode: squeeze both grips to leave";

void HidePill() {
    if (g_pill.shown < 0) return;
    PinHide(g_pill.overlay);
    g_pill.shown = -1, g_pill.hover = false;
}

void DrawPill(const char *text, bool lit) {
    const int H = kPillH, W = label::Width(text, kPillText) + 2 * 30;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    // SteamVR's dark pill, white under a laser
    const uint8_t bg[4] = {uint8_t(lit ? 240 : 24), uint8_t(lit ? 240 : 24), uint8_t(lit ? 244 : 30), uint8_t(lit ? 240 : 200)};
    const uint8_t fg[3] = {uint8_t(lit ? 20 : 245), uint8_t(lit ? 20 : 245), uint8_t(lit ? 24 : 250)};
    FillRound(px, W, H, 2, 2, W - 2, H - 2, (H - 4) / 2, bg);
    label::Draw(px.data(), W, H, text, kPillText, W / 2, H / 2, 255, fg);
    overlaytex::Set(g_pill.overlay, px.data(), W, H);
    const vr::HmdVector2_t scale = {{float(W), float(H)}};
    vr::VROverlay()->SetOverlayMouseScale(g_pill.overlay, &scale);
    g_pill.px = W, g_pill.drawn = text, g_pill.drawnLit = lit;
}

// The mode ended (`why`): said once. `laser`: SteamVR is asked for its laser back.
void Ended(const char *why, bool laser) {
    std::printf("gamepad mode: over (%s)\n", why);
    std::fflush(stdout);
    if (laser) SendDashpatch("laser");
}

}  // namespace

int PadPanel() { return g_mode.For(); }
bool PadHoldsLasers() { return g_mode.Holds(); }
bool PadLaserGrace() { return g_mode.Grace(NowMs()); }
int TypingPanel() { return g_pill.target; }

// Each tick: over its panel's top edge, centred; or hidden. Its size is the same angle at any distance, as the
// carousel's.
void UpdatePill() {
    const bool on = g_mode.Holds();
    const auto it = g_screens.find(on ? g_mode.For() : g_pill.target);
    if (it == g_screens.end() || !it->second.visible || g_paused || g_gameRunning) return HidePill();
    const Screen &s = it->second;
    if (!on && !(s.gsapp && !s.steamDash) && !vr::VROverlay()->IsDashboardVisible()) return HidePill();
    Mat p, head;
    if (!ScreenPose(s, &p)) return HidePill();
    if (g_pill.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kPillKey, "Frametop: Enter Gamepad Mode", &g_pill.overlay) != vr::VROverlayError_None)
            return;
        vr::VROverlay()->SetOverlayInputMethod(g_pill.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlaySortOrder(g_pill.overlay, 10);
        vr::VROverlay()->SetOverlayFlag(g_pill.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);   // (a carry's wheel over it: PollOverlay)
        AnnounceOverlay(kPillKey);   // (the 3D mouse's ray finds it)
    }
    const char *text = on ? kLeaveLabel : kEnterLabel;
    const bool lit = g_pill.hover && !on;
    if (g_pill.drawn != text || g_pill.drawnLit != lit) DrawPill(text, lit);
    if (g_pill.lasers != s.lasers) {
        g_pill.lasers = s.lasers;
        vr::VROverlay()->SetOverlayFlag(g_pill.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
    }
    // 0.45 mm a pixel at 1 m (30 px text: about 0.8 deg), a third of its height over the edge
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    const double mpp = 0.00045 * d, h = kPillH * mpp;
    vr::VROverlay()->SetOverlayWidthInMeters(g_pill.overlay, float(g_pill.px * mpp));
    const Mat at = Mul(p, OnSurface(s, 0, s.heightMetres() / 2 + h / 3 + h / 2, 0.003));
    vr::VROverlay()->SetOverlayTransformAbsolute(g_pill.overlay, vr::TrackingUniverseStanding, &at);
    if (g_pill.shown != it->first) {
        PinShow(g_pill.overlay);
        g_pill.shown = it->first;
    }
}

// Its input: a laser over it lights it, a click starts gamepad mode for its panel.
void PollPill(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (PollOverlay(g_pill.overlay, &ev)) {
        if (ev.eventType == vr::VREvent_FocusEnter || ev.eventType == vr::VREvent_MouseMove) {
            g_pill.hover = true;
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_pill.hover = false;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left) {
            if (g_pill.shown < 0 || g_mode.Holds()) continue;
            const bool menu = g_mode.Click(g_pill.shown, vr::VROverlay()->IsDashboardVisible(), NowMs());
            if (menu) SendDashpatch("gamepad");
            std::printf("gamepad pill: clicked (%s): gamepad mode for panel %d%s\n", DeviceLabel(ev.trackedDeviceIndex).c_str(),
                        g_pill.shown + 1, menu ? ", once the Steam menu has closed" : "");
            std::fflush(stdout);
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
}

// Each tick: the mode's own ends, and while it's on the relay hears every second which kind of panel has the
// pad, and once as it ends.
void UpdatePad() {
    const int64_t now = NowMs();
    const auto on = g_screens.find(g_mode.For());
    if (const char *why = g_mode.Tick(vr::VROverlay()->IsDashboardVisible(), on != g_screens.end() && on->second.visible,
                                      g_paused || g_gameRunning, now))
        Ended(why, false);
    const int want = g_mode.Panel();
    if (want == g_pill.pad && (want < 0 || now - g_pill.padSaidMs < 1000)) return;
    const bool remote = want >= 0 && g_screens[want].remote;
    const bool game = want >= 0 && g_screens[want].gsapp && !g_screens[want].steamDash;
    if (want != g_pill.pad) {
        if (want >= 0) {
            std::printf("gamepad mode: the pad is for panel %d (%s)\n", want + 1,
                        remote ? "a remote display" : game ? "a gamescope app" : "the desktop");
            std::fflush(stdout);
        }
        // The remote display that had the pad lets it go now: the relay's own "pad off" comes after
        // our "gamepad off" below, once the pad is no screen's, and compositor.c drops it (issue 105:
        // the host kept an idle Xbox pad for about 30 s).
        if (g_pill.pad >= 0 && g_screens.count(g_pill.pad) && g_screens[g_pill.pad].remote)
            ft_remote_pad(g_pill.pad, "pad off");
    }
    g_pill.pad = want, g_pill.padSaidMs = now;
    SendRelay(want < 0 ? "gamepad off" : remote ? "gamepad remote" : game ? "gamepad game" : "gamepad desktop");
}

// "padmode off <why>" on @ft_screens: the relay's (both grips, the mouse).
bool PadLeave(const char *why) {
    if (!g_mode.Leave(NowMs())) return false;
    Ended(why, true);
    return true;
}

// The "gamepadpill" command's reply.
void PadState(char *reply, int size) {
    std::snprintf(reply, size, "ok over %d shown %d pad %d ours %d lasers %d hover %d", g_pill.target + 1, g_pill.shown + 1,
                  g_pill.pad + 1, g_mode.Holds() ? 1 : 0, g_pill.lasers ? 1 : 0, g_pill.hover ? 1 : 0);
}

}  // namespace ftvr

using namespace ftvr;

void ft_vr_gamepad_target(int screen) {
    if (!g_vr || g_pill.target == screen) return;
    g_pill.target = screen;
    std::printf("gamepad pill: over %s\n", screen < 0 ? "nothing (typing goes to Steam)" : ("panel " + std::to_string(screen + 1)).c_str());
    std::fflush(stdout);
}

int ft_vr_gamepad_screen(void) { return g_vr ? g_pill.pad : -1; }
