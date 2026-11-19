// The grab bar's menu (Option C, user 2026-11-18: "a quick tap opens up the menu on the drag bar, and
// a hold and press acts as the current behavior"), and the edge each panel's bar is on.
//   A tap on the bar (vr.cpp's BarTap), a poke on it, or `barside N menu` opens the main page: what the
// panel's round buttons did, as rows (panelmenu.h: Curved, Rotate, Keep in View, On Dashboard, Stay
// Shown, Back to Desktop, Close Window, View in Theater, Move All Screens, Bar Position, Reset Layout).
// A right click on the bar, as before, or `barside N edges` opens the edges page: Bottom, Top, Left,
// Right (the panel's own marked), and View in Theater where the panel has it. Bar Position goes there
// with Back first. It opens over the panel beside the bar at the spot clicked, toward its middle
// (barside::MenuAt), a row lit under a laser, the 3D mouse or a fingertip near it.
//   A click on a row does it and closes the menu; Rotate and Move All Screens are held (a press starts
// the roll or the layout's carry, the release ends it; Rotate let go without turning levels the panel,
// as the roll button's click did). A fingertip coming through a row presses it (tiptap.h): Rotate levels.
// It closes on its own after kMenuIdleMs with nothing on it, when its panel hides, or on another tap on
// the same bar. Its panel's controls stay shown while it's open. One overlay for every panel.
//   Each panel's edge is remembered by its key (TileKnown's: "screen:2", "app:ID", "remote:HOST/APP")
// in ~/.config/frametop-bars.txt, a line each, "KEY<TAB>EDGE"; a panel with no key yet keeps its edge
// for the session. The geometry is barside.h's; vr.cpp places the controls by Screen::barSide.
//   View in Theater: Steam's window in SteamVR's theater, SteamVR's own way (vrsteamwin.cpp:
// SteamDashToTheater); Steam's page hides SteamVR's own button for that while it is a tile
// (pointer/dashpatch/steampage.py), so this is where it went. A Steam game's panel has it too: the
// game's window lent to SteamVR's theater (vrgsapps.cpp: GsToTheater).
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"
#include "handsoff.h"
#include "panelmenu.h"
#include "posemath.h"
#include "tiptap.h"

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
constexpr int64_t kMenuLeastMs = 300;   // a fingertip's tap no sooner (the poke that opened it, still on its way)
constexpr int kMenuText = 30, kMenuHint = 24;

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int panel = -1;            // the panel it's for, -1 closed
    bool edges = false;        // on its edges page (a right click opens it there; Bar Position goes there)
    bool back = false;         // ...with Back to the main page first
    std::vector<panelmenu::Row> rows;
    double along = 0;          // where on its bar it was opened (metres from the bar's middle)
    double w = 0, h = 0;       // its size now (m)
    Mat pose{};                // ...and where it is
    int hover = -1;            // the row under a laser, -1 none
    int handHover = -1;        // ...under a fingertip near it
    int64_t openedMs = -1, usedMs = -1;   // opened; opened, or a laser moved on it, then
    vr::TrackedDeviceIndex_t holding = kNone;   // a held row's device (Rotate, Move All Screens) till it's let go
    tiptap::Tap tip[2];        // each index tip's way through it
    tiptap::Tap beside[2];     // ...and through its panel beside it (a tap there closes it)
    std::string drawn;         // what its texture shows
    bool lasers = false;       // MakeOverlaysInteractiveIfVisible is set
} g_menu;

handsoff::Hold g_handsOff;   // the panel a fingertip pressed a row of, or tapped beside it (handsoff.h)

// That side's index tip on a panel posed at `p`: {u, v, d} along its surface (curved or flat), up, out.
bool TipOnPanel(int side, const Screen &s, const Mat &p, double out[3]) {
    double q[3];
    if (!IndexTipIn(side, p, q)) return false;
    posemath::OnPanel(s.curve, q, out);
    return true;
}

// Each tick: the panel's back to hand input once the tip that pressed it is out in front.
void UpdateHandsOff() {
    const int i = g_handsOff.panel();
    if (i < 0) return;
    const auto it = g_screens.find(i);
    const bool there = it != g_screens.end();
    Mat p;
    double q[3];
    const bool seen = there && ScreenPose(it->second, &p) && IndexTipIn(g_handsOff.side(), p, q);
    g_handsOff.Step(seen ? q : nullptr, there ? it->second.curve : 0, there ? it->second.metres / 2 : 0,
                    there ? it->second.heightMetres() / 2 : 0, NowMs(), there);
}

void CloseMenu(const char *why) {
    if (g_menu.panel < 0) return;
    std::printf("screen %d: grab bar menu closed (%s)\n", g_menu.panel + 1, why);
    std::fflush(stdout);
    PinHide(g_menu.overlay);
    g_menu.panel = -1, g_menu.hover = g_menu.handHover = -1, g_menu.holding = kNone;
    g_menu.rows.clear();
}

panelmenu::Kind KindOf(const Screen &s, int i) {
    panelmenu::Kind k;
    k.floating = s.floating, k.gsapp = s.gsapp;
    k.theater = i == SteamDashIndex() || GsCanTheater(i);
    k.anchored = TileAnchored(i);
    return k;
}

// A toggle on (its dot shows), or the panel's own edge.
bool Checked(const panelmenu::Row &r, const Screen &s, int i) {
    switch (r.item) {
        case panelmenu::Item::Curve: return s.curve > 0;
        case panelmenu::Item::KeepInView: return s.pinned == vr::k_unTrackedDeviceIndex_Hmd;
        case panelmenu::Item::OnDashboard: return TileAnchored(i);
        case panelmenu::Item::StayShown: return TileStays(s, i);
        case panelmenu::Item::Edge: return s.barSide == r.side;
        default: return false;
    }
}

// The grey words at a row's right: what a hold does, the edge it's on.
std::string Hint(const panelmenu::Row &r, const Screen &s) {
    switch (r.item) {
        case panelmenu::Item::Rotate: return "hold to turn";
        case panelmenu::Item::MoveAll: return "hold";
        case panelmenu::Item::BarPosition: return std::string(barside::Label(s.barSide)) + " \xe2\x80\xba";
        default: return "";
    }
}

// SteamVR's dark menu: a row each, the one under a laser or a fingertip white, a dot on each toggle
// that's on, grey hints at the right, a rule between groups, reset in red.
void DrawMenu(const Screen &s, int i) {
    const int W = barside::kMenuW, n = int(g_menu.rows.size()), H = barside::MenuH(n), R = barside::kRowH, pad = barside::kMenuPad;
    const int lit = g_menu.hover >= 0 ? g_menu.hover : g_menu.handHover;
    std::string what = std::to_string(lit) + "/" + std::to_string(int(s.barSide)) + "/";
    for (const auto &r : g_menu.rows) what += char('a' + int(r.item)), what += Checked(r, s, i) ? '1' : '0';
    if (what == g_menu.drawn) return;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    const uint8_t bg[4] = {24, 24, 30, 230}, on[4] = {240, 240, 244, 240}, rule[4] = {90, 90, 100, 255};
    FillRound(px, W, H, 0, 0, W, H, R / 2, bg);
    for (int r = 0; r < n; ++r) {
        const panelmenu::Row &row = g_menu.rows[size_t(r)];
        const int y0 = pad + r * R, cy = y0 + R / 2;
        if (panelmenu::RuleAbove(g_menu.rows, size_t(r))) FillRound(px, W, H, pad * 3, y0 - 1, W - pad * 3, y0 + 1, 1, rule);
        const bool hot = r == lit;
        if (hot) FillRound(px, W, H, pad, y0 + 3, W - pad, y0 + R - 3, (R - 6) / 2, on);
        const bool red = row.item == panelmenu::Item::Reset;
        const uint8_t ink[4] = {uint8_t(hot ? 20 : red ? 255 : 245), uint8_t(hot ? 20 : red ? 120 : 245), uint8_t(hot ? 24 : red ? 110 : 250), 255};
        const uint8_t dim[3] = {uint8_t(hot ? 70 : 150), uint8_t(hot ? 70 : 155), uint8_t(hot ? 80 : 165)};
        if (panelmenu::Toggle(row.item) && Checked(row, s, i)) FillRound(px, W, H, 34 - 7, cy - 7, 34 + 7, cy + 7, 7, ink);
        const char *text = panelmenu::Label(row);
        label::Draw(px.data(), W, H, text, kMenuText, 60 + label::Width(text, kMenuText) / 2, cy, 255, ink);
        const std::string hint = Hint(row, s);
        if (!hint.empty())
            label::Draw(px.data(), W, H, hint.c_str(), kMenuHint, W - 28 - label::Width(hint.c_str(), kMenuHint) / 2, cy, 255, dim);
    }
    overlaytex::Set(g_menu.overlay, px.data(), W, H);
    const vr::HmdVector2_t scale = {{float(W), float(H)}};
    vr::VROverlay()->SetOverlayMouseScale(g_menu.overlay, &scale);
    g_menu.drawn = what;
}

void Rows(const Screen &s, int i) {
    const panelmenu::Kind k = KindOf(s, i);
    g_menu.rows = g_menu.edges ? panelmenu::Edges(k, g_menu.back) : panelmenu::Main(k);
}

// A row pressed (`dev` a laser's or the 3D mouse's; kNone: a fingertip, `by` says whose). Held rows start
// here and end as the press is let go (PollBarMenu, UpdateBarMenu); the rest do their thing and close it.
void PressRow(int row, vr::TrackedDeviceIndex_t dev, const char *by) {
    const int i = g_menu.panel;
    const auto it = g_screens.find(i);
    if (it == g_screens.end() || row < 0 || row >= int(g_menu.rows.size())) return;
    Screen &s = it->second;
    const panelmenu::Row r = g_menu.rows[size_t(row)];
    std::printf("screen %d: grab bar menu: %s (%s)\n", i + 1, panelmenu::Label(r), by);
    using panelmenu::Item;
    switch (r.item) {
        case Item::BarPosition: g_menu.edges = g_menu.back = true, Rows(s, i), g_menu.hover = -1; return;
        case Item::Back: g_menu.edges = g_menu.back = false, Rows(s, i), g_menu.hover = -1; return;
        case Item::Rotate:
            if (dev == kNone) break;   // (a fingertip's tap levels it)
            StartDrag(s, Drag::Roll, dev);
            if (s.drag == Drag::Roll) g_menu.holding = dev;
            return;
        case Item::MoveAll:
            if (dev == kNone) return (void)std::printf("screen %d: Move All Screens is a laser's hold\n", i + 1);
            LayoutGrabStart(s, i, dev);
            if (s.drag == Drag::Move) g_menu.holding = dev;
            return;
        default: break;
    }
    CloseMenu(panelmenu::Label(r));
    switch (r.item) {
        case Item::Curve: ToggleCurve(s); break;
        case Item::Rotate: LevelScreen(s, i, by); break;
        case Item::KeepInView: TogglePinView(s, i, by); break;
        case Item::OnDashboard:
            if (TileAnchored(i)) TileUnanchor(i, by);
            else TileAnchor(i, dashtiles::Stay::Default, by);
            break;
        case Item::StayShown: TileStay(i, !TileStays(s, i), by); break;
        case Item::Dock: SendFloat("dock " + std::to_string(i + 1)); break;
        case Item::Close: SendFloat("close " + std::to_string(i + 1)); break;
        case Item::Theater:
            if (i == SteamDashIndex()) SteamDashToTheater();
            else GsToTheater(i);
            break;
        case Item::Reset: ShowResetAsk(i); break;
        case Item::Edge: SetBarSide(i, r.side, "its bar's menu"); break;
        default: break;
    }
}

// The row under a point on its face (metres from its centre, y up), -1 none.
int RowAtFace(double x, double y) {
    if (g_menu.w <= 0 || g_menu.h <= 0) return -1;
    return barside::RowAt((x / g_menu.w + 0.5) * barside::kMenuW, (y / g_menu.h + 0.5) * barside::MenuH(int(g_menu.rows.size())),
                          int(g_menu.rows.size()));
}

// Each hands frame: a fingertip near it lights the row under it (and keeps it open), one coming through
// it presses that row, and one coming through its panel beside it closes it, as a tap outside a menu
// does. Hand input has none of the panel meanwhile (BarMenuHasHands), so neither goes on into it.
void HandsOnMenu(const Screen &s, const Mat &panel) {
    if (!HandsFrame()) return;
    g_menu.handHover = -1;
    const int i = g_menu.panel;
    for (int k = 0; k < 2; ++k) {
        double l[3], q[3];
        const bool seen = IndexTipIn(k, g_menu.pose, l), onPanel = TipOnPanel(k, s, panel, q);
        const bool through = g_menu.tip[k].Step(seen ? l : nullptr, g_menu.w / 2, g_menu.h / 2);
        const bool overMenu = seen && std::fabs(l[0]) <= g_menu.w / 2 + tiptap::Tap::kMargin &&
                              std::fabs(l[1]) <= g_menu.h / 2 + tiptap::Tap::kMargin;
        const bool beside = g_menu.beside[k].Step(onPanel && !overMenu ? q : nullptr, s.metres / 2, s.heightMetres() / 2);
        if (seen && g_menu.tip[k].Near()) g_menu.handHover = RowAtFace(l[0], l[1]), g_menu.usedMs = NowMs();
        if (NowMs() - g_menu.openedMs < kMenuLeastMs) continue;
        const char *by = k ? "the right hand" : "the left hand";
        if (through) {
            g_menu.usedMs = NowMs();
            g_handsOff.Start(i, k, s.curve, NowMs());
            return PressRow(RowAtFace(l[0], l[1]), kNone, by);
        }
        if (beside) {
            g_handsOff.Start(i, k, s.curve, NowMs());
            return CloseMenu("tapped beside it");
        }
    }
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
bool BarMenuHasHands(int i) { return BarMenuOn(i) || g_handsOff.Holds(i); }

void OpenBarMenu(int i, double along, bool edges) {
    if (g_menu.panel == i && g_menu.edges == edges) return CloseMenu("its bar tapped again");
    if (g_menu.panel == i) {   // (a right click on its bar with the main page open, or a tap with the edges open)
        g_menu.edges = edges, g_menu.back = false, g_menu.hover = -1, g_menu.usedMs = NowMs();
        const auto it = g_screens.find(i);
        if (it != g_screens.end()) Rows(it->second, i);
        return;
    }
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return;
    if (g_menu.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kMenuKey, "Frametop: grab bar menu", &g_menu.overlay) != vr::VROverlayError_None) {
            g_menu.overlay = vr::k_ulOverlayHandleInvalid;
            return (void)std::printf("grab bar menu: can't make %s\n", kMenuKey);
        }
        vr::VROverlay()->SetOverlayInputMethod(g_menu.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlaySortOrder(g_menu.overlay, 10);
        AnnounceOverlay(kMenuKey);   // (the 3D mouse's ray finds it)
    }
    if (g_menu.panel >= 0) CloseMenu("another bar's opened");
    g_menu.panel = i, g_menu.along = along, g_menu.edges = edges, g_menu.back = false;
    g_menu.hover = g_menu.handHover = -1, g_menu.holding = kNone;
    g_menu.openedMs = g_menu.usedMs = NowMs(), g_menu.drawn.clear();
    for (tiptap::Tap &t : g_menu.tip) t.Reset();
    for (tiptap::Tap &t : g_menu.beside) t.Reset();
    Rows(it->second, i);
    std::printf("screen %d: grab bar menu open (%s; its bar on the %s)\n", i + 1, edges ? "its edges" : "tapped",
                barside::Name(it->second.barSide));
    std::fflush(stdout);
    UpdateBarMenu();
    if (g_menu.panel >= 0) PinShow(g_menu.overlay);
}

// Each tick: it follows its panel's bar (moved, rolled, or turned to another edge), its rows follow the
// panel (anchored: Stay Shown), and it goes when it's done.
void UpdateBarMenu() {
    UpdateHandsOff();
    if (g_menu.panel < 0) return;
    const int i = g_menu.panel;
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return CloseMenu("its panel went");
    Screen &s = it->second;
    if (!s.visible || g_paused) return CloseMenu("its panel hid");
    if (g_menu.holding != kNone) {
        if (s.drag == Drag::None || s.dragDevice != g_menu.holding) return CloseMenu("let go");   // (let go off it)
        g_menu.usedMs = NowMs();
    }
    if (NowMs() - g_menu.usedMs >= kMenuIdleMs) return CloseMenu("not used");
    Mat p, head;
    if (!ScreenPose(s, &p)) return CloseMenu("its panel isn't placed");
    Rows(s, i);
    if (g_menu.hover >= int(g_menu.rows.size())) g_menu.hover = -1;
    if (g_menu.lasers != s.lasers) {
        g_menu.lasers = s.lasers;
        vr::VROverlay()->SetOverlayFlag(g_menu.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
    }
    // 0.45 mm a pixel at 1 m (as the gamepad pill): its text about the pill's size
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    const double mpp = 0.00045 * d;
    g_menu.w = barside::kMenuW * mpp, g_menu.h = barside::MenuH(int(g_menu.rows.size())) * mpp;
    const double half = barhit::Width(s.chrome, g_barPaddingX) / 2;
    const barside::UV at = barside::Place(s.barSide, s.metres, s.heightMetres(), BarIn(s), std::clamp(g_menu.along, -half, half));
    const barside::UV c = barside::MenuAt(s.barSide, at, barhit::Height(s.chrome, g_barPaddingY) / 2,
                                          std::max(s.grip, s.chrome * 0.06), g_menu.w, g_menu.h);
    g_menu.pose = Mul(p, OnSurface(s, c.u, c.v, 0.004));
    vr::VROverlay()->SetOverlayWidthInMeters(g_menu.overlay, float(g_menu.w));
    vr::VROverlay()->SetOverlayTransformAbsolute(g_menu.overlay, vr::TrackingUniverseStanding, &g_menu.pose);
    if (g_menu.holding == kNone) HandsOnMenu(s, p);
    if (g_menu.panel >= 0) DrawMenu(s, i);
}

// Its input: a laser over a row lights it, a press there does it (a held row until it's let go).
void PollBarMenu(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (g_menu.overlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->PollNextOverlayEvent(g_menu.overlay, &ev, sizeof ev)) {
        const int n = int(g_menu.rows.size());
        if (ev.eventType == vr::VREvent_FocusEnter || ev.eventType == vr::VREvent_MouseMove) {
            g_menu.hover = g_menu.holding != kNone ? g_menu.hover : barside::RowAt(ev.data.mouse.x, ev.data.mouse.y, n);
            g_menu.usedMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            if (g_menu.holding == kNone) g_menu.hover = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && g_menu.holding == kNone &&
                   (ev.data.mouse.button == vr::VRMouseButton_Left || ev.data.mouse.button == vr::VRMouseButton_Right)) {
            if (g_menu.panel < 0) continue;
            g_menu.usedMs = NowMs();
            PressRow(barside::RowAt(ev.data.mouse.x, ev.data.mouse.y, n), ev.trackedDeviceIndex, DeviceLabel(ev.trackedDeviceIndex).c_str());
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            const vr::TrackedDeviceIndex_t dev = ev.trackedDeviceIndex;
            const int panel = g_menu.panel;
            const auto it = g_screens.find(panel);
            // Rotate pressed and let go without turning: a click, which levels it (as the roll button did)
            const bool level = g_menu.holding == dev && it != g_screens.end() && it->second.drag == Drag::Roll &&
                               it->second.dragDevice == dev && RollClick(it->second);
            if (g_menu.holding == dev) CloseMenu(level ? "Rotate tapped" : "let go");
            EndDragsBy(dev);
            if (level) LevelScreen(it->second, panel, DeviceLabel(dev).c_str());
            ReleaseAwayBy(dev, ev.data.mouse.button, handle, data);
        }
    }
}

}  // namespace ftvr
