// The gamescope apps' panels (docs/design.md "Gamescope apps"): the windows of apps in SteamOS's
// own gamescope session (those started from the SteamVR dashboard: gamescope's app display,
// gsdisplay.h) as panels of ft-screens'. gsforward.h has how, gslink.h the X connection on a thread
// of its own, gscontrol.h (with gsapps.h's bookkeeping) what's done with each window and the input;
// this file is the panels, on OpenVR (vrpanel.h: the panel, and what it uses of vr.cpp's).
//
// gamescope puts each window's frames on its panel; where the panel is, its size, its controls and
// the dashboard tiles are a panel's like any other's (an app's has the bar, curve, roll, resize and
// anchor). The first time an app is seen its panel is anchored to the dashboard, staying shown;
// after that it comes back as the app was (TileKnown). GSAPP_PANELS=0 (Display Settings) leaves
// every window to gamescope's own panels.

#include "vrpanel.h"
#include "gscontrol.h"
#include "gsdisplay.h"
#include "gslink.h"
#include "../conf/ft_pinlock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace ftvr {
namespace {
constexpr double kGsMpp = 1.0 / 1250;   // a new app's density: 1250 pixels a metre
}  // namespace

// The window's size and place: the panel keeps its density (a resize by its corner changed that),
// and its tile or corner where it is.
void GsGeometry(Screen &s, int index, const gsforward::Window &w) {
    if (w.w <= 0 || w.h <= 0) return;
    const bool first = s.width <= 0;
    const double oldW = s.metres, oldH = s.heightMetres();
    if (!first) s.mpp = s.metres / s.width;
    s.gsX = w.x, s.gsY = w.y;
    if (w.w == s.width && w.h == s.height) return PlaceSubs(s);
    s.width = w.w, s.height = w.h;
    const vr::HmdVector2_t scale = {{float(w.w), float(w.h)}};
    vr::VROverlay()->SetOverlayMouseScale(s.overlay, &scale);
    s.metres = std::clamp(w.w * s.mpp, kMinWidth, 12.0);
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    CapCurve(s);
    ApplyCurve(s);
    if (TileAnchored(index)) RetileDash(false);
    else if (!first) KeepCorner(s, oldW, oldH);
    PlaceChrome(s);
    PlaceSubs(s);
    std::printf("screen %d: %dx%d (a gamescope app)\n", index + 1, w.w, w.h);
}

namespace {
gsapps::PointerOwner g_gsPointer;   // which laser has the X pointer (all the app panels share it)

// The apps' panels (gscontrol.h's Panels), on OpenVR.
class GsPanels : public gsapps::Panels {
public:
    // Hidden until it maps: 1.5 m ahead of the head until it's on the dashboard or put somewhere.
    uint64_t Make(int index, const gsforward::Window &w, uint32_t pid) override {
        if (g_screens.count(index)) return 0;
        Screen &s = g_screens[index];
        s.gsapp = true, s.gsWin = w.id, s.mpp = kGsMpp;
        s.metres = std::clamp(w.w * kGsMpp, kMinWidth, 12.0);
        char prefix[64];
        std::snprintf(prefix, sizeof prefix, "frametop.app.%d", index + 1);
        if (!MakePanel(s, prefix, "App")) return g_screens.erase(index), 0;
        AnnounceOverlay(prefix);
        vr::VROverlay()->SetOverlayRenderingPid(s.overlay, pid);   // (only its renderer may set its texture)
        Mat head;
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
        const double yaw = std::atan2(head.m[0][2], head.m[2][2]);
        SetAbsolute(s, PanelPose(head.m[0][3] - std::sin(yaw) * 1.5, head.m[1][3], head.m[2][3] - std::cos(yaw) * 1.5,
                                 yaw * 180 / M_PI, 0, 0));
        return s.overlay;
    }
    // Named, sized, back where its app was (or anchored, staying shown, the first time), shown.
    void Show(int index, const gsforward::Window &w) override {
        const auto it = g_screens.find(index);
        if (it == g_screens.end()) return;
        Screen &s = it->second;
        s.label = w.title;
        vr::VROverlay()->SetOverlayName(s.overlay, w.title.c_str());
        GsGeometry(s, index, w);
        s.shown = this;   // (no buffer of ours: gamescope's frames)
        const std::string key = w.key.empty() ? "" : dashtiles::Memory::Clean(w.key);
        const bool known = !key.empty() && !g_tileMemory.Get(key).empty();
        if (!key.empty()) TileKnown(index, w.key);
        if (!known && !s.gsSeen) TileAnchor(index, true, "a gamescope app");
        std::printf("screen %d: %s (%s, window 0x%x), a gamescope app%s\n", index + 1, w.title.c_str(),
                    key.empty() ? "no app id" : key.c_str(), w.id, s.gsSeen ? ", mapped again" : "");
        s.gsSeen = true;
        UpdateVisibility();
    }
    // Unmapped: hidden where it is (its place on the grid kept) until it maps again.
    void Hide(int index) override {
        const auto it = g_screens.find(index);
        if (it == g_screens.end()) return;
        if (g_mouseScreen == index) g_mouseScreen = -1;
        g_gsPointer.PanelGone(index);
        EndDrag(it->second);
        it->second.shown = nullptr;
        std::printf("screen %d: unmapped (a gamescope app): hidden until it maps again\n", index + 1);
        UpdateVisibility();
    }
    void Move(int index, const gsforward::Window &w) override {
        if (auto it = g_screens.find(index); it != g_screens.end()) GsGeometry(it->second, index, w);
    }
    void Drop(int index) override {
        if (g_mouseScreen == index) g_mouseScreen = -1;
        g_gsPointer.PanelGone(index);
        if (g_screens.count(index)) std::printf("screen %d: gone (a gamescope app)\n", index + 1);
        ft_vr_screen_destroy(index);
    }
    uint64_t MakePopup(int index, int sub, const gsforward::Window &w, uint32_t pid) override {
        const auto it = g_screens.find(index);
        if (it == g_screens.end() || it->second.subs.count(sub)) return 0;
        Screen &s = it->second;
        Sub o;
        char key[80];
        std::snprintf(key, sizeof key, "frametop.app.%d.sub.%d", index + 1, sub);
        if (vr::VROverlay()->CreateOverlay(key, "App menu", &o.overlay) != vr::VROverlayError_None) {
            std::printf("screen %d: no overlay for a popup (%s): gamescope's own panel has it\n", index + 1, key);
            return 0;
        }
        vr::VROverlay()->SetOverlayInputMethod(o.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayFlag(o.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        vr::VROverlay()->SetOverlayFlag(o.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
        vr::VROverlay()->SetOverlaySortOrder(o.overlay, 5);
        vr::VROverlay()->SetOverlayRenderingPid(o.overlay, pid);
        vr::VROverlay()->SetOverlayAlpha(o.overlay, s.alpha);
        AnnounceOverlay(key);
        if (s.visible) PinShow(o.overlay);
        s.subs[sub] = o;
        MovePopup(index, sub, w);
        return o.overlay;
    }
    // At its place on X's screen, over its app's panel.
    void MovePopup(int index, int sub, const gsforward::Window &w) override {
        const auto it = g_screens.find(index);
        if (it == g_screens.end()) return;
        const auto sit = it->second.subs.find(sub);
        if (sit == it->second.subs.end()) return;
        sit->second.x = w.x, sit->second.y = w.y, sit->second.w = w.w, sit->second.h = w.h;
        const vr::HmdVector2_t scale = {{float(std::max(1, w.w)), float(std::max(1, w.h))}};
        vr::VROverlay()->SetOverlayMouseScale(sit->second.overlay, &scale);
        PlaceSubs(it->second);
    }
    void DropPopup(int index, int sub) override {
        const auto it = g_screens.find(index);
        if (it == g_screens.end()) return;
        const auto sit = it->second.subs.find(sub);
        if (sit == it->second.subs.end()) return;
        vr::VROverlay()->DestroyOverlay(sit->second.overlay);
        it->second.subs.erase(sit);
        g_gsPointer.PanelGone(index, sub);   // (a menu item that closes its menu on the press)
    }
    // (PIN locked: nothing is, whatever the panel last said)
    bool Shown(int index) const override {
        const auto it = g_screens.find(index);
        return ft_pin_allowed() && it != g_screens.end() && it->second.visible;
    }
};

GsPanels g_gsPanels;
std::unique_ptr<gsforward::AsyncLink> g_gsLink;   // (made when first switched on: its thread)
std::unique_ptr<gsapps::Control> g_gsControl;     // (after it: goes first)
bool g_gsOn = false;                              // GSAPP_PANELS
}  // namespace

// Each tick: what happened on the app display since (gslink.h's thread opens it, every
// AsyncLink::kRetryMs while it's closed: gamescope comes and goes with Steam).
void GsStep() {
    if (!g_gsLink) return;
    const int64_t now = NowMs();
    for (gsforward::Update &u : g_gsLink->Take()) switch (u.type) {
            case gsforward::Update::Opened:
                g_gsControl->Opened(u.pid);
                std::printf("gamescope apps: on %s (gamescope %u)\n", u.display.c_str(), u.pid);
                break;
            case gsforward::Update::Closed:
                g_gsControl->Closed();
                if (g_gsOn && u.why != "off")
                    std::printf("gamescope apps: %s (trying again every %d s)\n", u.why.c_str(), gsforward::AsyncLink::kRetryMs / 1000);
                break;
            case gsforward::Update::Window:
                if (g_gsOn) g_gsControl->On(u.event, now);
                break;
        }
    if (g_gsOn) g_gsControl->Tick(now);
}

// Laser events on an app's panel and its popups: the X pointer, buttons and wheel there.
void GsPanelEvents(Screen &s, int index) {
    if (!g_gsControl) return;
    gsapps::Control &gs = *g_gsControl;
    auto one = [&](const vr::VREvent_t &ev, int sub, int ox, int oy, int h) {
        const int64_t now = NowMs();
        if (ev.eventType != vr::VREvent_FocusLeave) {
            s.inputMs = now;
            if (FromLaser(ev)) s.laserMs = now, s.laserDevice = ev.trackedDeviceIndex;
        }
        const int ours = OurPointer(ev.trackedDeviceIndex);
        if (ev.eventType == vr::VREvent_FocusLeave) {
            if (ours >= 0 && g_mouseScreen == index) g_mouseScreen = -1;
        } else if (ours > 0) {
            g_mouseScreen = index, g_mouseDevice = ev.trackedDeviceIndex;
        }
        // where on the X screen (OpenVR's y runs up from the bottom)
        const int x = ox + int(std::lround(ev.data.mouse.x)), y = oy + h - int(std::lround(ev.data.mouse.y));
        switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                if (s.toggleCarry || s.drag != Drag::None) return;
                if (!g_gsPointer.Move(ev.trackedDeviceIndex, now)) return;   // (another laser's, for now)
                gs.Point(index, sub, x, y);
                break;
            case vr::VREvent_MouseButtonDown:
            case vr::VREvent_MouseButtonUp: {
                if (CarryClick(ev)) return;
                const bool down = ev.eventType == vr::VREvent_MouseButtonDown;
                const uint32_t button = LinuxButton(ev.data.mouse.button);
                if (!down) {
                    EndDragsBy(ev.trackedDeviceIndex);
                    if (g_press.torn && ev.trackedDeviceIndex == g_press.device) {   // a modifier drag's: nowhere
                        g_press.buttons &= ~ButtonBit(button);
                        if (!g_press.buttons) g_press.upAt = -1, g_press.torn = false;
                        return;
                    }
                } else {
                    s.useMs = now;
                    if (g_meta || g_alt || g_ctrl || g_shift) SendRelay("notap");
                    if (ModCarry(s, index, ev.trackedDeviceIndex, button, ev.data.mouse.x, s.height - ev.data.mouse.y)) return;
                }
                g_gsPointer.Press(ev.trackedDeviceIndex, down, now, index, sub);
                gs.Press(index, sub, gsforward::XButton(int(button)), down, x, y);   // (a press: typing goes to it)
                break;
            }
            case vr::VREvent_ScrollDiscrete:
                if (CarryScroll(ev)) return;
                s.useMs = now;
                gs.Scroll(int(std::lround(ev.data.scroll.ydelta)));
                break;
            case vr::VREvent_FocusLeave:
                g_gsPointer.Left(ev.trackedDeviceIndex);
                if (gs.Holding() && s.drag == Drag::None) gs.LetGo();
                break;
            default:
                break;
        }
    };
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) one(ev, 0, s.gsX, s.gsY, s.height);
    for (const auto &[k, sub] : s.subs)
        while (vr::VROverlay()->PollNextOverlayEvent(sub.overlay, &ev, sizeof ev)) one(ev, k, sub.x, sub.y, sub.h);
}

void GsResize(int index, int w, int h) {
    if (g_gsControl) g_gsControl->Resize(index, w, h);
}

// GSAPP_PANELS: off, every window back to gamescope's own panel (keys and buttons held let go of).
void GsSwitch(bool on) {
    if (on == g_gsOn) return;
    g_gsOn = on;
    if (!g_gsLink) {
        if (!on) return;
        g_gsLink = std::make_unique<gsforward::AsyncLink>(gsforward::AppDisplays, true);
        g_gsControl = std::make_unique<gsapps::Control>(*g_gsLink, g_gsPanels);
    }
    if (!on) g_gsControl->Off(), std::printf("gamescope apps: off (GSAPP_PANELS)\n");
    g_gsLink->SetOn(on);   // (off: closed after the untags)
}

// ft-screens going: the windows back to gamescope first (half a second at most: a hung one isn't waited for).
void GsShutdown() {
    if (!g_gsLink) return;
    g_gsControl->Off();
    g_gsLink->Stop(500);
}

bool GsTyping() { return g_gsControl && g_gsControl->Typing(); }
bool GsKey(uint32_t code, int value, bool elsewhere, gsforward::Mods now) {
    return g_gsControl && g_gsControl->Key(code, value, elsewhere, now);
}
void GsBlur() {
    if (g_gsControl) g_gsControl->Blur();
}

}  // namespace ftvr
