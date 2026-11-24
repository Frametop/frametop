// The grab bar's menu (Option C, user 2026-11-18: "a quick tap opens up the menu on the drag bar, and
// a hold and press acts as the current behavior"), and the edge each panel's bar is on. It looks as the
// Option C design draws it (user, 2026-11-19: "Use that as the design"; barmenuart.h, menulayout.h).
//   A tap on the bar (vr.cpp's BarTap), a poke on it, or `barside N menu` opens the main page: quick tiles
// on top (Curved, Rotate; a floating window's To Desktop and Close), then Placement, a PC app window's Scale
// (vrappscale.cpp), View in Theater, Move All Windows, Bar Position and Reset Layout (panelmenu.h). Placement opens its page: In the Room,
// Keep in View, Tiled (the one it is checked) and Stay Shown while it's on the dashboard.
// A right click on the bar, as before, or `barside N edges` opens the edges page: Bottom, Top, Left,
// Right (the panel's own checked), and View in Theater where the panel has it. Bar Position goes there
// with Back first. It opens over the panel beside the bar at the spot clicked, toward its middle
// (barside::MenuAt), a row lit under a laser, the 3D mouse or a fingertip near it; its bar goes blue.
//   A click on a row does it and closes the menu; Rotate and Move All Windows are held (a press starts
// the roll or the layout's carry, the release ends it; Rotate let go without turning levels the panel,
// as the roll button's click did). A fingertip coming through a row presses it (tiptap.h): Rotate levels.
// It closes on its own after kMenuIdleMs with nothing on it, when its panel hides, or on another tap on
// the same bar. Its panel's controls stay shown while it's open. One overlay for every panel.
//   Each panel's edge, and its memory, are vrbarside.cpp's.
//   View in Theater: Steam's window in SteamVR's theater, SteamVR's own way (vrsteamwin.cpp:
// SteamDashToTheater); Steam's page hides SteamVR's own button for that while it is a tile
// (pointer/dashpatch/steampage.py), so this is where it went. A Steam game's panel has it too: the
// game's window lent to SteamVR's theater (vrgsapps.cpp: GsToTheater).
#include "vrpanel.h"
#include "appscale.h"
#include "barmenuart.h"
#include "overlaytex.h"
#include "handsoff.h"
#include "menulayout.h"
#include "panelmenu.h"
#include "posemath.h"
#include "tiptap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ftvr {
namespace {

constexpr const char *kMenuKey = "frametop.barmenu";
constexpr int64_t kMenuIdleMs = 6000;   // nothing on it this long: it goes
constexpr int64_t kMenuLeastMs = 300;   // a fingertip's tap no sooner (the poke that opened it, still on its way)

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int panel = -1;            // the panel it's for, -1 closed
    panelmenu::Page page = panelmenu::Page::Main;   // a right click opens the edges; Placement, Bar Position go on
    bool back = false;         // ...with Back to the main page first
    std::vector<panelmenu::Row> rows;
    menulayout::Layout layout; // where they sit on its texture
    double along = 0;          // where on its bar it was opened (metres from the bar's middle)
    double w = 0, h = 0;       // its size now (m)
    Mat pose{};                // ...and where it is
    double lift = 0;           // ...out in front of its panel's surface (posemath::CardLift)
    int hover = -1;            // the row under a laser, -1 none
    int handHover = -1;        // ...under a fingertip near it
    int64_t openedMs = -1, usedMs = -1;   // opened; opened, or a laser moved on it, then
    vr::TrackedDeviceIndex_t holding = kNone;   // a held row's device (Rotate, Move All Windows) till it's let go
    tiptap::Tap tip[2];        // each index tip's way through it
    tiptap::Tap beside[2];     // ...and through its panel beside it (a tap there closes it)
    std::string drawn;         // what its texture shows
    std::string basePage;      // ...what its unlit pixels are of (the page, its rows' looks)
    std::vector<barmenuart::RowLook> looks;   // ...their looks
    std::vector<uint8_t> base; // ...and those pixels (barmenuart::Base)
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
    const int i = g_menu.panel;
    g_menu.panel = -1, g_menu.hover = g_menu.handHover = -1, g_menu.holding = kNone;
    g_menu.rows.clear();
    // its bar back from lit to idle now, not on the next thing that touches its alpha
    const auto it = g_screens.find(i);
    if (it != g_screens.end()) ApplyAlpha(it->second), PaintBar(it->second);
}

panelmenu::Kind KindOf(const Screen &s, int i) {
    panelmenu::Kind k;
    k.floating = s.floating, k.gsapp = s.gsapp;
    k.theater = i == SteamDashIndex() || GsCanTheater(i);
    k.anchored = TileAnchored(i);
    k.scales = AppScale(i) > 0;
    k.rides = s.pinned != kNone && s.pinned != kDashboard;
    return k;
}

// Where the panel lives (Placement's value and its check).
enum class Where { Room, View, Dashboard, Wrist };
Where Placed(const Screen &s, int i) {
    if (TileAnchored(i) || s.pinned == kDashboard) return Where::Dashboard;   // (a tile, or `ft-layout pin N dashboard`)
    if (s.pinned == vr::k_unTrackedDeviceIndex_Hmd) return Where::View;
    if (s.pinned != kNone) return Where::Wrist;
    return Where::Room;
}

// A toggle on, or the choice picked: the panel's placement, its bar's edge.
bool On(const panelmenu::Row &r, const Screen &s, int i) {
    switch (r.item) {
        case panelmenu::Item::Curve: return s.curve > 0;
        case panelmenu::Item::InRoom: return Placed(s, i) == Where::Room;
        case panelmenu::Item::KeepInView: return Placed(s, i) == Where::View;
        case panelmenu::Item::OnDashboard: return Placed(s, i) == Where::Dashboard;
        case panelmenu::Item::StayShown: return TileStays(s, i);
        case panelmenu::Item::Edge: return s.barSide == r.side;
        case panelmenu::Item::ScaleStep: return appscale::Same(AppScale(i), r.percent / 100.0);
        default: return false;
    }
}

// What a row shows (barmenuart.h draws it): the design's icon, its value at the right, its check.
barmenuart::RowLook Look(const panelmenu::Row &r, const Screen &s, int i, const char *caption) {
    using barmenuicons::Icon;
    using panelmenu::Item;
    barmenuart::RowLook l;
    l.text = panelmenu::Label(r);
    const Where w = Placed(s, i);
    switch (r.item) {
        case Item::Curve: l.icon = Icon::Curved; break;
        case Item::Rotate: l.icon = Icon::Rotate; break;
        case Item::Dock: l.icon = Icon::Desktop; break;
        case Item::Close: l.icon = Icon::Close, l.red = true; break;
        case Item::Placement:
            l.icon = w == Where::View ? Icon::InView : w == Where::Dashboard ? Icon::Dashboard : w == Where::Wrist ? Icon::Controller : Icon::InRoom;
            l.value = w == Where::View ? "Keep in View" : w == Where::Dashboard ? "Tiled" : w == Where::Wrist ? "On a Wrist" : "In the Room";
            l.valueBlue = w != Where::Room;
            break;
        case Item::InRoom: l.icon = Icon::InRoom; break;
        case Item::KeepInView: l.icon = Icon::InView; break;
        case Item::OnDashboard: l.icon = Icon::Dashboard; break;
        case Item::StayShown: l.icon = Icon::StayShown; break;
        case Item::MoveAll: l.icon = Icon::MoveAll, l.value = "Hold"; break;   // (a click alone carries nothing)
        case Item::Theater: l.icon = Icon::Theater; break;
        case Item::BarPosition: l.icon = Icon::BarPosition, l.value = barside::Label(s.barSide); break;
        case Item::Reset: l.icon = Icon::Reset, l.red = true; break;
        case Item::Back: l.icon = Icon::Back; break;
        case Item::Edge: break;
        case Item::Scale: l.icon = Icon::Scale, l.value = appscale::Label(appscale::Percent(AppScale(i))); break;
        case Item::ScaleStep: break;
    }
    l.chevron = panelmenu::Opens(r.item);
    const bool on = On(r, s, i);
    l.on = r.item == Item::Curve && on;
    l.chosen = on && r.item != Item::Curve;
    if (caption) l.caption = caption;
    return l;
}

std::vector<menulayout::Spec> Specs(const std::vector<panelmenu::Row> &rows) {
    std::vector<menulayout::Spec> specs;
    for (size_t r = 0; r < rows.size(); ++r)
        specs.push_back({panelmenu::Tile(rows[r].item), panelmenu::RuleAbove(rows, r), panelmenu::Caption(rows, r) != nullptr});
    return specs;
}

// Its texture, drawn again only when what it shows changes: the page and its rows' looks (Base, kept),
// then the lit row over that (a laser sweeping down the list redraws one row a step, not the card).
void DrawMenu(const Screen &s, int i) {
    const menulayout::Layout &L = g_menu.layout;
    const int n = int(g_menu.rows.size()), lit = g_menu.hover >= 0 ? g_menu.hover : g_menu.handHover;
    const bool pressed = g_menu.holding != kNone;
    std::string page = std::to_string(int(s.barSide)) + "/" + std::to_string(int(Placed(s, i))) + "/" +
                       std::to_string(appscale::Percent(AppScale(i))) + "/";
    for (const auto &r : g_menu.rows) page += char('a' + int(r.item)), page += On(r, s, i) ? '1' : '0';
    const std::string what = std::to_string(lit) + "/" + (pressed ? "p" : "") + page;
    if (what == g_menu.drawn) return;
    if (page != g_menu.basePage) {
        g_menu.looks.clear();
        for (int r = 0; r < n; ++r) g_menu.looks.push_back(Look(g_menu.rows[size_t(r)], s, i, panelmenu::Caption(g_menu.rows, size_t(r))));
        g_menu.base = barmenuart::Base(g_menu.looks, L);
        g_menu.basePage = page;
    }
    const std::vector<uint8_t> px = barmenuart::Lit(g_menu.base, g_menu.looks, L, lit, pressed);
    overlaytex::Set(g_menu.overlay, px.data(), L.w, L.h);
    const vr::HmdVector2_t scale = {{float(L.w), float(L.h)}};
    vr::VROverlay()->SetOverlayMouseScale(g_menu.overlay, &scale);
    g_menu.drawn = what;
}

void Rows(const Screen &s, int i) {
    g_menu.rows = panelmenu::Rows(g_menu.page, KindOf(s, i), g_menu.back);
    g_menu.layout = menulayout::Lay(Specs(g_menu.rows));
}

// A page of the menu (Back: the main one).
void ToPage(const Screen &s, int i, panelmenu::Page p) {
    g_menu.page = p, g_menu.back = p != panelmenu::Page::Main, Rows(s, i), g_menu.hover = -1;
}

// A row pressed (`dev` a laser's or the 3D mouse's; kNone: a fingertip, `by` says whose). Held rows start
// here and end as the press is let go (PollBarMenu, UpdateBarMenu); the rest do their thing and close it.
void PressRow(int row, vr::TrackedDeviceIndex_t dev, const char *by) {
    const int i = g_menu.panel;
    const auto it = g_screens.find(i);
    if (it == g_screens.end() || row < 0 || row >= int(g_menu.rows.size())) return;
    Screen &s = it->second;
    const panelmenu::Row r = g_menu.rows[size_t(row)];
    std::printf("screen %d: grab bar menu: %s (%s)\n", i + 1, panelmenu::Label(r).c_str(), by);
    using panelmenu::Item;
    switch (r.item) {
        case Item::Placement: return ToPage(s, i, panelmenu::Page::Placement);
        case Item::Scale: return ToPage(s, i, panelmenu::Page::Scale);
        case Item::BarPosition: return ToPage(s, i, panelmenu::Page::Edges);
        case Item::Back: return ToPage(s, i, panelmenu::Page::Main);
        case Item::Rotate:
            if (dev == kNone) break;   // (a fingertip's tap levels it)
            StartDrag(s, Drag::Roll, dev);
            if (s.drag == Drag::Roll) g_menu.holding = dev, g_menu.hover = row;   // (lit while held, the laser anywhere)
            return;
        case Item::MoveAll:
            if (dev == kNone) return (void)std::printf("screen %d: Move All Windows is a laser's hold\n", i + 1);
            LayoutGrabStart(s, i, dev);
            if (s.drag == Drag::Move) g_menu.holding = dev, g_menu.hover = row;   // (lit while held, the laser anywhere)
            return;
        default: break;
    }
    CloseMenu(panelmenu::Label(r).c_str());
    switch (r.item) {
        case Item::Curve: ToggleCurve(s); break;
        case Item::Rotate: LevelScreen(s, i, by); break;
        case Item::InRoom: PlaceInRoom(s, i, by); break;
        case Item::KeepInView:
            if (s.pinned != vr::k_unTrackedDeviceIndex_Hmd) TogglePinView(s, i, by);   // (off the dashboard or a wrist too)
            break;
        case Item::OnDashboard:
            if (Placed(s, i) != Where::Dashboard) TileAnchor(i, dashtiles::Stay::Default, by);   // (the choice it is: nothing)
            break;
        case Item::StayShown: TileStay(i, !TileStays(s, i), by); break;
        case Item::Dock: SendFloat("dock " + std::to_string(i + 1)); break;
        case Item::Close: SendFloat("close " + std::to_string(i + 1)); break;
        case Item::Theater:
            if (i == SteamDashIndex()) SteamDashToTheater();
            else GsToTheater(i);
            break;
        case Item::Reset: DoReset(i, "its bar's menu"); break;   // (ft-layout's reset counts 3-2-1 in front of you first)
        case Item::ScaleStep: SetAppScale(i, r.percent / 100.0); break;
        case Item::Edge: SetBarSide(i, r.side, "its bar's menu"); break;
        default: break;
    }
}

// The row under a point on its face (metres from its centre, y up), -1 none.
int RowAtFace(double x, double y) {
    if (g_menu.w <= 0 || g_menu.h <= 0) return -1;
    return menulayout::At(g_menu.layout, (x / g_menu.w + 0.5) * g_menu.layout.w, (y / g_menu.h + 0.5) * g_menu.layout.h);
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
            g_handsOff.Start(i, k, s.curve, NowMs(), g_menu.lift);
            return PressRow(RowAtFace(l[0], l[1]), kNone, by);
        }
        if (beside) {
            g_handsOff.Start(i, k, s.curve, NowMs());
            return CloseMenu("tapped beside it");
        }
    }
}

}  // namespace
void CloseBarMenu(const char *why) { CloseMenu(why); }
bool BarMenuOn(int i) { return g_menu.panel >= 0 && g_menu.panel == i; }
bool BarMenuHasHands(int i) { return BarMenuOn(i) || g_handsOff.Holds(i); }

void OpenBarMenu(int i, double along, bool edges) {
    const panelmenu::Page page = edges ? panelmenu::Page::Edges : panelmenu::Page::Main;
    if (g_menu.panel == i && (g_menu.page == page || !edges)) return CloseMenu("its bar tapped again");
    if (g_menu.panel == i) {   // (a right click on its bar with another page open)
        g_menu.page = page, g_menu.back = false, g_menu.hover = -1, g_menu.usedMs = NowMs();
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
        vr::VROverlay()->SetOverlayFlag(g_menu.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);   // (a carry's wheel over it: PollOverlay)
        AnnounceOverlay(kMenuKey);   // (the 3D mouse's ray finds it)
    }
    if (g_menu.panel >= 0) CloseMenu("another bar's opened");
    g_menu.panel = i, g_menu.along = along, g_menu.page = page, g_menu.back = false;
    g_menu.hover = g_menu.handHover = -1, g_menu.holding = kNone;
    g_menu.openedMs = g_menu.usedMs = NowMs(), g_menu.drawn.clear(), g_menu.basePage.clear();
    for (tiptap::Tap &t : g_menu.tip) t.Reset();
    for (tiptap::Tap &t : g_menu.beside) t.Reset();
    Rows(it->second, i);
    std::printf("screen %d: grab bar menu open (%s; its bar on the %s)\n", i + 1, edges ? "its edges" : "tapped",
                barside::Name(it->second.barSide));
    std::fflush(stdout);
    UpdateBarMenu();
    if (g_menu.panel >= 0) PinShow(g_menu.overlay), ApplyAlpha(it->second), PaintBar(it->second);   // (its bar lit, and blue, while it's open)
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
    // 0.4 mm a pixel at 1 m: its 32 px text 13 mm, about the gamepad pill's; the card 29 cm wide
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    const double mpp = 0.0004 * d;
    g_menu.w = g_menu.layout.w * mpp, g_menu.h = g_menu.layout.h * mpp;
    const double half = barhit::Width(s.chrome, g_barPaddingX) / 2;
    const barside::UV at = barside::Place(s.barSide, s.metres, s.heightMetres(), BarIn(s), std::clamp(g_menu.along, -half, half));
    const barside::UV c = barside::MenuAt(s.barSide, at, barhit::Height(s.chrome, g_barPaddingY) / 2,
                                          std::max(s.grip, s.chrome * 0.06), g_menu.w, g_menu.h);
    g_menu.lift = posemath::CardLift(s.curve, g_menu.w / 2, 0.004);   // (clear of a curve)
    g_menu.pose = Mul(p, OnSurface(s, c.u, c.v, g_menu.lift));
    vr::VROverlay()->SetOverlayWidthInMeters(g_menu.overlay, float(g_menu.w));
    vr::VROverlay()->SetOverlayTransformAbsolute(g_menu.overlay, vr::TrackingUniverseStanding, &g_menu.pose);
    if (g_menu.holding == kNone) HandsOnMenu(s, p);
    if (g_menu.panel >= 0) DrawMenu(s, i);
}

// Its input: a laser over a row lights it, a press there does it (a held row until it's let go).
void PollBarMenu(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (PollOverlay(g_menu.overlay, &ev)) {
        // (FocusEnter's data isn't a point: the row lights on the move that follows it)
        if (ev.eventType == vr::VREvent_FocusEnter || ev.eventType == vr::VREvent_MouseMove) {
            if (ev.eventType == vr::VREvent_MouseMove && g_menu.holding == kNone)
                g_menu.hover = menulayout::At(g_menu.layout, ev.data.mouse.x, ev.data.mouse.y);
            g_menu.usedMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            if (g_menu.holding == kNone) g_menu.hover = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && g_menu.holding == kNone &&
                   (ev.data.mouse.button == vr::VRMouseButton_Left || ev.data.mouse.button == vr::VRMouseButton_Right)) {
            if (g_menu.panel < 0) continue;
            g_menu.usedMs = NowMs();
            const int row = menulayout::At(g_menu.layout, ev.data.mouse.x, ev.data.mouse.y);
            const bool moveAll = row >= 0 && row < int(g_menu.rows.size()) && g_menu.rows[size_t(row)].item == panelmenu::Item::MoveAll;
            if (moveAll) {
                if (ev.data.mouse.button != vr::VRMouseButton_Left) continue;   // (the release catcher's is the left one)
                // (a window's press by this laser still held: its release was lost; a claimed one, BarClaims,
                // never gets to the ReleaseAwayBy below, so KWin would keep the button, as with the bar's own)
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
            PressRow(row, ev.trackedDeviceIndex, DeviceLabel(ev.trackedDeviceIndex).c_str());
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
