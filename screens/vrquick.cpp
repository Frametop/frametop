// Quick Access over the palm (quickaccess.h's tiles, palmmenu.h's palm-up test): one overlay,
// frametop.quickaccess, over whichever palm shows it, facing the head. The other hand's index tip
// coming through a tile presses it (tiptap.h), and so does a click on it; a tile under either lights.
// While that tip is near the card, the hand is the card's (vr.cpp leaves it out of near input and tells
// the pointer helper it's ours), and the card stays though the palm turns as you reach (Input::engaged),
// as it does for a laser on it (not the mouse's or gaze's cursor, which sits on whatever you look at).
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"
#include "palmmenu.h"
#include "quickaccess.h"
#include "tiptap.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ftvr {
namespace {

using quickaccess::Tile;

constexpr const char *kKey = "frametop.quickaccess";
constexpr int64_t kLeastMs = 300;       // a poke no sooner (the hand reaching over as it showed)
constexpr int64_t kLaserKeepMs = 500;   // a laser on it keeps it (engaged) this long after it last moved there
constexpr int kText = 28;

// How a tile looks: what Draw last drew of it, so a change repaints only it.
struct Look {
    bool valid = false, hot = false, red = false;
    std::string text;
    bool operator==(const Look &o) const { return valid == o.valid && hot == o.hot && red == o.red && text == o.text; }
};

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int side = -1;              // the palm it's over, -1 hidden
    double w = 0, h = 0;        // its size (m)
    Mat pose{};                 // ...and where it is
    tiptap::Tap tip;            // the other hand's index tip through it
    quickaccess::Spent spent;   // ...pressed a tile, and hasn't come back out yet
    bool tipNear = false;       // ...near it this hands frame
    int hover = -1;             // the tile under a laser or the cursor, -1 none
    int handHover = -1;         // ...under that fingertip
    int64_t laserMs = -1;       // a laser last moved on it
    int64_t shownMs = -1;
    bool pinched = false;       // the palm hand's pinch held (its Steam Menu tile lights)
    bool lasers = false;        // MakeOverlaysInteractiveIfVisible is set
    float widthSet = -1;        // the width SetOverlayWidthInMeters has
    std::vector<uint8_t> base, px;   // the card without its tiles, and as shown
    Look looks[quickaccess::kTiles];
} g_card;
quickaccess::Confirm g_resetAsk;

void Invalidate() {
    for (Look &l : g_card.looks) l.valid = false;
}

std::string Label(const quickaccess::Spec &s) {
    if (s.tile == Tile::Screens) return ModeVisible() ? "Hide Screens" : "Show Screens";
    if (s.tile == Tile::Reset && g_resetAsk.Armed(NowMs())) return "Tap to Reset";
    return s.label;
}

// One tile, onto g_card.px over the base under it.
void DrawTile(int i, const Look &look) {
    using namespace quickaccess;
    int x0, y0, x1, y1;
    Box(i, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; ++y) {
        const size_t at = (size_t(y) * kW + x0) * 4;
        std::memcpy(&g_card.px[at], &g_card.base[at], size_t(x1 - x0) * 4);
    }
    const uint8_t tile[4] = {52, 54, 62, 245}, on[4] = {240, 240, 244, 245}, ask[4] = {196, 52, 48, 245};
    FillRound(g_card.px, kW, kH, x0, y0, x1, y1, 16, look.red ? ask : look.hot ? on : tile);
    const bool dark = look.hot && !look.red;
    const uint8_t ink[3] = {uint8_t(dark ? 20 : 245), uint8_t(dark ? 20 : 245), uint8_t(dark ? 24 : 250)};
    // two lines when it's wide: "Reset" over "Layout…"
    const std::string &text = look.text;
    const size_t sp = text.find(' ');
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (label::Width(text.c_str(), kText) <= kTile - 20 || sp == std::string::npos) {
        label::Draw(g_card.px.data(), kW, kH, text.c_str(), kText, cx, cy, 255, ink);
    } else {
        label::Draw(g_card.px.data(), kW, kH, text.substr(0, sp).c_str(), kText, cx, cy - kText * 6 / 10, 255, ink);
        label::Draw(g_card.px.data(), kW, kH, text.substr(sp + 1).c_str(), kText, cx, cy + kText * 6 / 10, 255, ink);
    }
}

// SteamVR's dark tiles: the one under the fingertip (else a laser's) white, Steam Menu white while the
// palm's pinch is held, Reset red while it asks. Only the tiles that changed are repainted (council,
// 2026-11-19: the whole card took ~25 ms on a little core).
void Draw() {
    using namespace quickaccess;
    if (g_card.base.empty()) {
        g_card.base.assign(size_t(kW) * kH * 4, 0);
        const uint8_t bg[4] = {24, 24, 30, 230};
        FillRound(g_card.base, kW, kH, 0, 0, kW, kH, 24, bg);
        g_card.px = g_card.base;
        Invalidate();
    }
    const int lit = g_card.handHover >= 0 ? g_card.handHover : g_card.hover;
    const bool armed = g_resetAsk.Armed(NowMs());
    bool changed = false;
    for (int i = 0; i < kTiles; ++i) {
        const Spec &s = kSpecs[i];
        Look look;
        look.valid = true;
        look.hot = i == lit || (s.tile == Tile::SteamMenu && g_card.pinched);
        look.red = s.tile == Tile::Reset && armed;
        look.text = Label(s);
        if (look == g_card.looks[i]) continue;
        DrawTile(i, look);
        g_card.looks[i] = look, changed = true;
    }
    if (changed) overlaytex::Set(g_card.overlay, g_card.px.data(), kW, kH);
}

void Hide() {
    if (g_card.side < 0) return;
    std::printf("palm Quick Access: hidden\n");
    PinHide(g_card.overlay);
    g_card.side = -1, g_card.hover = g_card.handHover = -1, g_card.tipNear = false, g_card.laserMs = -1;
    g_card.spent.Clear();
    g_resetAsk.Clear();
}

// A tile pressed (`by`: whose).
void PressTile(int i, const char *by) {
    if (i < 0 || i >= quickaccess::kTiles) return;
    const quickaccess::Spec &s = quickaccess::kSpecs[i];
    const std::string label = Label(s);
    std::printf("palm Quick Access: %s (%s)\n", label.c_str(), by);
    std::fflush(stdout);
    if (s.tile == Tile::Reset) {
        if (g_resetAsk.Tap(NowMs())) DoReset(-1, (std::string("palm Quick Access, ") + by + ", confirmed").c_str());
        return;
    }
    g_resetAsk.Clear();
    // (the input relay's own action, under its rules: this user, not while paused)
    SendRelay(std::string("action ") + s.action);
}

bool Make() {
    if (g_card.overlay != vr::k_ulOverlayHandleInvalid) return true;
    if (vr::VROverlay()->CreateOverlay(kKey, "Frametop: Quick Access", &g_card.overlay) != vr::VROverlayError_None) {
        g_card.overlay = vr::k_ulOverlayHandleInvalid;
        return false;
    }
    vr::VROverlay()->SetOverlayInputMethod(g_card.overlay, vr::VROverlayInputMethod_Mouse);
    const vr::HmdVector2_t scale = {{float(quickaccess::kW), float(quickaccess::kH)}};
    vr::VROverlay()->SetOverlayMouseScale(g_card.overlay, &scale);
    vr::VROverlay()->SetOverlaySortOrder(g_card.overlay, 22);
    g_card.widthSet = -1, g_card.lasers = false;
    Invalidate();
    AnnounceOverlay(kKey);   // (the 3D mouse's ray finds it)
    return true;
}

}  // namespace

void UpdateQuickAccess(const QuickAccessIn &in) {
    const palmmenu::PalmMenu &palm = *in.palm;
    // the palm it's over while that shows; else whichever shows (both up: the right). Not while
    // Frametop is paused for a game: its tiles are the relay's, which does nothing then.
    const int side = g_paused ? -1 : g_card.side >= 0 && palm.shown(g_card.side) ? g_card.side : palm.shown(1) ? 1 : palm.shown(0) ? 0 : -1;
    double c[3];
    if (side < 0 || !in.haveHead || !palm.Card(side, *in.shown, c) || !Make()) return Hide();
    const bool fresh = side != g_card.side;
    if (fresh) {
        g_card.side = side, g_card.shownMs = NowMs(), g_card.hover = g_card.handHover = -1, g_card.laserMs = -1;
        g_card.tip.Reset(), g_card.spent.Clear(), g_resetAsk.Clear();
        Invalidate();
        std::printf("palm Quick Access: over the %s palm\n", side ? "right" : "left");
    }
    g_card.w = palm.params().size, g_card.h = g_card.w * quickaccess::kH / quickaccess::kW;
    g_card.pose = FacingPose(c, in.head);
    if (float(g_card.w) != g_card.widthSet) {
        g_card.widthSet = float(g_card.w);
        vr::VROverlay()->SetOverlayWidthInMeters(g_card.overlay, g_card.widthSet);
    }
    vr::VROverlay()->SetOverlayTransformAbsolute(g_card.overlay, vr::TrackingUniverseStanding, &g_card.pose);
    if (g_card.lasers != LasersByMode()) {
        g_card.lasers = !g_card.lasers;
        vr::VROverlay()->SetOverlayFlag(g_card.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, g_card.lasers);
    }
    if (HandsFrame()) {
        const int other = 1 - side;
        // the tip against the card where the hands were seen (the card shows where they're predicted to
        // be, ahead of them), by a hand that isn't holding or pressing something
        double cs[3], l[3];
        const bool seen = !in.busy[other] && palm.Card(side, *in.seen, cs) && IndexTipIn(other, FacingPose(cs, in.head), l);
        const bool through = g_card.tip.Step(seen ? l : nullptr, g_card.w / 2, g_card.h / 2);
        g_card.spent.Step(seen ? l : nullptr, g_card.w / 2, g_card.h / 2);
        g_card.tipNear = seen && g_card.tip.Near();
        if (g_card.tipNear) {
            double x, y;
            quickaccess::FaceToPx(l[0], l[1], g_card.w, g_card.h, &x, &y);
            g_card.handHover = quickaccess::StickyTileAt(g_card.handHover, x, y);
        } else {
            g_card.handHover = -1;
        }
        if (through && !g_card.spent.on() && NowMs() - g_card.shownMs >= kLeastMs && g_card.handHover >= 0) {
            g_card.spent.Pressed();
            PressTile(g_card.handHover, other ? "the right hand" : "the left hand");
        }
    }
    g_card.pinched = palm.pinched(side);
    Draw();
    if (fresh) PinShow(g_card.overlay);
}

bool QuickAccessEngaged(int side) {
    return g_card.side >= 0 && g_card.side == side && (g_card.tipNear || (g_card.laserMs >= 0 && NowMs() - g_card.laserMs < kLaserKeepMs));
}

bool QuickAccessHasHand(int side) { return g_card.side >= 0 && side == 1 - g_card.side && g_card.tipNear; }

void PollQuickAccess(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (g_card.overlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->PollNextOverlayEvent(g_card.overlay, &ev, sizeof ev)) {
        // (only a laser keeps the card: the mouse's or gaze's cursor would hold it wherever you look)
        const bool laser = FromLaser(ev);
        if (ev.eventType == vr::VREvent_MouseMove) {
            g_card.hover = quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y);
            if (laser) g_card.laserMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusEnter) {   // (its data is an overlay's, not a point)
            if (laser) g_card.laserMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_card.hover = -1;
            if (laser) g_card.laserMs = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left && g_card.side >= 0) {
            if (laser) g_card.laserMs = NowMs();
            if (NowMs() - g_card.shownMs >= kLeastMs)
                PressTile(quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y), DeviceLabel(ev.trackedDeviceIndex).c_str());
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
}

void DestroyQuickAccess() {
    if (g_card.overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::VROverlay()->DestroyOverlay(g_card.overlay);
    g_card.overlay = vr::k_ulOverlayHandleInvalid, g_card.side = -1;
}

}  // namespace ftvr
