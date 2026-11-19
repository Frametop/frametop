// Quick Access over the palm (quickaccess.h's tiles, palmmenu.h's palm-up test): one overlay,
// frametop.quickaccess, over whichever palm shows it, facing the head. The other hand's index tip
// coming through a tile presses it (tiptap.h), and so does a laser's click; a tile under either lights.
// While that tip is near the card, the hand is the card's (vr.cpp leaves it out of near input and tells
// the pointer helper it's ours), and the card stays though the palm turns as you reach (Input::engaged).
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"
#include "palmmenu.h"
#include "quickaccess.h"
#include "tiptap.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ftvr {
namespace {

using quickaccess::Tile;

constexpr const char *kKey = "frametop.quickaccess";
constexpr int64_t kLeastMs = 300;       // a poke no sooner (the hand reaching over as it showed)
constexpr int64_t kLaserKeepMs = 500;   // a laser on it keeps it (engaged) this long after it last moved there
constexpr int kText = 28;

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int side = -1;              // the palm it's over, -1 hidden
    double w = 0, h = 0;        // its size (m)
    Mat pose{};                 // ...and where it is
    tiptap::Tap tip;            // the other hand's index tip through it
    bool tipNear = false;       // ...near it this hands frame
    int hover = -1;             // the tile under a laser, -1 none
    int handHover = -1;         // ...under that fingertip
    int64_t laserMs = -1;       // a laser last moved on it
    int64_t shownMs = -1;
    bool pinched = false;       // the palm hand's pinch held (its Steam Menu tile lights)
    std::string drawn;          // what its texture shows
    bool lasers = false;        // MakeOverlaysInteractiveIfVisible is set
} g_card;
quickaccess::Confirm g_resetAsk;

std::string Label(const quickaccess::Spec &s) {
    if (s.tile == Tile::Screens) return ModeVisible() ? "Hide Screens" : "Show Screens";
    if (s.tile == Tile::Reset && g_resetAsk.Armed(NowMs())) return "Tap to Reset";
    return s.label;
}

// SteamVR's dark tiles: the one under a laser or a fingertip white, Steam Menu white while the palm's
// pinch is held, Reset red while it asks.
void Draw() {
    using namespace quickaccess;
    const int lit = g_card.hover >= 0 ? g_card.hover : g_card.handHover;
    const bool armed = g_resetAsk.Armed(NowMs());
    std::string what = std::to_string(lit) + "/" + char('0' + g_card.pinched) + char('0' + armed) + char('0' + ModeVisible());
    if (what == g_card.drawn) return;
    std::vector<uint8_t> px(size_t(kW) * kH * 4, 0);
    const uint8_t bg[4] = {24, 24, 30, 230}, tile[4] = {52, 54, 62, 245}, on[4] = {240, 240, 244, 245}, ask[4] = {196, 52, 48, 245};
    FillRound(px, kW, kH, 0, 0, kW, kH, 24, bg);
    for (int i = 0; i < kTiles; ++i) {
        const Spec &s = kSpecs[i];
        int x0, y0, x1, y1;
        Box(i, &x0, &y0, &x1, &y1);
        const bool hot = i == lit || (s.tile == Tile::SteamMenu && g_card.pinched);
        const bool red = s.tile == Tile::Reset && armed;
        FillRound(px, kW, kH, x0, y0, x1, y1, 16, red ? ask : hot ? on : tile);
        const uint8_t ink[3] = {uint8_t(hot && !red ? 20 : 245), uint8_t(hot && !red ? 20 : 245), uint8_t(hot && !red ? 24 : 250)};
        // two lines when it's wide: "Reset" over "Layout…"
        const std::string text = Label(s);
        const size_t sp = text.find(' ');
        if (label::Width(text.c_str(), kText) <= kTile - 20 || sp == std::string::npos) {
            label::Draw(px.data(), kW, kH, text.c_str(), kText, (x0 + x1) / 2, (y0 + y1) / 2, 255, ink);
        } else {
            label::Draw(px.data(), kW, kH, text.substr(0, sp).c_str(), kText, (x0 + x1) / 2, (y0 + y1) / 2 - kText * 6 / 10, 255, ink);
            label::Draw(px.data(), kW, kH, text.substr(sp + 1).c_str(), kText, (x0 + x1) / 2, (y0 + y1) / 2 + kText * 6 / 10, 255, ink);
        }
    }
    overlaytex::Set(g_card.overlay, px.data(), kW, kH);
    g_card.drawn = what;
}

void Hide() {
    if (g_card.side < 0) return;
    std::printf("palm Quick Access: hidden\n");
    PinHide(g_card.overlay);
    g_card.side = -1, g_card.hover = g_card.handHover = -1, g_card.tipNear = false, g_card.laserMs = -1;
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
    AnnounceOverlay(kKey);   // (the 3D mouse's ray finds it)
    return true;
}

}  // namespace

void UpdateQuickAccess(const palmmenu::PalmMenu &palm, const std::vector<handcut::Hand> &hands, const Mat &head, bool haveHead) {
    // the palm it's over while that shows; else whichever shows (both up: the right)
    const int side = g_card.side >= 0 && palm.shown(g_card.side) ? g_card.side : palm.shown(1) ? 1 : palm.shown(0) ? 0 : -1;
    double c[3];
    if (side < 0 || !haveHead || !palm.Card(side, hands, c) || !Make()) return Hide();
    const bool fresh = side != g_card.side;
    if (fresh) {
        g_card.side = side, g_card.shownMs = NowMs(), g_card.hover = g_card.handHover = -1, g_card.laserMs = -1;
        g_card.tip.Reset(), g_card.drawn.clear(), g_resetAsk.Clear();
        std::printf("palm Quick Access: over the %s palm\n", side ? "right" : "left");
    }
    g_card.w = palm.params().size, g_card.h = g_card.w * quickaccess::kH / quickaccess::kW;
    g_card.pose = FacingPose(c, head);
    vr::VROverlay()->SetOverlayWidthInMeters(g_card.overlay, float(g_card.w));
    vr::VROverlay()->SetOverlayTransformAbsolute(g_card.overlay, vr::TrackingUniverseStanding, &g_card.pose);
    if (g_card.lasers != LasersByMode()) {
        g_card.lasers = !g_card.lasers;
        vr::VROverlay()->SetOverlayFlag(g_card.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, g_card.lasers);
    }
    if (HandsFrame()) {
        const int other = 1 - side;
        double l[3];
        const bool seen = IndexTipIn(other, g_card.pose, l);
        const bool through = g_card.tip.Step(seen ? l : nullptr, g_card.w / 2, g_card.h / 2);
        g_card.tipNear = seen && g_card.tip.Near();
        g_card.handHover = g_card.tipNear ? quickaccess::TileAtFace(l[0], l[1], g_card.w, g_card.h) : -1;
        if (through && NowMs() - g_card.shownMs >= kLeastMs)
            PressTile(quickaccess::TileAtFace(l[0], l[1], g_card.w, g_card.h), other ? "the right hand" : "the left hand");
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
        if (ev.eventType == vr::VREvent_FocusEnter || ev.eventType == vr::VREvent_MouseMove) {
            g_card.hover = quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y), g_card.laserMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_card.hover = -1, g_card.laserMs = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left && g_card.side >= 0) {
            g_card.laserMs = NowMs();
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
    g_card.overlay = vr::k_ulOverlayHandleInvalid, g_card.side = -1, g_card.drawn.clear();
}

}  // namespace ftvr
