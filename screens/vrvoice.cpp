// Dictation's notice (user, 2026-11-21: "implement the dictation listening notice", the UI design's "Dictation is
// listening"): while Frametop Voice listens, a pill on the panel typing goes to, low and centred on it, so you see
// where the words will land; "Typing…" while the last words are decoded and typed. Typing that goes to Steam or a
// game (no panel of ours has it), or a panel that's hidden: the pill sits low in your view instead. Sounds stay
// the second cue.
//   ft-voiced says "voice listen hold|toggle|command LEVEL" (about 8 times a second while it listens: LEVEL is
// the mic's, 0..1), "voice typing" and "voice off" on @ft_screens (compositor.c passes them to ft_vr_voice).
// A word that stops coming hides it: listening after 1.5 s, typing after 8 s (ft-voiced gone, or a stop lost).
// Its look is voicenoticeart.h's. It takes no input: lasers pass through.
#include "vr.h"
#include "vrpanel.h"
#include "overlaytex.h"
#include "voicenoticeart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ftvr {
namespace {

using voicenoticeart::Kind;

struct VoiceNotice {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    bool on = false;          // listening or typing
    Kind kind = Kind::Listening;
    std::string hint;         // "release to type", ...
    double level = 0;
    int64_t saidMs = 0;       // ft-voiced's last word
    bool shown = false;
    int drawnLevel = -1, drawnTurn = -1;
    Kind drawnKind = Kind::Typing;
    std::string drawnHint = "-";
    int px = 0, pxH = 0;      // its texture's size
} g_voice;
constexpr const char *kVoiceKey = "frametop.voice";
constexpr int64_t kListenQuietMs = 1500, kTypingMs = 8000;
constexpr double kMpp = 0.0004;   // metres a pixel at 1 m: its 28 px text about 0.65 deg, as SteamVR's pills

void Hide() {
    if (g_voice.shown) PinHide(g_voice.overlay);
    g_voice.shown = false;
}

void Draw() {
    // (level in 8 steps, the ring's turn in 12: no new texture for changes nobody sees)
    const int level = int(std::lround(g_voice.level * 8)), turn = int(NowMs() / 83 % 12);
    const bool typing = g_voice.kind == Kind::Typing;
    if (g_voice.drawnKind == g_voice.kind && g_voice.drawnHint == g_voice.hint &&
        (typing ? g_voice.drawnTurn == turn : g_voice.drawnLevel == level))
        return;
    voicenoticeart::Look look;
    look.kind = g_voice.kind, look.hint = g_voice.hint, look.level = level / 8.0, look.turn = turn / 12.0;
    const voicenoticeart::Art a = voicenoticeart::Draw(look);
    overlaytex::Set(g_voice.overlay, a.px.data(), a.w, a.h);
    g_voice.px = a.w, g_voice.pxH = a.h;
    g_voice.drawnKind = g_voice.kind, g_voice.drawnHint = g_voice.hint, g_voice.drawnLevel = level, g_voice.drawnTurn = turn;
}

// On the panel typing goes to: centred, its bottom edge 12 design px (24 of ours) above the panel's. Else low in
// view, 1 m out and 0.3 m down, tilted to face you. True if placed.
bool Place() {
    const auto it = g_screens.find(TypingPanel());
    Mat p, head;
    if (it != g_screens.end() && it->second.visible && ScreenPose(it->second, &p)) {
        const Screen &s = it->second;
        const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
        const double mpp = kMpp * d, h = g_voice.pxH * mpp;
        vr::VROverlay()->SetOverlayWidthInMeters(g_voice.overlay, float(g_voice.px * mpp));
        const Mat at = Mul(p, OnSurface(s, 0, -s.heightMetres() / 2 + 24 * mpp + h / 2, 0.004));
        vr::VROverlay()->SetOverlayTransformAbsolute(g_voice.overlay, vr::TrackingUniverseStanding, &at);
        return true;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(g_voice.overlay, float(g_voice.px * kMpp));
    // (pitch: the direction it's seen from, down to it; PanelPose)
    const Mat rel = PanelPose(0, -0.3, -1.0, 0, -std::atan2(0.3, 1.0) * 180 / M_PI, 0);
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(g_voice.overlay, vr::k_unTrackedDeviceIndex_Hmd, &rel);
    return true;
}

}  // namespace

// Each tick.
void UpdateVoice() {
    if (!g_voice.on) return Hide();
    const int64_t quiet = NowMs() - g_voice.saidMs;
    if (quiet >= (g_voice.kind == Kind::Typing ? kTypingMs : kListenQuietMs)) {
        g_voice.on = false;
        std::printf("dictation notice: hidden (no word from Frametop Voice for %.1f s)\n", quiet / 1000.0);
        std::fflush(stdout);
        return Hide();
    }
    if (g_paused) return Hide();
    if (g_voice.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay(kVoiceKey, "Frametop: dictation", &g_voice.overlay) != vr::VROverlayError_None) {
            g_voice.overlay = vr::k_ulOverlayHandleInvalid;
            return;
        }
        vr::VROverlay()->SetOverlayInputMethod(g_voice.overlay, vr::VROverlayInputMethod_None);
        vr::VROverlay()->SetOverlaySortOrder(g_voice.overlay, 12);
        g_voice.drawnLevel = g_voice.drawnTurn = -1;
    }
    Draw();
    if (!Place()) return Hide();
    if (!g_voice.shown) PinShow(g_voice.overlay);
    g_voice.shown = true;
}

}  // namespace ftvr

// "listen hold|toggle|command LEVEL", "typing" or "off", from ft-voiced (compositor.c).
void ft_vr_voice(const char *words) {
    using namespace ftvr;
    char what[16] = {}, mode[16] = {};
    double level = 0;
    const int n = std::sscanf(words, "%15s %15s %lf", what, mode, &level);
    const bool was = g_voice.on;
    const Kind before = g_voice.kind;
    if (n >= 1 && std::strcmp(what, "listen") == 0) {
        g_voice.on = true, g_voice.kind = Kind::Listening;
        g_voice.hint = std::strcmp(mode, "hold") == 0 ? "release to type" : std::strcmp(mode, "command") == 0 ? "say a command" : "";
        g_voice.level = n >= 3 && std::isfinite(level) ? std::clamp(level, 0.0, 1.0) : 0;
    } else if (n >= 1 && std::strcmp(what, "typing") == 0) {
        g_voice.on = true, g_voice.kind = Kind::Typing;
    } else if (n >= 1 && std::strcmp(what, "off") == 0) {
        g_voice.on = false;
    } else {
        return;
    }
    g_voice.saidMs = NowMs();
    if (was != g_voice.on || before != g_voice.kind) {
        std::printf("dictation notice: %s\n", !g_voice.on ? "off" : g_voice.kind == Kind::Typing ? "typing" : "listening");
        std::fflush(stdout);
    }
}
