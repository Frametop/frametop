// Steam's own windows as panels of ours (steamwin.h has why and how; a spike, STEAM_DASH_PANEL, off
// by default). This file is its X link (Steam's own display, gsforward::Role::Steam, on a thread of
// its own: gslink.h), the tiles on OpenVR, and the laser on them as X's pointer. Steam's popups,
// the followers, are vrsteamfollow.cpp's.
//
// The tiles are screen 190 (Steam's dashboard window) and 191 (its menu), panels gamescope draws
// (gsapp) at their windows' sizes, anchored to the dashboard the first time (shown with it, not
// staying; the menu left of the dashboard window), then as they were (tile memory, keys
// "steam:dashboard" and "steam:menu"). They're fixed-size tiles: their windows aren't resized. With
// the dashboard window tiled, the dashboard tiles keep no slot for Steam's menu (vrtiles.cpp's
// Regroup).
//
// What the spike is there to find out (docs/design.md, HEADSET-TESTS.md): whether Steam tags its
// windows back itself (each time is logged, and after steamwin::kMaxRetakes in a row it's given
// back) and whether the X pointer reaches them. Keys aren't sent to them yet. Steam's own panels are
// left alone: with a window ours they have no texture (gamescope forwards to one overlay), so they
// draw nothing, and Steam paints only while its panel counts as visible (alpha 0 = a black tile).

#include "vrpanel.h"
#include "gsdisplay.h"
#include "gslink.h"
#include "steamwin.h"
#include "vrsteamfollow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace ftvr {
namespace {
constexpr int kIndex = steamfollow::kTileIndex;   // screen 190 + slot (remote displays 101..116, gamescope apps 200..)
constexpr double kMpp = 1.0 / 1250;               // their density at first: 1250 pixels a metre
constexpr int64_t kFindMs = 1000;                 // Steam's overlays looked up again this often (made again: new ones)
const char *const kLabels[steamwin::kTiles] = {"Steam", "Steam menu"};
const char *const kPrefixes[steamwin::kTiles] = {"frametop.steamdash", "frametop.steammenu"};

std::unique_ptr<gsforward::AsyncLink> g_link;
steamwin::Fleet g_fleet;
bool g_on = false;
uint32_t g_pid = 0;                 // gamescope's (the panels' renderer)
int64_t g_findAt = 0;
uint32_t g_buttons = 0;             // X buttons held on a tile (bit b: button b)
uint32_t g_raised = 0;              // the window of Steam's raised last
const char g_frames = 0;            // (Screen::shown: gamescope's frames, no buffer of ours)

uint64_t FindSteams(const std::string &key) {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    return !key.empty() && vr::VROverlay()->FindOverlay(key.c_str(), &h) == vr::VROverlayError_None ? h : 0;
}
int IndexOf(const steamwin::Spec &spec) { return kIndex + spec.slot; }

uint64_t MakeTile(const steamwin::Spec &spec, const gsforward::Window &w) {
    const int index = IndexOf(spec);
    if (g_screens.count(index) || !g_pid) return 0;
    Screen &s = g_screens[index];
    s.gsapp = true, s.steamDash = true, s.gsWin = w.id, s.mpp = kMpp;
    s.metres = std::clamp(w.w * kMpp, kMinWidth, 12.0);
    const char *prefix = kPrefixes[spec.slot];
    if (!MakePanel(s, prefix, kLabels[spec.slot])) return g_screens.erase(index), 0;
    AnnounceOverlay(prefix);
    vr::VROverlay()->SetOverlayRenderingPid(s.overlay, g_pid);   // (only its renderer may set its texture)
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]);
    SetAbsolute(s, PanelPose(head.m[0][3] - std::sin(yaw) * 1.5, head.m[1][3], head.m[2][3] - std::cos(yaw) * 1.5,
                             yaw * 180 / M_PI, 0, 0));
    return s.overlay;
}

void ShowTile(const steamwin::Spec &spec, const gsforward::Window &w) {
    const int index = IndexOf(spec);
    const auto it = g_screens.find(index);
    if (it == g_screens.end()) return;
    Screen &s = it->second;
    s.label = kLabels[spec.slot];
    s.gsWin = w.id;
    vr::VROverlay()->SetOverlayName(s.overlay, kLabels[spec.slot]);
    GsGeometry(s, index, w);
    s.shown = &g_frames;
    const bool known = !g_tileMemory.Get(spec.tileKey).empty();
    TileKnown(index, spec.tileKey);
    if (!known && !s.gsSeen) TileAnchor(index, false, kLabels[spec.slot], spec.slot == 1 ? kIndex : -1);
    std::printf("screen %d: %s's window (0x%x, %dx%d)%s\n", index + 1, kLabels[spec.slot], w.id, w.w, w.h,
                s.gsSeen ? ", mapped again" : "");
    s.gsSeen = true;
    UpdateVisibility();
}

void LetGo() {
    for (uint8_t b = 1; b < 32; ++b)
        if (g_buttons & 1u << b) g_link->Button(b, false);
    g_buttons = 0;
    steamfollow::LetGo(*g_link);
}

const char *What(const steamwin::Spec &spec) { return spec.role == steamwin::Spec::Tile ? kLabels[spec.slot] : "Steam popup"; }

void Do(const std::vector<steamwin::Action> &actions) {
    for (const steamwin::Action &a : actions) {
        const bool tile = a.spec.role == steamwin::Spec::Tile;
        const int index = tile ? IndexOf(a.spec) : -1;
        switch (a.type) {
            case steamwin::Action::Make: {
                const uint64_t o = tile ? MakeTile(a.spec, a.win) : steamfollow::Make(a.win, g_pid);
                if (!o) std::printf("%s (0x%x): nothing of ours for it (it stays Steam's)\n", What(a.spec), a.win.id);
                Do(g_fleet.Made(a.win.id, o));
                break;
            }
            case steamwin::Action::Tag: {
                const steamwin::Tracker *t = g_fleet.Find(a.win.id);
                if (t && a.overlay == t->Steams()) {
                    // Steam's back: its overlay as it is now (made again since: the new one)
                    const uint64_t steams = FindSteams(a.spec.overlay);
                    if (!steams) {
                        std::printf("%s (0x%x): Steam's overlay is gone, left as it is\n", What(a.spec), a.win.id);
                        break;
                    }
                    g_link->Tag(a.win.id, steams);
                    std::printf("%s (0x%x): Steam's own again\n", What(a.spec), a.win.id);
                } else {
                    g_link->Tag(a.win.id, a.overlay);
                    std::printf("%s (0x%x, %s): ours (Steam has tagged it back %d times)\n", What(a.spec), a.win.id,
                                a.spec.overlay.c_str(), t ? t->Retaken() : 0);
                }
                break;
            }
            case steamwin::Action::Show:
                if (tile) ShowTile(a.spec, a.win);
                break;   // (a follower shows when Steam shows its own: steamfollow::Step)
            case steamwin::Action::Move:
                if (!tile) steamfollow::Size(a.overlay, a.win);
                else if (auto it = g_screens.find(index); it != g_screens.end()) GsGeometry(it->second, index, a.win);
                break;
            case steamwin::Action::Hide:
                LetGo();
                if (auto it = g_screens.find(index); tile && it != g_screens.end()) {
                    EndDrag(it->second);
                    it->second.shown = nullptr;
                    UpdateVisibility();
                }
                break;
            case steamwin::Action::Drop:
                LetGo();
                if (!tile) {
                    steamfollow::Drop(a.overlay);
                    break;
                }
                if (g_mouseScreen == index) g_mouseScreen = -1;
                if (g_screens.count(index)) {
                    std::printf("screen %d: %s's panel gone\n", index + 1, kLabels[a.spec.slot]);
                    ft_vr_screen_destroy(index);
                }
                break;
            case steamwin::Action::GaveUp:
                std::printf("%s (0x%x): Steam tagged it back %d times in %lld s: given back to Steam "
                            "(STEAM_DASH_PANEL off and on to try again)\n",
                            What(a.spec), a.win.id, steamwin::kMaxRetakes, (long long)(steamwin::kRetakeWindowMs / 1000));
                break;
        }
    }
}

// Steam's overlays for its windows: looked up for the new ones, and for all of them now and then
// (Steam restarted: new ones).
void Find(bool all) {
    std::vector<std::pair<uint32_t, uint64_t>> found;
    for (const auto &[id, t] : g_fleet.Windows())
        if (all || !t.Steams())
            if (const uint64_t h = FindSteams(t.What().overlay); h != t.Steams()) found.push_back({id, h});
    for (const auto &[id, h] : found) Do(g_fleet.SetSteams(id, h));
}

// A tile closed from Frametop's side (its panel gone, the window still ours): the window back to Steam.
void Lost() {
    std::vector<uint32_t> lost;
    for (const auto &[id, t] : g_fleet.Windows())
        if (t.What().role == steamwin::Spec::Tile && t.Panel() && !g_screens.count(IndexOf(t.What()))) lost.push_back(id);
    for (uint32_t id : lost) {
        std::printf("Steam's window 0x%x: its panel closed: Steam's again\n", id);
        Do(g_fleet.Lost(id));
    }
}
}  // namespace

int SteamDashIndex() { return g_on ? kIndex : -1; }

// Each tick: Steam's display's news (its thread opens it, again every AsyncLink::kRetryMs).
void SteamDashStep() {
    if (!g_link) return;
    const int64_t now = NowMs();
    bool fresh = false;
    for (gsforward::Update &u : g_link->Take()) switch (u.type) {
            case gsforward::Update::Opened:
                g_pid = u.pid;
                g_findAt = 0;
                std::printf("Steam's windows: watching %s (gamescope %u)\n", u.display.c_str(), u.pid);
                break;
            case gsforward::Update::Closed:
                Do(g_fleet.Closed());
                if (g_on && u.why != "off") std::printf("Steam's windows: %s\n", u.why.c_str());
                break;
            case gsforward::Update::Window: {
                const steamwin::Tracker *t = g_fleet.Find(u.event.win.id);
                if (u.event.type == gsforward::Event::Retarget && t && t->Panel() && u.event.win.target != t->Panel())
                    std::printf("Steam's window 0x%x: tagged again by someone else (to 0x%llx)\n", u.event.win.id,
                                (unsigned long long)u.event.win.target);
                Do(g_fleet.On(u.event, now));
                fresh = fresh || u.event.type == gsforward::Event::Mapped;
                break;
            }
        }
    if (!g_link->IsOpen()) return;
    const bool all = now >= g_findAt;
    if (all) g_findAt = now + kFindMs;
    if (all || fresh) Find(all);
    Lost();
    steamfollow::Step(g_fleet, *g_link, &g_raised);
}

// The laser on a tile: X's pointer, buttons and wheel there (its window raised first: Steam's
// windows all sit at the top left of its screen, and the pointer goes to the top one).
void SteamDashEvents(Screen &s, int index) {
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) {
        if (!g_link || !s.gsWin) continue;
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
            if (g_raised != s.gsWin) g_link->Raise(s.gsWin), g_raised = s.gsWin;
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
                if (down) std::printf("screen %d: a click at %d,%d on Steam's window 0x%x\n", index + 1, x, y, s.gsWin);
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
                if (g_raised == s.gsWin) g_raised = 0;
                if (g_buttons && s.drag == Drag::None) LetGo();
                break;
            default:
                break;
        }
    }
}

// STEAM_DASH_PANEL: on, Steam's windows become ours when they map (or now, if they are); off,
// they're Steam's again.
void SteamDashSwitch(bool on) {
    if (on == g_on) return;
    g_on = on;
    if (!g_link) {
        if (!on) return;
        g_link = std::make_unique<gsforward::AsyncLink>(gsforward::SteamDisplays, true, gsforward::Role::Steam);
        g_link->SetOn(true);   // (kept open when it's off again: Steam's windows go back to it, and stay)
    }
    std::printf("Steam's windows: %s (STEAM_DASH_PANEL)\n", on ? "Frametop's when they map" : "Steam's own");
    Do(g_fleet.Switch(on));
}

// ft-screens going: the windows back to Steam first (half a second at most).
void SteamDashShutdown() {
    if (!g_link) return;
    LetGo();
    Do(g_fleet.Shutdown());
    g_link->Stop(500);
}

}  // namespace ftvr
