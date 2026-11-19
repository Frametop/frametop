// Steam's own windows as panels of ours (steamwin.h has why and how; STEAM_DASH_PANEL, Display
// Settings' "Steam's window" switch, off by default). This file is its X link (Steam's own display, gsforward::Role::Steam, on a thread of
// its own: gslink.h), the tiles on OpenVR, and the laser on them as their pages' mouse (steamcdp.h:
// X's pointer doesn't reach Steam's windows). Steam's popups, the followers, are vrsteamfollow.cpp's.
//
// The tiles are screen 190 (Steam's dashboard window) and 191 (its menu), panels gamescope draws
// (gsapp) at their windows' sizes. The dashboard window's is anchored to the dashboard the first
// time (shown with it, not staying), then as it was (tile memory, key "steam:dashboard"); it's a
// fixed-size tile: its window isn't resized. The menu's is glued to it (steamglue.h: flush left of
// it, only the part Steam draws, moved, shown and tiled with it; GlueStep). With the dashboard
// window tiled, the dashboard tiles keep no slot for Steam's menu (vrtiles.cpp's Regroup).
//
// Steam may tag its windows back itself: each time is logged, and after steamwin::kMaxRetakes in a
// row it's given back (docs/design.md). Keys aren't sent to them: a click in a text field opens Steam's keyboard (with Frametop,
// Full Keyboard, mounted below the dashboard tile; FrametopVr::steam_tile). Steam's own panels are
// left shown: with a window ours they have no texture (gamescope forwards to one overlay), so they
// draw nothing, and Steam paints only while its panel counts as visible (alpha 0 = a black tile).
// Shown, they'd catch the laser in front of the tiles: vrsteamhit.cpp shrinks them to nothing.

#include "vrpanel.h"
#include "gsdisplay.h"
#include "gslink.h"
#include "steamcdp.h"
#include "steamglue.h"
#include "steamwin.h"
#include "vrsteamfollow.h"
#include "vrsteamhit.h"

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
bool g_on = false;                  // STEAM_DASH_PANEL
// SteamVR shows Steam's page in its theater (its own View in Theater; ft-dashpatch tells us): Steam's
// windows are Steam's meanwhile, so its theater draws them, with its own way out (user, 2026-11-18:
// "Hand off to SteamVR"; kept, the page was shrunk to nothing there, an invisible theater with no way
// back). Taken again once SteamVR docks it back.
bool g_theater = false;
uint32_t g_pid = 0;                 // gamescope's (the panels' renderer)
int64_t g_findAt = 0;
bool g_dashWasUp = false;          // SteamVR's dashboard open, last tick
std::unique_ptr<steamcdp::Pages> g_pages;   // the laser on them, to their pages

// The menu glued to the dashboard window's panel (steamglue.h, GlueStep).
constexpr int kMenu = kIndex + 1;
constexpr int64_t kAskOnMs = 20;      // what it draws asked this often while the laser's on it (its labels open and close)
constexpr int64_t kAskOffMs = 1000;   // ...and with nobody on it
constexpr int64_t kSettleMs = 1100;   // its labels closed this long after the laser left (Steam shuts them after ~800 ms)
constexpr uint32_t kGlueSortOrder = 5;   // over the tiles (its open labels go over the next one), under the controls
struct Glue {
    steamglue::Rect crop;          // what it draws now (w 0: not known yet, all of it shown)
    steamglue::Rect rest;          // ...with nobody on it (w 0: not known yet); its width is what the tiles count (GluedPx)
    steamglue::Slide slide;        // the crop shown, sliding to `crop` while the laser's on it, else to `rest`
    bool on = false;               // the laser is on it
    int64_t leftMs = 0;            // ...left it then
    steamglue::Rect leftCrop;      // ...what it drew then (steamglue::NewRest)
    int64_t askAt = 0, seenMs = 0;   // the next ask; the last answer taken (when it was asked)
    steamglue::Rect shownCrop;     // the crop set on its overlay
    int shownW = 0, shownH = 0;    // ...for a window this size
} g_glue;
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
    if (index == kMenu) {
        s.glueTo = kIndex;
        g_glue = {};
        vr::VROverlay()->SetOverlaySortOrder(s.overlay, kGlueSortOrder);
    }
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
    if (s.glueTo < 0) {   // (a glued one goes with what it's glued to)
        const bool known = !g_tileMemory.Get(spec.tileKey).empty();
        TileKnown(index, spec.tileKey);
        if (!known && !s.gsSeen) TileAnchor(index, dashtiles::Stay::Default, kLabels[spec.slot]);
    }
    std::printf("screen %d: %s's window (0x%x, %dx%d)%s\n", index + 1, kLabels[spec.slot], w.id, w.w, w.h,
                s.gsSeen ? ", mapped again" : "");
    s.gsSeen = true;
    UpdateVisibility();
}

void LetGo() {
    if (g_pages) g_pages->LetGo();
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

// Steam's menu: of its menu windows, the one whose overlay Steam shows (it paints only that one; the
// others are black). The one held now first, if Steam shows that one too; none shown (the dashboard
// closed), the one held stays.
void PickMenu() {
    uint32_t pick = 0;
    for (const auto &[id, t] : g_fleet.Windows()) {
        if (t.What().role != steamwin::Spec::Tile || t.What().slot != 1 || !t.Steams()) continue;
        if (vr::VROverlay()->IsOverlayVisible(t.Steams()) && (!pick || t.Held())) pick = id;
    }
    const steamwin::Tracker *held = g_fleet.TileIn(1);
    if (!pick || (held && held->Window() == pick)) return;
    std::printf("Steam menu: Steam shows 0x%x's (was 0x%x's): that one is the tile\n", pick, held ? held->Window() : 0);
    Do(g_fleet.Showing(1, pick));
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
bool Differ(const Mat &a, const Mat &b) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(a.m[r][c] - b.m[r][c]) > 1e-5f) return true;
    return false;
}

// The menu's panel glued to the dashboard window's (steamglue.h): what its page draws asked for
// (often while the laser's on it), its overlay cropped to that (sliding there; shut as the laser
// leaves) at the dashboard window's density, and put flush left of it: pinned as it's pinned (riding with it), or where it is in the room.
void GlueStep() {
    const auto mit = g_screens.find(kMenu), pit = g_screens.find(kIndex);
    if (mit == g_screens.end() || pit == g_screens.end()) return;
    Screen &m = mit->second;
    const Screen &p = pit->second;
    if (m.width <= 0 || m.height <= 0 || p.width <= 0 || m.overlay == vr::k_ulOverlayHandleInvalid) return;
    const int64_t now = NowMs();
    const steamwin::Tracker *t = m.gsWin ? g_fleet.Find(m.gsWin) : nullptr;
    const bool settling = g_glue.on || now - g_glue.leftMs < kSettleMs ||
                          steamglue::Shutting(g_glue.crop, g_glue.rest, now - g_glue.leftMs);   // (asked often till Steam shuts)
    if (g_pages && t) {
        const std::string &page = t->Win().title;
        if (now >= g_glue.askAt) g_pages->AskDrawn(page), g_glue.askAt = now + (settling ? kAskOnMs : kAskOffMs);
        steamcdp::Pages::Drawn d;
        steamglue::Rect r;
        if (g_pages->LastDrawn(page, &d) && d.askedMs > g_glue.seenMs) {
            g_glue.seenMs = d.askedMs;
            if (steamglue::FromCss(d.l, d.t, d.r, d.b, d.dpr, m.width, m.height, &r)) {
                g_glue.crop = r;
                const steamglue::Rect &was = g_glue.rest;
                if (!g_glue.on && d.askedMs >= g_glue.leftMs + kSettleMs && steamglue::NewRest(r, was, g_glue.leftCrop)) {
                    std::printf("screen %d: Steam's menu draws %dx%d at %d,%d of its window (glued to screen %d)\n", kMenu + 1,
                                r.w, r.h, r.x, r.y, kIndex + 1);
                    const bool resized = r.w != was.w;   // (the tiles count its width)
                    g_glue.rest = r;
                    if (resized && TileAnchored(kIndex)) RetileDash();
                }
            }
        }
    }
    // (open when Steam has; shut when Steam has too, its icons drawn first)
    const steamglue::Rect goal = steamglue::Goal(g_glue.on, g_glue.crop, g_glue.rest, now - g_glue.leftMs);
    if (goal.w > 0) g_glue.slide.To(goal, now);
    const steamglue::Rect c = g_glue.slide.Set() ? g_glue.slide.At(now) : steamglue::Rect{0, 0, m.width, m.height};
    if (c.x != g_glue.shownCrop.x || c.y != g_glue.shownCrop.y || c.w != g_glue.shownCrop.w || c.h != g_glue.shownCrop.h ||
        m.width != g_glue.shownW || m.height != g_glue.shownH) {
        // (fractions of the texture, v from the top; the mouse stays in the whole window's pixels)
        const vr::VRTextureBounds_t b = {float(c.x) / m.width, float(c.y) / m.height, float(c.x + c.w) / m.width,
                                         float(c.y + c.h) / m.height};
        vr::VROverlay()->SetOverlayTextureBounds(m.overlay, &b);
        g_glue.shownCrop = c, g_glue.shownW = m.width, g_glue.shownH = m.height;
        m.cropX = c.x, m.cropY = c.y, m.cropW = c.w, m.cropH = c.h;
    }
    m.mpp = p.metres / p.width;   // (the dashboard window's density: the same size of text)
    const double width = c.w * m.mpp;
    if (std::fabs(m.metres - width) > 1e-4 || m.curve != p.curve) {
        m.metres = width, m.curve = p.curve;
        vr::VROverlay()->SetOverlayWidthInMeters(m.overlay, float(width));
        ApplyCurve(m);
    }
    double off[3][4];   // (its middle level with the window's middle: Steam centres what it draws in it)
    steamglue::Beside(p.metres, p.curve, width, (m.height / 2.0 - (c.y + c.h / 2.0)) * m.mpp, off);
    Mat rel;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) rel.m[r][k] = float(off[r][k]);
    if (p.pinned != kNone) {
        const Mat want = Mul(p.pinRel, rel);
        if (m.pinned != p.pinned || m.pinMode != p.pinMode || Differ(m.pinRel, want)) PinIn(m, p.pinned, want, p.pinMode);
    } else if (Mat at; ScreenPose(p, &at)) {
        const Mat want = Mul(at, rel);
        if (m.pinned != kNone || Differ(m.pose, want)) SetAbsolute(m, want, p.universe);
    }
}
}  // namespace

int SteamDashIndex() { return g_fleet.On() ? kIndex : -1; }
int GluedPx(int index) { return index == kIndex && g_screens.count(kMenu) ? g_glue.rest.w : 0; }

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
            case gsforward::Update::Note: std::printf("Steam's windows: %s\n", u.why.c_str()); break;
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
    if (!g_link->IsOpen()) return steamhit::Clear();
    const bool all = now >= g_findAt;
    if (all) g_findAt = now + kFindMs;
    // SteamVR's controls under Steam's page (View in Theater, Float in World...) hidden while its
    // window is our tile: the page is shrunk to nothing, so they stood at its middle, over the tiles.
    // A lease ft-dashpatch lets lapse after a few seconds (ft-screens gone: they come back).
    if (all && g_fleet.On() && g_screens.count(kIndex) && g_screens[kIndex].shown) SendDashpatch("steamstrip 1");
    // The dashboard opening over a game lent to SteamVR's theater opens on the game's tab, and Steam
    // paints its window only while its own tab shows: the tile was black in front (user, 2026-11-18:
    // "an invisible game scope window that is taking over the forward spot"). Steam's tab then,
    // once each time it opens (steampage.py: only from a game's tab).
    const bool dashUp = vr::VROverlay()->IsDashboardVisible();
    if (dashUp && !g_dashWasUp && g_fleet.On() && GsInTheater()) SendDashpatch("steamtab");
    g_dashWasUp = dashUp;
    if (all || fresh) Find(all);
    if (all || fresh) PickMenu();
    Lost();
    steamhit::Step(g_fleet, all);
    if (g_pages) steamfollow::Step(g_fleet, *g_pages);
    GlueStep();
}

// The laser on a tile: its page's mouse (steamcdp.h), at the window's pixels it points at.
void SteamDashEvents(Screen &s, int index) {
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) {
        const steamwin::Tracker *t = s.gsWin ? g_fleet.Find(s.gsWin) : nullptr;
        if (!g_pages || !t) continue;
        const std::string &page = t->Win().title;
        const int64_t now = NowMs();
        if (ev.eventType != vr::VREvent_FocusLeave) {
            s.inputMs = now;
            if (FromLaser(ev)) s.laserMs = now, s.laserDevice = ev.trackedDeviceIndex;
        }
        if (s.glueTo >= 0) {   // the laser on the menu or off it: its labels open or close (GlueStep asks often)
            const bool on = ev.eventType == vr::VREvent_MouseMove, off = ev.eventType == vr::VREvent_FocusLeave;
            if ((on && !g_glue.on) || (off && g_glue.on)) {
                g_glue.on = on;
                if (off) g_glue.leftMs = now, g_glue.leftCrop = g_glue.crop;
                g_glue.askAt = 0;
            }
        }
        const int ours = OurPointer(ev.trackedDeviceIndex);
        if (ev.eventType == vr::VREvent_FocusLeave) {
            if (ours >= 0 && g_mouseScreen == index) g_mouseScreen = -1;
        } else if (ours > 0) {
            g_mouseScreen = index, g_mouseDevice = ev.trackedDeviceIndex;
        }
        const int x = int(std::lround(ev.data.mouse.x)), y = s.height - int(std::lround(ev.data.mouse.y));
        switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                if (s.toggleCarry || s.drag != Drag::None) break;
                g_pages->Move(page, x, y);
                break;
            case vr::VREvent_MouseButtonDown:
            case vr::VREvent_MouseButtonUp: {
                if (CarryClick(ev) || BarClaims(ev)) break;   // (a bar's press, let go over Steam's page: the bar's)
                const bool down = ev.eventType == vr::VREvent_MouseButtonDown;
                const uint32_t button = LinuxButton(ev.data.mouse.button);
                if (!down) {
                    EndDragsBy(ev.trackedDeviceIndex);
                } else {
                    s.useMs = now;
                    if (s.glueTo < 0 &&   // (a glued one isn't carried on its own)
                        ModCarry(s, index, ev.trackedDeviceIndex, button, ev.data.mouse.x, s.height - ev.data.mouse.y))
                        break;
                }
                steamcdp::Button b;
                if (!steamcdp::FromLinux(button, &b)) break;
                if (down) std::printf("screen %d: a click at %d,%d on Steam's window 0x%x\n", index + 1, x, y, s.gsWin);
                g_pages->Press(page, x, y, b, down);
                break;
            }
            case vr::VREvent_ScrollDiscrete:
                if (CarryScroll(ev)) break;
                s.useMs = now;
                g_pages->Wheel(page, int(std::lround(ev.data.scroll.ydelta)));
                break;
            case vr::VREvent_FocusLeave:
                if (s.drag == Drag::None) g_pages->Leave(page);
                break;
            default:
                break;
        }
    }
}

namespace {
// Steam's windows ours (STEAM_DASH_PANEL on and Steam's page not in SteamVR's theater) or Steam's.
void Watch() {
    if (g_link) return;
    g_link = std::make_unique<gsforward::AsyncLink>(gsforward::SteamDisplays, true, gsforward::Role::Steam);
    g_link->SetOn(true);   // (kept open when it's off again: Steam's windows go back to it, and stay)
    g_pages = std::make_unique<steamcdp::Pages>();
}

void Apply(const char *why) {
    const bool on = g_on && !g_theater;
    if (on == g_fleet.On()) return;
    if (!g_link) {
        if (!on) return;
        Watch();
    }
    std::printf("Steam's windows: %s (%s)\n", on ? "Frametop's when they map" : "Steam's own", why);
    Do(g_fleet.Switch(on));
    if (!on) SendDashpatch("steamstrip 0");   // (SteamVR's controls under its page back at once)
}
}  // namespace

// STEAM_DASH_PANEL: on, Steam's windows become ours when they map (or now, if they are); off,
// they're Steam's again.
void SteamDashSwitch(bool on) {
    static bool swept = false;   // (at start, switched on or not)
    if (!swept) {
        swept = true, steamhit::Sweep();
        // Steam's display watched even with the switch off: a tag of ours on its windows from an
        // ft-screens that was killed goes back to Steam (steamwin.h: Tracker::SetSteams), or Steam's
        // page stays blank with SteamVR's controls for it standing over the tiles.
        Watch();
    }
    g_on = on;
    Apply("STEAM_DASH_PANEL");
}

void SteamDashTheater(bool on) {
    if (on == g_theater) return;
    g_theater = on;
    Apply(on ? "SteamVR shows Steam's page in its theater" : "Steam's page out of SteamVR's theater");
}

bool SteamDashInTheater() { return g_theater; }

// View in Theater, from Steam's tile: SteamVR's own (dashpatch calls its frame's SetInTheater), so
// its theater and its way out are SteamVR's; SteamDashTheater gives the windows back once it shows.
void SteamDashToTheater() {
    std::printf("Steam's tile: View in Theater (asked of SteamVR)\n");
    std::fflush(stdout);
    SendDashpatch("steamtheater");
}

// ft-screens going: the windows back to Steam first (half a second at most).
void SteamDashShutdown() {
    if (!g_link) return;
    LetGo();
    g_pages.reset();   // (the buttons up first)
    Do(g_fleet.Shutdown());
    steamhit::Clear();   // (none of them ours now: Steam's get their widths back)
    g_link->Stop(500);
}

}  // namespace ftvr
