// The edge a panel's grab bar and buttons are on, and the bar's right-click menu that picks it
// (user, 2026-11-18: "if you right-click on the grab bar, a context menu will open up that lets you
// choose for that one display where the grab bar should actually be... bottom, top, left, or right").
//   Each panel's edge is remembered by its key (TileKnown's: "screen:2", "app:ID", "remote:HOST/APP")
// in ~/.config/frametop-bars.txt, a line each, "KEY<TAB>EDGE"; a panel with no key yet keeps its edge
// for the session. The geometry is barside.h's; vr.cpp places the controls by Screen::barSide.
//   The menu: four rows (Bottom, Top, Left, Right; the panel's own edge marked), out past the bar at
// the spot clicked, a row lit under a laser or the pointer. A click on a row moves the bar there and
// closes it; it closes on its own after kMenuIdleMs with nothing on it, when its panel hides, or on
// another right click on the same bar. Its panel's controls stay shown while it's open.
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ftvr {
namespace {

dashtiles::Memory g_barMemory;          // key -> edge name (barside::Name)
std::map<int, std::string> g_barKey;    // panel -> its key (TileKnown's)

std::string BarsPath() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/frametop-bars.txt";
}
// (a pick is a rare, deliberate act: written at once)
void WriteBars() {
    const std::string path = BarsPath(), tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return (void)std::fprintf(stderr, "grab bar edges: can't write %s\n", tmp.c_str());
    const std::string text = g_barMemory.Text();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0)
        std::fprintf(stderr, "grab bar edges: can't write %s\n", path.c_str());
}

// Moved to an edge: its controls go there now.
void Apply(Screen &s, barside::Side side) {
    if (s.barSide == side) return;
    s.barSide = side;
    PlaceChrome(s);
}

constexpr const char *kMenuKey = "frametop.barmenu";
constexpr int64_t kMenuIdleMs = 6000;   // nothing on it this long: it goes
constexpr int kMenuText = 30;

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int panel = -1;            // the panel it's for, -1 closed
    double along = 0;          // where on its bar it was opened (metres from the bar's middle)
    int hover = -1;            // the row under a laser, -1 none
    int64_t usedMs = -1;       // opened, or a laser moved on it, then
    int drawnHover = -2;       // (what its texture shows: -2 nothing yet)
    barside::Side drawnSide = barside::Side::Bottom;
    bool lasers = false;       // MakeOverlaysInteractiveIfVisible is set
} g_menu;

void CloseMenu(const char *why) {
    if (g_menu.panel < 0) return;
    std::printf("screen %d: grab bar menu closed (%s)\n", g_menu.panel + 1, why);
    std::fflush(stdout);
    PinHide(g_menu.overlay);
    g_menu.panel = -1, g_menu.hover = -1;
}

// The four edges, SteamVR's dark pill style: the row under a laser white, the panel's own edge marked.
void DrawMenu(barside::Side current) {
    const int W = barside::kMenuW, H = barside::kMenuH, R = barside::kRowH, pad = barside::kMenuPad;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    const uint8_t bg[4] = {24, 24, 30, 225}, lit[4] = {240, 240, 244, 240};
    FillRound(px, W, H, 0, 0, W, H, R / 2, bg);
    for (int r = 0; r < barside::kSides; ++r) {
        const int y0 = pad + r * R;
        const bool on = r == g_menu.hover;
        if (on) FillRound(px, W, H, pad, y0 + 3, W - pad, y0 + R - 3, (R - 6) / 2, lit);
        const uint8_t ink[4] = {uint8_t(on ? 20 : 245), uint8_t(on ? 20 : 245), uint8_t(on ? 24 : 250), 255};
        if (barside::kAll[r] == current) FillRound(px, W, H, 34 - 7, y0 + R / 2 - 7, 34 + 7, y0 + R / 2 + 7, 7, ink);
        label::Draw(px.data(), W, H, barside::Label(barside::kAll[r]), kMenuText, W / 2, y0 + R / 2, 255, ink);
    }
    overlaytex::Set(g_menu.overlay, px.data(), W, H);
    const vr::HmdVector2_t scale = {{float(W), float(H)}};
    vr::VROverlay()->SetOverlayMouseScale(g_menu.overlay, &scale);
    g_menu.drawnHover = g_menu.hover, g_menu.drawnSide = current;
}

}  // namespace

void LoadBarSides() {
    std::FILE *f = std::fopen(BarsPath().c_str(), "r");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1 << 20)) text.append(buf, n);
    std::fclose(f);
    g_barMemory.Load(text);
    std::printf("grab bar edges: %zu remembered (%s)\n", g_barMemory.Size(), BarsPath().c_str());
}

void BarSideKnown(int i, const std::string &key) {
    g_barKey[i] = key;
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return;
    barside::Side side = barside::Side::Bottom;
    if (!barside::Parse(g_barMemory.Get(key), &side)) {
        // nothing remembered: an edge picked before its key came is its key's now
        if (it->second.barSide != barside::Side::Bottom && g_barMemory.Set(key, barside::Name(it->second.barSide))) WriteBars();
        return;
    }
    g_barMemory.Used(key);
    if (side != it->second.barSide)
        std::printf("screen %d: its grab bar on the %s (remembered for %s)\n", i + 1, barside::Name(side), key.c_str());
    Apply(it->second, side);
}

// Its app or display went: the next one on the panel starts at the bottom (or its own remembered edge).
void BarSideGone(int i) {
    if (g_menu.panel == i) CloseMenu("its panel went");
    g_barKey.erase(i);
    const auto it = g_screens.find(i);
    if (it != g_screens.end()) it->second.barSide = barside::Side::Bottom;
}

bool SetBarSide(int i, barside::Side side, const char *by) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return false;
    const auto key = g_barKey.find(i);
    if (key != g_barKey.end() && g_barMemory.Set(key->second, barside::Name(side))) WriteBars();
    std::printf("screen %d: its grab bar on the %s (%s)%s\n", i + 1, barside::Name(side), by,
                key == g_barKey.end() ? "; not remembered: it has no key yet" : "");
    std::fflush(stdout);
    Apply(it->second, side);
    return true;
}

bool BarMenuOn(int i) { return g_menu.panel >= 0 && g_menu.panel == i; }

void OpenBarMenu(int i, double along) {
    if (g_menu.panel == i) return CloseMenu("its bar right-clicked again");
    if (!g_screens.count(i)) return;
    if (g_menu.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kMenuKey, "Frametop: grab bar edge", &g_menu.overlay) != vr::VROverlayError_None) {
            g_menu.overlay = vr::k_ulOverlayHandleInvalid;
            return (void)std::printf("grab bar menu: can't make %s\n", kMenuKey);
        }
        vr::VROverlay()->SetOverlayInputMethod(g_menu.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlaySortOrder(g_menu.overlay, 10);
        AnnounceOverlay(kMenuKey);   // (the 3D mouse's ray finds it)
    }
    if (g_menu.panel >= 0) CloseMenu("another bar's opened");
    g_menu.panel = i, g_menu.along = along, g_menu.hover = -1, g_menu.usedMs = NowMs(), g_menu.drawnHover = -2;
    std::printf("screen %d: grab bar menu open (its bar on the %s)\n", i + 1, barside::Name(g_screens[i].barSide));
    std::fflush(stdout);
    UpdateBarMenu();
    if (g_menu.panel >= 0) PinShow(g_menu.overlay);
}

// Each tick: it follows its panel's bar (moved, or turned to another edge), and goes when it's done.
void UpdateBarMenu() {
    if (g_menu.panel < 0) return;
    const auto it = g_screens.find(g_menu.panel);
    if (it == g_screens.end()) return CloseMenu("its panel went");
    Screen &s = it->second;
    if (!s.visible || g_paused) return CloseMenu("its panel hid");
    if (NowMs() - g_menu.usedMs >= kMenuIdleMs) return CloseMenu("not used");
    Mat p, head;
    if (!ScreenPose(s, &p)) return CloseMenu("its panel isn't placed");
    if (g_menu.drawnHover != g_menu.hover || g_menu.drawnSide != s.barSide) DrawMenu(s.barSide);
    if (g_menu.lasers != s.lasers) {
        g_menu.lasers = s.lasers;
        vr::VROverlay()->SetOverlayFlag(g_menu.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
    }
    // 0.45 mm a pixel at 1 m (as the gamepad pill): its text about the pill's size
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    const double mpp = 0.00045 * d, w = barside::kMenuW * mpp, h = barside::kMenuH * mpp;
    const double half = barhit::Width(s.chrome, g_barPaddingX) / 2;
    const barside::UV at = barside::Place(s.barSide, s.metres, s.heightMetres(), BarOut(s), std::clamp(g_menu.along, -half, half));
    const barside::UV c = barside::MenuAt(s.barSide, at, barhit::Height(s.chrome, g_barPaddingY) / 2,
                                          std::max(s.grip, s.chrome * 0.06), w, h);
    const Mat m = Mul(p, OnSurface(s, c.u, c.v, 0.004));
    vr::VROverlay()->SetOverlayWidthInMeters(g_menu.overlay, float(w));
    vr::VROverlay()->SetOverlayTransformAbsolute(g_menu.overlay, vr::TrackingUniverseStanding, &m);
}

// Its input: a laser over a row lights it, a click there moves the bar to that edge.
void PollBarMenu(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (g_menu.overlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->PollNextOverlayEvent(g_menu.overlay, &ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_FocusEnter || ev.eventType == vr::VREvent_MouseMove) {
            g_menu.hover = barside::RowAt(ev.data.mouse.x, ev.data.mouse.y);
            g_menu.usedMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_menu.hover = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown &&
                   (ev.data.mouse.button == vr::VRMouseButton_Left || ev.data.mouse.button == vr::VRMouseButton_Right)) {
            const int row = barside::RowAt(ev.data.mouse.x, ev.data.mouse.y), panel = g_menu.panel;
            if (row < 0 || panel < 0) continue;
            SetBarSide(panel, barside::kAll[row], "its bar's menu");
            CloseMenu("picked");
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
}

}  // namespace ftvr
