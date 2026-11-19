// Quick Access over the palm (quickaccess.h's tiles, palmmenu.h's palm-up test): one overlay,
// frametop.quickaccess, over whichever palm shows it, facing the head. The other hand's index tip
// coming through a tile presses it (tiptap.h), and so does a click on it; a tile under either lights.
// While that tip is near the card, the hand is the card's (vr.cpp leaves it out of near input and tells
// the pointer helper it's ours), and the card stays though the palm turns as you reach (Input::engaged),
// as it does for a laser on it (not the mouse's or gaze's cursor, which sits on whatever you look at).
//   The palm hand's own pinch holds the card still where it was, lights Steam Menu, and moves what's lit
// as the pinch moves across it (quickaccess::Drag); its release does that tile (PinchRelease).
#include "vrpanel.h"
#include "label.h"
#include "overlaytex.h"
#include "palmmenu.h"
#include "quickaccess.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ftvr {
namespace {

using quickaccess::Tile;

constexpr const char *kKey = "frametop.quickaccess";
constexpr int64_t kLaserKeepMs = 500;   // a laser on it keeps it (engaged) this long after it last moved there
constexpr int kText = 28;

// How a tile looks: what Draw last drew of it, so a change repaints only it.
enum Style { kPlain, kHot, kAsk, kDim, kStyles };
struct Look {
    bool valid = false;
    Style style = kPlain;
    std::string text;
    bool operator==(const Look &o) const { return valid == o.valid && style == o.style && text == o.text; }
};

struct {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int side = -1;              // the palm it's over, -1 hidden
    double w = 0, h = 0;        // its size (m)
    Mat pose{};                 // ...and where it is
    quickaccess::Poke poke;     // the other hand's index tip at it
    int hover = -1;             // the tile under a laser or the cursor, -1 none
    int64_t laserMs = -1;       // a laser last moved on it
    int64_t shownMs = -1;
    quickaccess::Drag drag;     // the palm hand's pinch held: what it lights
    Mat dragPose{};             // ...and where the card held still for it
    uint32_t dragPinch = 0;     // ...and which pinch it is (palmmenu's pinchCount)
    bool lasers = false;        // MakeOverlaysInteractiveIfVisible is set
    float widthSet = -1;        // the width SetOverlayWidthInMeters has
    std::vector<uint8_t> px;                // the card as shown
    std::vector<uint8_t> styles[kStyles];   // a tile's box in each style, without its label
    Look looks[quickaccess::kTiles];
} g_card;
quickaccess::Countdown g_resetAsk;

void Invalidate() {
    for (Look &l : g_card.looks) l.valid = false;
}

std::string Label(const quickaccess::Spec &s) {
    if (s.tile == Tile::Screens) return ModeVisible() ? "Hide Screens" : "Show Screens";
    if (s.tile == Tile::Reset && g_resetAsk.On()) return "Reset in " + std::to_string(g_resetAsk.Left(NowMs()));
    return s.label;
}

// The card without its tiles into g_card.px, and each style's tile over it (council, 2026-11-19: a
// FillRound per tile cost ~1.8 ms on a little core; a copy, next to nothing).
void MakeImages() {
    using namespace quickaccess;
    g_card.px.assign(size_t(kW) * kH * 4, 0);
    const uint8_t bg[4] = {24, 24, 30, 230};
    FillRound(g_card.px, kW, kH, 0, 0, kW, kH, 24, bg);
    const uint8_t fill[kStyles][4] = {{52, 54, 62, 245}, {240, 240, 244, 245}, {196, 52, 48, 245}, {36, 37, 42, 245}};
    int x0, y0, x1, y1;
    Box(0, &x0, &y0, &x1, &y1);   // (every tile's box has the same card under it)
    for (int k = 0; k < kStyles; ++k) {
        std::vector<uint8_t> &t = g_card.styles[k];
        t.resize(size_t(kTile) * kTile * 4);
        for (int y = 0; y < kTile; ++y)
            std::memcpy(&t[size_t(y) * kTile * 4], &g_card.px[(size_t(y0 + y) * kW + x0) * 4], size_t(kTile) * 4);
        FillRound(t, kTile, kTile, 0, 0, kTile, kTile, 16, fill[k]);
    }
}

// One tile, onto g_card.px.
void DrawTile(int i, const Look &look) {
    using namespace quickaccess;
    int x0, y0, x1, y1;
    Box(i, &x0, &y0, &x1, &y1);
    const std::vector<uint8_t> &t = g_card.styles[look.style];
    for (int y = 0; y < kTile; ++y)
        std::memcpy(&g_card.px[(size_t(y0 + y) * kW + x0) * 4], &t[size_t(y) * kTile * 4], size_t(kTile) * 4);
    const uint8_t v = look.style == kHot ? 20 : look.style == kDim ? 120 : 245;
    const uint8_t ink[3] = {v, v, uint8_t(look.style == kHot ? 24 : look.style == kDim ? 128 : 250)};
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

// A tile that does nothing now: one that doesn't work while Frametop is paused for a VR game.
bool Idle(Tile t) { return g_paused && !quickaccess::WorksPaused(t); }

// SteamVR's dark tiles: the one under the fingertip (else the palm pinch's drag, else a laser's) white,
// Reset red while it counts down, and the idle ones dim. Only the tiles that changed are
// repainted (council, 2026-11-19: the whole card took ~25 ms on a little core).
void Draw() {
    using namespace quickaccess;
    if (g_card.px.empty()) MakeImages(), Invalidate();
    const int lit = g_card.poke.hover() >= 0 ? g_card.poke.hover() : g_card.drag.on() ? g_card.drag.tile() : g_card.hover;
    const bool armed = g_resetAsk.On();
    bool changed = false;
    Look now[kTiles];
    for (int i = 0; i < kTiles; ++i) {
        const Spec &s = kSpecs[i];
        Look &look = now[i];
        look.valid = true;
        look.style = Idle(s.tile)                                       ? kDim
                     : s.tile == Tile::Reset && armed                   ? kAsk
                     : i == lit                                         ? kHot
                                                                        : kPlain;
        look.text = Label(s);
        if (look == g_card.looks[i]) continue;
        DrawTile(i, look);
        changed = true;
    }
    if (!changed) return;
    // (kept only once it's on the overlay; if that failed, what it drew isn't what any look says, so
    // those tiles are drawn again whatever they look like next)
    const bool ok = overlaytex::Set(g_card.overlay, g_card.px.data(), kW, kH) == vr::VROverlayError_None;
    for (int i = 0; i < kTiles; ++i)
        if (!(now[i] == g_card.looks[i])) g_card.looks[i] = ok ? now[i] : Look{};
}

void Hide() {
    if (g_card.side < 0) return;
    std::printf("palm Quick Access: hidden\n");
    PinHide(g_card.overlay);
    g_card.side = -1, g_card.hover = -1, g_card.laserMs = -1;
    g_card.poke.Reset(NowMs()), g_card.drag.End();   // (Reset's countdown goes on: the palm put down after its tap)
}

// A tile pressed (`by`: whose).
void PressTile(int i, const char *by) {
    if (i < 0 || i >= quickaccess::kTiles) return;
    const quickaccess::Spec &s = quickaccess::kSpecs[i];
    const std::string label = Label(s);
    std::printf("palm Quick Access: %s (%s)\n", label.c_str(), by);
    std::fflush(stdout);
    if (Idle(s.tile)) return (void)std::printf("palm Quick Access: not while paused for a game\n");
    if (s.tile == Tile::Reset) {
        std::printf("palm Quick Access: Reset %s\n", g_resetAsk.Tap(NowMs()) ? "counts down" : g_resetAsk.On() ? "tapped too soon" : "stopped");
        return;
    }
    g_resetAsk.Clear();
    // (the input relay's own action, under its rules: this user, and while paused only PAUSED_ACTIONS)
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
    vr::VROverlay()->SetOverlayFlag(g_card.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);   // (a carry's wheel over it: PollOverlay)
    g_card.widthSet = -1, g_card.lasers = false;
    Invalidate();
    AnnounceOverlay(kKey);   // (the 3D mouse's ray finds it)
    return true;
}

// The palm hand's pinch, let go (palmmenu's toggled): `tile` its drag lit, Steam Menu for the pinch alone
// (as before the drag), -1 nothing (let go off the card, or the palm turned away).
void PinchRelease(int side, int tile) {
    const char *by = side ? "the right hand's pinch" : "the left hand's pinch";
    if (tile == int(Tile::SteamMenu)) {
        g_resetAsk.Clear();   // (as any other tile does)
        PalmSteamMenu(side);
    } else if (tile >= 0) {
        PressTile(tile, by);
    } else {
        std::printf("palm Quick Access: %s let go off the card: nothing\n", by);
    }
}

// The drag, on to this tick's pinch point and palm: the tile it lights.
int StepDrag(const palmmenu::PalmMenu &palm, int side) {
    double at[3];
    const int was = g_card.drag.tile();
    const int now = g_card.drag.Step(PinchPointIn(side, g_card.dragPose, at) ? at : nullptr, g_card.w, g_card.h, palm.up(side));
    if (now != was) std::printf("palm Quick Access: the pinch on %s\n", now >= 0 ? quickaccess::kSpecs[now].label : "nothing");
    return now;
}

}  // namespace

void UpdateQuickAccess(const QuickAccessIn &in) {
    const palmmenu::PalmMenu &palm = *in.palm;
    // the palm it's over while that shows; else whichever shows (both up: the right). (Paused for a VR
    // game, it shows with the dashboard open, as the palm menu does: Steam Menu works, the rest dim.)
    const int side = g_card.side >= 0 && palm.shown(g_card.side) ? g_card.side : palm.shown(1) ? 1 : palm.shown(0) ? 0 : -1;
    const int released = palm.toggled();   // (a pinch let go this Update, on that side)
    if (g_paused) g_resetAsk.Clear();   // (Reset does nothing then: its countdown ends, not waits out the pause)
    // (the card shown or not: the palm put down after a tap on Reset still resets)
    if (g_resetAsk.Due(NowMs())) DoReset(-1, "palm Quick Access, after its countdown");
    // dragged across, the card holds still where it was, the palm seen this frame or not (council,
    // 2026-11-19: one lost hands frame hid it and the drag began again on Steam Menu; palmmenu's lostMs
    // ends a pinch whose hand is gone, and no head ends it at once)
    const bool held = side >= 0 && side == g_card.side && g_card.drag.on();
    double c[3];
    const bool card = side >= 0 && in.haveHead && palm.Card(side, *in.shown, c);
    if ((!held && !card) || !Make()) {
        Hide();
        if (released >= 0) PinchRelease(released, int(Tile::SteamMenu));   // (no card: the pinch alone)
        return;
    }
    const bool fresh = side != g_card.side;
    if (fresh) {
        // (no Invalidate: hidden, the overlay kept its image, which g_card.looks still says)
        g_card.side = side, g_card.shownMs = NowMs(), g_card.hover = -1, g_card.laserMs = -1;
        g_card.poke.Reset(NowMs()), g_card.drag.End();   // (the other palm's drag ends; Reset's countdown goes on)
        std::printf("palm Quick Access: over the %s palm\n", side ? "right" : "left");
    }
    g_card.w = palm.params().size, g_card.h = g_card.w * quickaccess::kH / quickaccess::kW;
    // this Update's release: the tile under its last held frame, as drawn (council, 2026-11-19: not the
    // release frame's, whose contact moves as the fingers open; palmmenu let go only with the palm up)
    if (released >= 0) {
        const bool mine = released == side && g_card.drag.on();
        const int tile = mine ? g_card.drag.place() : int(Tile::SteamMenu);
        if (mine) g_card.drag.End();
        PinchRelease(released, tile);
    }
    // (the palm hand's pinch: the card holds still where it was while it's dragged across it; a new
    // pinch, let go and pinched again in one read whether it did anything or not, starts afresh)
    if (!palm.pinched(side)) g_card.drag.End();
    else if (!g_card.drag.on() || palm.pinchCount(side) != g_card.dragPinch)
        g_card.drag.Start(), g_card.dragPinch = palm.pinchCount(side), g_card.dragPose = card ? FacingPose(c, in.head) : g_card.pose;
    if (g_card.drag.on()) StepDrag(palm, side);   // (each tick: HandsFrame() is the last tick's news)
    g_card.pose = g_card.drag.on() ? g_card.dragPose : card ? FacingPose(c, in.head) : g_card.pose;
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
        const bool seen = !in.busy[other] && (g_card.drag.on() || palm.Card(side, *in.seen, cs)) &&
                          IndexTipIn(other, g_card.drag.on() ? g_card.dragPose : FacingPose(cs, in.head), l);
        const int pressed = g_card.poke.Step(seen ? l : nullptr, g_card.w, g_card.h, NowMs());
        if (pressed >= 0) PressTile(pressed, other ? "the right hand" : "the left hand");
    }
    Draw();
    if (fresh) PinShow(g_card.overlay);
}

bool QuickAccessEngaged(int side) {
    return g_card.side >= 0 && g_card.side == side && (g_card.poke.near() || (g_card.laserMs >= 0 && NowMs() - g_card.laserMs < kLaserKeepMs));
}

bool QuickAccessHasHand(int side) { return g_card.side >= 0 && side == 1 - g_card.side && g_card.poke.near(); }

void PollQuickAccess(void (*handle)(const struct ft_event *, void *), void *data) {
    vr::VREvent_t ev;
    while (PollOverlay(g_card.overlay, &ev)) {
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
            if (NowMs() - g_card.shownMs >= quickaccess::kShownMs)
                PressTile(quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y), DeviceLabel(ev.trackedDeviceIndex).c_str());
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
            EndDragsBy(ev.trackedDeviceIndex);
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
        }
    }
}

void DestroyQuickAccess() {
    if (g_card.overlay == vr::k_ulOverlayHandleInvalid) return;
    overlaytex::Forget(g_card.overlay);
    vr::VROverlay()->DestroyOverlay(g_card.overlay);
    g_card.overlay = vr::k_ulOverlayHandleInvalid, g_card.side = -1;
}

}  // namespace ftvr
