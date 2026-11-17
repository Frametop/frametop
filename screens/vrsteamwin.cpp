// Steam's dashboard window as a panel of ours (steamwin.h has why and how; a spike, STEAM_DASH_PANEL,
// off by default). This file is its X link (Steam's own display, gsforward::Role::Steam, on a
// thread of its own: gslink.h), the panel on OpenVR, and the laser on it as X's pointer.
//
// The panel is screen 190, a panel gamescope draws (gsapp) at Steam's window's size, anchored to
// the dashboard the first time (shown with it, not staying), then as it was (tile memory, key
// "steam:dashboard"). It's a fixed-size tile: its window isn't resized. With it tiled, the
// dashboard tiles keep no slot for Steam's menu (vrtiles.cpp's Regroup).
//
// What the spike is there to find out (docs/design.md, HEADSET-TESTS.md): whether Steam tags its
// window back itself (each time is logged, and after steamwin::kMaxRetakes in a row it's given
// back) and whether the X pointer reaches the window. Keys aren't sent to it yet. Steam's own panel
// is left alone: with the window ours it has no texture (gamescope forwards to one overlay), so it
// draws nothing, and Steam paints only while that panel counts as visible (alpha 0 = a black tile).

#include "vrpanel.h"
#include "gsdisplay.h"
#include "gslink.h"
#include "steamwin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace ftvr {
namespace {
constexpr int kIndex = 189;                 // screen 190 (remote displays 101..116, gamescope apps 200..)
constexpr double kMpp = 1.0 / 1250;         // its density at first: 1250 pixels a metre
constexpr int64_t kFindMs = 1000;           // Steam's overlay looked up again this often (made again: a new one)
const char *const kKey = "steam:dashboard";  // its tile memory's key

std::unique_ptr<gsforward::AsyncLink> g_link;
steamwin::Tracker g_win;
bool g_on = false;
uint32_t g_pid = 0;                 // gamescope's (the panel's renderer)
uint64_t g_steams = 0;              // Steam's overlay for it (steamwin::kSteamOverlay)
int64_t g_findAt = 0;
uint32_t g_buttons = 0;             // X buttons held on it (bit b: button b)
bool g_raised = false;              // its window raised since the laser came onto it
const char g_frames = 0;            // (Screen::shown: gamescope's frames, no buffer of ours)

uint64_t FindSteams() {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    return vr::VROverlay()->FindOverlay(steamwin::kSteamOverlay, &h) == vr::VROverlayError_None ? h : 0;
}

uint64_t Make(const gsforward::Window &w) {
    if (g_screens.count(kIndex) || !g_pid) return 0;
    Screen &s = g_screens[kIndex];
    s.gsapp = true, s.steamDash = true, s.gsWin = w.id, s.mpp = kMpp;
    s.metres = std::clamp(w.w * kMpp, kMinWidth, 12.0);
    const char *prefix = "frametop.steamdash";
    if (!MakePanel(s, prefix, "Steam")) return g_screens.erase(kIndex), 0;
    AnnounceOverlay(prefix);
    vr::VROverlay()->SetOverlayRenderingPid(s.overlay, g_pid);   // (only its renderer may set its texture)
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]);
    SetAbsolute(s, PanelPose(head.m[0][3] - std::sin(yaw) * 1.5, head.m[1][3], head.m[2][3] - std::cos(yaw) * 1.5,
                             yaw * 180 / M_PI, 0, 0));
    return s.overlay;
}

void Show(const gsforward::Window &w) {
    const auto it = g_screens.find(kIndex);
    if (it == g_screens.end()) return;
    Screen &s = it->second;
    s.label = "Steam";
    vr::VROverlay()->SetOverlayName(s.overlay, "Steam");
    GsGeometry(s, kIndex, w);
    s.shown = &g_frames;
    const bool known = !g_tileMemory.Get(kKey).empty();
    TileKnown(kIndex, kKey);
    if (!known && !s.gsSeen) TileAnchor(kIndex, false, "Steam's window");
    std::printf("screen %d: Steam's dashboard window (0x%x, %dx%d)%s\n", kIndex + 1, w.id, w.w, w.h,
                s.gsSeen ? ", mapped again" : "");
    s.gsSeen = true;
    UpdateVisibility();
}

void LetGo() {
    for (uint8_t b = 1; b < 32; ++b)
        if (g_buttons & 1u << b) g_link->Button(b, false);
    g_buttons = 0;
}

void Do(const std::vector<steamwin::Action> &actions) {
    for (const steamwin::Action &a : actions) switch (a.type) {
            case steamwin::Action::Make: {
                const uint64_t o = Make(a.win);
                if (!o) std::printf("Steam's window: no panel for it (it stays Steam's)\n");
                Do(g_win.Made(o));
                break;
            }
            case steamwin::Action::Tag:
                if (a.overlay == g_steams) {
                    // Steam's back: its overlay as it is now (made again since: the new one)
                    if (!(g_steams = FindSteams())) {
                        std::printf("Steam's window (0x%x): Steam's overlay is gone, left as it is\n", a.win.id);
                        break;
                    }
                    g_link->Tag(a.win.id, g_steams);
                    std::printf("Steam's window (0x%x): Steam's own again\n", a.win.id);
                } else {
                    g_link->Tag(a.win.id, a.overlay);
                    std::printf("Steam's window (0x%x): on screen %d (Steam has tagged it back %d times)\n", a.win.id,
                                kIndex + 1, g_win.Retaken());
                }
                break;
            case steamwin::Action::Show: Show(a.win); break;
            case steamwin::Action::Move:
                if (auto it = g_screens.find(kIndex); it != g_screens.end()) GsGeometry(it->second, kIndex, a.win);
                break;
            case steamwin::Action::Hide:
                if (auto it = g_screens.find(kIndex); it != g_screens.end()) {
                    LetGo();
                    EndDrag(it->second);
                    it->second.shown = nullptr;
                    UpdateVisibility();
                }
                break;
            case steamwin::Action::Drop:
                LetGo();
                if (g_mouseScreen == kIndex) g_mouseScreen = -1;
                if (g_screens.count(kIndex)) {
                    std::printf("screen %d: Steam's window's panel gone\n", kIndex + 1);
                    ft_vr_screen_destroy(kIndex);
                }
                break;
            case steamwin::Action::GaveUp:
                std::printf("Steam's window: Steam tagged it back %d times in %lld s: given back to Steam "
                            "(STEAM_DASH_PANEL off and on to try again)\n",
                            steamwin::kMaxRetakes, (long long)(steamwin::kRetakeWindowMs / 1000));
                break;
        }
}
}  // namespace

int SteamDashIndex() { return g_on ? kIndex : -1; }

// Each tick: Steam's display's news (its thread opens it, again every AsyncLink::kRetryMs).
void SteamDashStep() {
    if (!g_link) return;
    const int64_t now = NowMs();
    for (gsforward::Update &u : g_link->Take()) switch (u.type) {
            case gsforward::Update::Opened:
                g_pid = u.pid;
                g_steams = FindSteams();
                g_findAt = now + kFindMs;
                std::printf("Steam's window: watching %s (gamescope %u; Steam's overlay %s 0x%llx)\n", u.display.c_str(), u.pid,
                            steamwin::kSteamOverlay, (unsigned long long)g_steams);
                Do(g_win.SetSteams(g_steams));
                break;
            case gsforward::Update::Closed:
                Do(g_win.Closed());
                if (g_on && u.why != "off") std::printf("Steam's window: %s\n", u.why.c_str());
                break;
            case gsforward::Update::Window:
                if (u.event.type == gsforward::Event::Retarget && u.event.win.id == g_win.Window() && g_win.Panel() &&
                    u.event.win.target != g_win.Panel())
                    std::printf("Steam's window: tagged again by someone else (to 0x%llx)\n",
                                (unsigned long long)u.event.win.target);
                Do(g_win.On(u.event, now));
                break;
        }
    // Steam's overlay made again (Steam restarted), or not there yet: looked up again
    if (g_link->IsOpen() && now >= g_findAt) {
        g_findAt = now + kFindMs;
        const uint64_t h = FindSteams();
        if (h != g_steams) g_steams = h, Do(g_win.SetSteams(h));
    }
}

// The laser on its panel: X's pointer, buttons and wheel there (its window raised first: Steam's
// windows all sit at the top left of its screen, and the pointer goes to the top one).
void SteamDashEvents(Screen &s, int index) {
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) {
        if (!g_link || !g_win.Window()) continue;
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
        const int x = s.gsX + int(std::lround(ev.data.mouse.x)), y = s.gsY + s.height - int(std::lround(ev.data.mouse.y));
        auto point = [&] {
            if (!g_raised) g_link->Raise(g_win.Window()), g_raised = true;
            g_link->Pointer(x, y);
        };
        switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                if (s.toggleCarry || s.drag != Drag::None) break;
                point();
                break;
            case vr::VREvent_MouseButtonDown:
            case vr::VREvent_MouseButtonUp: {
                if (CarryClick(ev)) break;
                const bool down = ev.eventType == vr::VREvent_MouseButtonDown;
                const uint32_t button = LinuxButton(ev.data.mouse.button);
                if (!down) {
                    EndDragsBy(ev.trackedDeviceIndex);
                } else {
                    s.useMs = now;
                    if (ModCarry(s, index, ev.trackedDeviceIndex, button, ev.data.mouse.x, s.height - ev.data.mouse.y)) break;
                }
                const uint8_t xb = gsforward::XButton(int(button));
                point();
                g_link->Button(xb, down);
                if (xb < 32) g_buttons = down ? g_buttons | 1u << xb : g_buttons & ~(1u << xb);
                break;
            }
            case vr::VREvent_ScrollDiscrete:
                if (CarryScroll(ev)) break;
                s.useMs = now;
                g_link->Scroll(int(std::lround(ev.data.scroll.ydelta)));
                break;
            case vr::VREvent_FocusLeave:
                g_raised = false;
                if (g_buttons && s.drag == Drag::None) LetGo();
                break;
            default:
                break;
        }
    }
}

// STEAM_DASH_PANEL: on, Steam's window becomes a panel of ours when it maps (or now, if it is);
// off, it's Steam's again.
void SteamDashSwitch(bool on) {
    if (on == g_on) return;
    g_on = on;
    if (!g_link) {
        if (!on) return;
        g_link = std::make_unique<gsforward::AsyncLink>(gsforward::SteamDisplays, true, gsforward::Role::Steam);
        g_link->SetOn(true);   // (kept open when it's off again: Steam's window goes back to it, and stays)
    }
    std::printf("Steam's window: %s (STEAM_DASH_PANEL)\n", on ? "a Frametop panel when it maps" : "Steam's own");
    Do(g_win.Switch(on));
}

// ft-screens going: the window back to Steam first (half a second at most).
void SteamDashShutdown() {
    if (!g_link) return;
    LetGo();
    Do(g_win.Shutdown());
    g_link->Stop(500);
}

}  // namespace ftvr
