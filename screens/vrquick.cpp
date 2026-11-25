// Quick Access over the palm (quickaccess.h's tiles, palmmenu.h's palm-up test): one overlay,
// frametop.quickaccess, over whichever palm shows it, facing the head. The other hand's index tip
// coming through a tile presses it (tiptap.h), and so does a click on it; a tile under either lights.
// While that tip is near the card, the hand is the card's (vr.cpp leaves it out of near input and tells
// the pointer helper it's ours), and the card stays though the palm turns as you reach (Input::engaged),
// as it does for a laser on it (not the mouse's or gaze's cursor, which sits on whatever you look at).
//   What the tiles say and run is the user's (palmtiles.h), read from ~/.config/frametop-palm.txt each
// time the card shows. A tile is the input relay's own action (its "palm ACTION"), under its rules
// (this user; while Frametop is paused for a VR game only some, and the rest dim); Reset Layout is ours
// (DoReset: at once, as ft-layout's reset counts 3-2-1 in front of you itself).
//   The palm hand's own pinch holds the card still where it was, lights Steam Menu (palmtiles::PinchSlot),
// and moves what's lit as the pinch moves across it (quickaccess::Drag); its release does that tile
// (PinchRelease). With no card to see, the pinch is Steam Menu.
//   Its header shows CPU, GPU, memory, temperature and the battery (vrstats.cpp) and the Quick Settings button, which
// turns the card to that page's tiles with Back in the button's place; hidden, it's on its first page again.
//   With gaze mode on (vrgazemode.cpp, read as it shows), a wide row sits under the tiles on both pages:
// Quick Eye Calibrate at first (the relay's gaze_quickcal).
#include "vrpanel.h"
#include "batteryart.h"
#include "label.h"
#include "labelfit.h"
#include "overlaytex.h"
#include "palmmenu.h"
#include "palmtiles.h"
#include "quickaccess.h"
#include "../conf/ft_conf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ftvr {
namespace {

constexpr const char *kKey = "frametop.quickaccess";
constexpr int64_t kLaserKeepMs = 500;   // a laser on it keeps it (engaged) this long after it last moved there
constexpr int kText = 28, kStatsText = 24;
constexpr uint8_t kCardBg[4] = {24, 24, 30, 230};
using quickaccess::kButton;
using quickaccess::kEyeCal;
using quickaccess::kSlots;
static_assert(palmtiles::kMax == quickaccess::kTiles, "a page's tiles fill the card's grid");

// How a tile looks: what Draw last drew of it, so a change repaints only it. kOn: a toggle that's on;
// kBlank: no tile there (a page with fewer than six).
enum Style { kPlain, kHot, kDim, kOn, kBlank, kStyles };
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
    bool settings = false;      // on the Quick Settings page
    palmtiles::Menu menu;       // what each page's tiles say and run, as read when it showed
    bool eyes = false;          // the wide row shows (gaze mode was on as it showed)
    std::vector<uint8_t> px;                // the card as shown
    std::vector<uint8_t> bg;                // ...without its tiles or header
    std::vector<uint8_t> styles[kStyles];   // a tile's box in each style, without its label
    std::vector<uint8_t> button[kStyles];   // ...and the header button's
    std::vector<uint8_t> eyeRow[kStyles];   // ...and the eye row's
    Look looks[kSlots];
    std::string head;                       // what the header's left last showed (empty: drawn again)
} g_card;

void Invalidate() {
    for (Look &l : g_card.looks) l.valid = false;
    g_card.head.clear();
}

// The tiles of the page it's on.
const std::vector<palmtiles::Entry> &Page() { return g_card.settings ? g_card.menu.quick : g_card.menu.first; }

// The tile in slot i, null for none (a blank slot, the header's button, no eye row).
const palmtiles::Entry *At(int i) {
    if (i == kEyeCal) return g_card.eyes ? &g_card.menu.eyes : nullptr;
    return i >= 0 && i < int(Page().size()) ? &Page()[i] : nullptr;
}

// The palm menu's file now (palmtiles.h); with none, the default card.
palmtiles::Menu ReadMenu() {
    const char *home = std::getenv("HOME");
    const std::string path = std::string(home ? home : "/tmp") + "/.config/frametop-palm.txt";
    if (std::FILE *f = std::fopen(path.c_str(), "r")) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1 << 18)) text.append(buf, n);
        std::fclose(f);
        return palmtiles::Parse(text);
    }
    char value[512];
    return palmtiles::Defaults(ft_conf_setting("QUICK_SETTINGS", value, sizeof value) ? value : nullptr);
}

int H() { return quickaccess::Height(g_card.eyes); }   // the card's height in pixels now

std::string Label(int i) {
    if (i == kButton) return g_card.settings ? "Back" : "Quick Settings";
    const palmtiles::Entry *e = At(i);
    return e ? palmtiles::Words(*e, ModeVisible()) : "";
}

// The card's picture under box (x0, y0) w x h, with each style's box over it, into out[].
void MakeBoxes(std::vector<uint8_t> (&out)[kStyles], int x0, int y0, int w, int h) {
    const uint8_t fill[kStyles][4] = {
        {52, 54, 62, 245}, {240, 240, 244, 245}, {36, 37, 42, 245}, {38, 104, 196, 245}, {0, 0, 0, 0}};
    for (int k = 0; k < kStyles; ++k) {
        std::vector<uint8_t> &t = out[k];
        t.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(&t[size_t(y) * w * 4], &g_card.bg[(size_t(y0 + y) * quickaccess::kW + x0) * 4], size_t(w) * 4);
        if (k != kBlank) FillRound(t, w, h, 0, 0, w, h, 16, fill[k]);
    }
}

// The card without its tiles into g_card.px, and each style's tile over it (council, 2026-11-19: a
// FillRound per tile cost ~1.8 ms on a little core; a copy, next to nothing).
void MakeImages() {
    using namespace quickaccess;
    g_card.bg.assign(size_t(kW) * H() * 4, 0);
    FillRound(g_card.bg, kW, H(), 0, 0, kW, H(), 24, kCardBg);
    g_card.px = g_card.bg;
    int x0, y0, x1, y1;
    Box(0, &x0, &y0, &x1, &y1);   // (every tile's box has the same card under it)
    MakeBoxes(g_card.styles, x0, y0, kTile, kTile);
    Box(kButton, &x0, &y0, &x1, &y1);
    MakeBoxes(g_card.button, x0, y0, x1 - x0, y1 - y0);
    if (g_card.eyes) Box(kEyeCal, &x0, &y0, &x1, &y1), MakeBoxes(g_card.eyeRow, x0, y0, x1 - x0, y1 - y0);
}

// One tile (or the header's button), onto g_card.px.
void DrawTile(int i, const Look &look) {
    using namespace quickaccess;
    int x0, y0, x1, y1;
    Box(i, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const std::vector<uint8_t> &t = (i == kButton ? g_card.button : i == kEyeCal ? g_card.eyeRow : g_card.styles)[look.style];
    // a box past the card, or a picture of the wrong size, would copy outside g_card.px or t
    if (x0 < 0 || y0 < 0 || x1 > kW || y1 > H() || t.size() != size_t(w) * (y1 - y0) * 4) return;
    for (int y = 0; y < y1 - y0; ++y)
        std::memcpy(&g_card.px[(size_t(y0 + y) * kW + x0) * 4], &t[size_t(y) * w * 4], size_t(w) * 4);
    const uint8_t v = look.style == kHot ? 20 : look.style == kDim ? 120 : 245;
    const uint8_t ink[3] = {v, v, uint8_t(look.style == kHot ? 24 : look.style == kDim ? 128 : 250)};
    // as many lines as it takes, "Reset" over "Layout…", smaller when the words are long
    const labelfit::Fit fit = labelfit::Make(look.text, {kText, 24, 20}, w - 20, i == kEyeCal ? 2 : 3,
                                             [](const std::string &t, int size) { return label::Width(t.c_str(), size); });
    const int n = int(fit.lines.size()), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, step = fit.size * 12 / 10;
    for (int k = 0; k < n; ++k)
        label::Draw(g_card.px.data(), kW, H(), fit.lines[k].c_str(), fit.size, cx, cy + (2 * k - (n - 1)) * step / 2, 255, ink);
}

// A tile that does nothing now: one that doesn't work while Frametop is paused for a VR game (the
// header's button always does: Game Pause is on the page it opens).
bool Idle(int i) {
    const palmtiles::Entry *e = g_paused ? At(i) : nullptr;
    return e && !palmtiles::WorksPaused(e->action);
}

// The battery's line in the header's left, its picture and words together centred on (cx, cy).
void DrawBattery(const battery::Reading &r, int cx, int cy) {
    using namespace quickaccess;
    const uint8_t ink[3] = {225, 225, 232};
    const std::string words = battery::Say(r);
    if (words.empty()) return (void)label::Draw(g_card.px.data(), kW, H(), "Battery \xe2\x80\x93", kStatsText, cx, cy, 255, ink);
    const int gap = 10, text = label::Width(words.c_str(), kStatsText), x0 = cx - (batteryart::kW + gap + text) / 2;
    batteryart::Draw(g_card.px, kW, H(), x0, cy, r, kCardBg);
    label::Draw(g_card.px.data(), kW, H(), words.c_str(), kStatsText, x0 + batteryart::kW + gap + text / 2, cy, 255, ink);
}

// The header's left: the readouts, three lines (on the Quick Settings page, its title), into g_card.px
// when they changed. Whether it drew.
bool DrawHead() {
    using namespace quickaccess;
    std::string a, b;
    battery::Reading bat;
    if (g_card.settings) {
        a = "Quick Settings";
        b = Page().empty() ? "Pick them in Frametop Settings" : "";
    } else {
        const sysstats::Words &w = SystemStats(NowMs());
        auto or_dash = [](const std::string &s, const char *name) { return s.empty() ? std::string(name) + " \xe2\x80\x93" : s; };
        a = or_dash(w.cpu, "CPU") + "    " + or_dash(w.gpu, "GPU");
        b = or_dash(w.mem, "Mem") + "    " + (w.temp.empty() ? std::string("\xe2\x80\x93 \xc2\xb0" "C") : w.temp);
        bat = BatteryNow(NowMs());
    }
    const std::string text = a + "\n" + b + (g_card.settings ? "" : "\n" + battery::Say(bat));
    if (text == g_card.head) return false;
    int x0, y0, x1, y1;
    StatsBox(&x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; ++y)
        std::memcpy(&g_card.px[(size_t(y) * kW + x0) * 4], &g_card.bg[(size_t(y) * kW + x0) * 4], size_t(x1 - x0) * 4);
    const uint8_t ink[3] = {225, 225, 232};
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, size = g_card.settings && b.empty() ? kText : kStatsText;
    if (b.empty()) {
        label::Draw(g_card.px.data(), kW, H(), a.c_str(), size, cx, cy, 255, ink);
    } else if (!g_card.settings) {
        const int line = size * 14 / 10;
        label::Draw(g_card.px.data(), kW, H(), a.c_str(), size, cx, cy - line, 255, ink);
        label::Draw(g_card.px.data(), kW, H(), b.c_str(), size, cx, cy, 255, ink);
        DrawBattery(bat, cx, cy + line);
    } else {
        label::Draw(g_card.px.data(), kW, H(), a.c_str(), size, cx, cy - size * 7 / 10, 255, ink);
        label::Draw(g_card.px.data(), kW, H(), b.c_str(), 20, cx, cy + size * 7 / 10, 255, ink);
    }
    g_card.head = text;
    return true;
}

// SteamVR's dark tiles: the one under the fingertip (else the palm pinch's drag, else a laser's) white,
// a toggle that's on blue (palmtiles::Lit), and the idle ones dim. Only the tiles that changed are
// repainted (council, 2026-11-19: the whole card took ~25 ms on a little core).
void Draw() {
    using namespace quickaccess;
    if (g_card.px.empty()) MakeImages(), Invalidate();
    const int lit = g_card.poke.hover() >= 0 ? g_card.poke.hover() : g_card.drag.on() ? g_card.drag.tile() : g_card.hover;
    bool changed = DrawHead();
    Look now[kSlots];
    for (int i = 0; i < kSlots; ++i) {
        // (no eye row with gaze mode off: the card ends above where it would be, and its pictures weren't made)
        if (i == kEyeCal && !g_card.eyes) continue;
        Look &look = now[i];
        look.valid = true;
        look.text = Label(i);
        const palmtiles::Entry *e = At(i);
        look.style = !e && i != kButton ? kBlank
                     : Idle(i)          ? kDim
                     : i == lit         ? kHot
                     : e && palmtiles::Lit(*e, ModeVisible(), g_paused) ? kOn
                                                                        : kPlain;
        if (look == g_card.looks[i]) continue;
        DrawTile(i, look);
        changed = true;
    }
    if (!changed) return;
    // (kept only once it's on the overlay; if that failed, what it drew isn't what any look says, so
    // those tiles are drawn again whatever they look like next)
    const bool ok = overlaytex::Set(g_card.overlay, g_card.px.data(), kW, H()) == vr::VROverlayError_None;
    for (int i = 0; i < kSlots; ++i)
        if (!(now[i] == g_card.looks[i])) g_card.looks[i] = ok ? now[i] : Look{};
    if (!ok) g_card.head.clear();
}

// To the Quick Settings page, or back.
void TurnPage(bool settings) {
    g_card.settings = settings;
    std::printf("palm Quick Access: %s\n", settings ? "the Quick Settings page" : "the first page");
}

void Hide() {
    if (g_card.side < 0) return;
    std::printf("palm Quick Access: hidden\n");
    PinHide(g_card.overlay);
    g_card.side = -1, g_card.hover = -1, g_card.laserMs = -1;
    g_card.poke.Reset(NowMs()), g_card.drag.End();
    if (g_card.settings) TurnPage(false);
    ForgetStats();
}

// A tile pressed (`by`: whose).
void PressTile(int i, const char *by) {
    if (i < 0 || i >= kSlots) return;
    const std::string label = Label(i);
    if (label.empty()) return;   // (a blank slot)
    std::printf("palm Quick Access: %s (%s)\n", label.c_str(), by);
    std::fflush(stdout);
    if (Idle(i)) return (void)std::printf("palm Quick Access: not while paused for a game\n");
    if (i == kButton) return TurnPage(!g_card.settings);
    const palmtiles::Entry *e = At(i);
    if (!e) return;
    if (e->action == palmtiles::kReset) return DoReset(-1, (std::string("palm Quick Access, ") + by).c_str());
    SendRelay("palm " + e->action);
}

bool Make() {
    if (g_card.overlay != vr::k_ulOverlayHandleInvalid) return true;
    if (vr::VROverlay()->CreateOverlay(kKey, "Frametop: Quick Access", &g_card.overlay) != vr::VROverlayError_None) {
        g_card.overlay = vr::k_ulOverlayHandleInvalid;
        return false;
    }
    vr::VROverlay()->SetOverlayInputMethod(g_card.overlay, vr::VROverlayInputMethod_Mouse);
    const vr::HmdVector2_t scale = {{float(quickaccess::kW), float(H())}};
    vr::VROverlay()->SetOverlayMouseScale(g_card.overlay, &scale);
    vr::VROverlay()->SetOverlaySortOrder(g_card.overlay, 22);
    vr::VROverlay()->SetOverlayFlag(g_card.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);   // (a carry's wheel over it: PollOverlay)
    g_card.widthSet = -1, g_card.lasers = false;
    Invalidate();
    AnnounceOverlay(kKey);   // (the 3D mouse's ray finds it)
    return true;
}

// The palm hand's pinch, let go (palmmenu's toggled): `tile` its drag lit, kAlone for the pinch with no
// card to see (Steam Menu, as before there was one), -1 nothing (let go off the card, or the palm turned away).
constexpr int kAlone = -2;
void PinchRelease(int side, int tile) {
    const char *by = side ? "the right hand's pinch" : "the left hand's pinch";
    const palmtiles::Entry *e = At(tile);
    if (tile == kAlone || (e && e->action == palmtiles::kSteamMenu)) {
        PalmSteamMenu(side);
    } else if (tile >= 0) {
        PressTile(tile, by);
    } else {
        std::printf("palm Quick Access: %s let go off the card: nothing\n", by);
    }
}

// The slot the palm hand's pinch starts on.
int PinchStart() { return g_card.settings ? kButton : palmtiles::PinchSlot(g_card.menu); }

// The drag, on to this tick's pinch point and palm: the tile it lights.
int StepDrag(const palmmenu::PalmMenu &palm, int side) {
    double at[3];
    const int was = g_card.drag.tile();
    const int now = g_card.drag.Step(PinchPointIn(side, g_card.dragPose, at) ? at : nullptr, g_card.w, g_card.h, palm.up(side));
    if (now != was) std::printf("palm Quick Access: the pinch on %s\n", now >= 0 ? Label(now).c_str() : "nothing");
    return now;
}

}  // namespace

void UpdateQuickAccess(const QuickAccessIn &in) {
    const palmmenu::PalmMenu &palm = *in.palm;
    const bool eyes = GazeModeOn();   // (asked every tick, hidden or not, so it's current as the card shows)
    // the palm it's over while that shows; else whichever shows (both up: the right). (Paused for a VR
    // game, it shows with the dashboard open, as the palm menu does: Steam Menu works, the rest dim.)
    const int side = g_card.side >= 0 && palm.shown(g_card.side) ? g_card.side : palm.shown(1) ? 1 : palm.shown(0) ? 0 : -1;
    const int released = palm.toggled();   // (a pinch let go this Update, on that side)
    // dragged across, the card holds still where it was, the palm seen this frame or not (council,
    // 2026-11-19: one lost hands frame hid it and the drag began again on Steam Menu; palmmenu's lostMs
    // ends a pinch whose hand is gone, and no head ends it at once)
    const bool held = side >= 0 && side == g_card.side && g_card.drag.on();
    double c[3];
    const bool card = side >= 0 && in.haveHead && palm.Card(side, *in.shown, c);
    if ((!held && !card) || !Make()) {
        Hide();
        if (released >= 0) PinchRelease(released, kAlone);   // (no card: the pinch alone)
        return;
    }
    const bool fresh = side != g_card.side;
    if (fresh) {
        // (no Invalidate: hidden, the overlay kept its image, which g_card.looks still says)
        g_card.side = side, g_card.shownMs = NowMs(), g_card.hover = -1, g_card.laserMs = -1;
        g_card.poke.Reset(NowMs()), g_card.drag.End();   // (the other palm's drag ends)
        if (g_card.settings) TurnPage(false);
        g_card.menu = ReadMenu();   // (Frametop Settings may have changed it)
        const bool row = eyes && g_card.menu.eyesShown;
        if (row != g_card.eyes) {
            // a card of the other height: its pictures made again, and the mouse's scale with it
            g_card.eyes = row, g_card.px.clear();
            const vr::HmdVector2_t scale = {{float(quickaccess::kW), float(H())}};
            vr::VROverlay()->SetOverlayMouseScale(g_card.overlay, &scale);
        }
        std::printf("palm Quick Access: over the %s palm%s\n", side ? "right" : "left", g_card.eyes ? ", with the eye row" : "");
    }
    g_card.w = palm.params().size, g_card.h = g_card.w * H() / quickaccess::kW;
    // this Update's release: the tile under its last held frame, as drawn (council, 2026-11-19: not the
    // release frame's, whose contact moves as the fingers open; palmmenu let go only with the palm up)
    if (released >= 0) {
        const int tile = released != side || fresh ? kAlone :   // (a card only now back wasn't seen)
                         g_card.drag.on() ? g_card.drag.place() : PinchStart();
        if (released == side) g_card.drag.End();
        PinchRelease(released, tile);
    }
    // (the palm hand's pinch: the card holds still where it was while it's dragged across it; a new
    // pinch, let go and pinched again in one read whether it did anything or not, starts afresh)
    if (!palm.pinched(side)) g_card.drag.End();
    else if (!g_card.drag.on() || palm.pinchCount(side) != g_card.dragPinch)
        g_card.drag.Start(PinchStart()), g_card.dragPinch = palm.pinchCount(side), g_card.dragPose = card ? FacingPose(c, in.head) : g_card.pose;
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
            g_card.hover = quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y, H());
            if (laser) g_card.laserMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusEnter) {   // (its data is an overlay's, not a point)
            if (laser) g_card.laserMs = NowMs();
        } else if (ev.eventType == vr::VREvent_FocusLeave) {
            g_card.hover = -1;
            if (laser) g_card.laserMs = -1;
        } else if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left && g_card.side >= 0) {
            if (laser) g_card.laserMs = NowMs();
            if (NowMs() - g_card.shownMs >= quickaccess::kShownMs)
                PressTile(quickaccess::TileAtMouse(ev.data.mouse.x, ev.data.mouse.y, H()), DeviceLabel(ev.trackedDeviceIndex).c_str());
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
