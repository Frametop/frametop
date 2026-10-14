// ft-gazepanel: the gaze calibration panel (docs/design.md, gaze/README.md). A SteamVR overlay
// fixed to the headset, so wherever you turn your head it stays in the same place in your
// view: a dot drawn at a head-relative direction is exactly that direction from the headset,
// which is what the gaze service needs to know where you were looking. It's drawn on the CPU
// and takes no input: the gaze service (gaze/ft-gazed) drives it, and the pointer helper
// passes it your presses (calaccept, calquit).
//
// The panel sits POINTER-like at --distance (1.5 m, about where Frametop's screens are, so
// the eyes converge as they do in use). "quick" is a small square, QUICK_DEG across, for the
// one-dot check; "five" is FIVE_DEG across (4:3), see-through like quick, for the five-dot
// check, whose dots are 12 degrees left and right and 9 up and down; "full" is FULL_DEG
// across (4:3), with a solid background whose brightness the service sets per round (pupil
// size changes with it, and the tracker's error with it); "fit" is FIT_DEG across (4:3),
// see-through like quick, for the headset fit check: a card per eye (tracked or lost, the
// tracker's signal, how much of the last 10 s it was seen) and hints. Every dot a check shows
// must fit its panel: gazecheck.py's PANEL_DEG mirrors these sizes and checks it. "note" is a
// strip NOTE_DEG across (4:1), see-through, NOTE_PITCH degrees below straight ahead, for the gaze
// service's slip notice: its title and text lines only, no dot.
//
// Control socket: abstract unix datagram "@ft_gazepanel" (--socket NAME); a sender with an
// address gets "ok" or "error ...":
//   show quick|five|full|fit|note the panel, empty, in front of you (note: below it)
//   hide
//   bg <0..1>                    the background's brightness (full)
//   dot <yaw> <pitch> <state> [<progress 0..1>]
//                                the dot, head-relative degrees (yaw +left, pitch +up); state:
//                                look, capture (a ring filling to progress), done,
//                                fail, off
//   title <text> / text <text>   a line at the top / at the bottom (empty to clear)
//   note <text>                  a warning line just above the bottom one, in orange (red on
//                                the bright round): why a dot wasn't taken (empty to clear)
//   eye <0|1> <r> <g> <b> <signal 0..1|-1> <seen 0..1|-1> <word>
//                                fit: an eye's card (0 left): its state in that colour, the
//                                tracker's signal, the share of the last 10 s it was seen
//   hints <line>|<line>|...      fit: lines under the cards (empty to clear)
//   ping
//
// The eye recorder's mode (gaze/rec/DESIGN.md, "Panel"): "show rec" is FULL_DEG across with
// the full calibration's solid background (bg), and the hand panel's lines (hands/rec/panel):
//   title <text>                 the big line at the top
//   step <text>                  a small line at the top right, "Round 2 of 5"; rec on|off puts a
//                                red "Rec" dot by it
//   text <text>                  the instruction, under the title, wrapped ("|" starts a line)
//   big <text> / action <text>   large in the middle (the countdown) / a cyan line under it
//   note <text> / keys <text>    the orange warning line / a faint line along the bottom
//   countdown <0..1>|off         a thin bar along the bottom: the share of the step's time left
//   paused on|off                "Paused" over the picture
// Its dot is a second overlay (OVERLAY.dot), drawn once per state and only moved, so a moving
// dot is smooth at the headset's frame rate and costs no pictures:
//   dot <yaw> <pitch> <state> [<progress>]   as above, at the panel's distance
//   path circle <yaw> <pitch> <radius> <seconds>
//   path line <yaw0> <pitch0> <yaw1> <pitch1> <seconds>
//   path eight <yaw> <pitch> <yaw radius> <pitch radius> <seconds>
//                                the dot moves by itself: round the circle once per seconds
//                                (from the top, going left), along the line and back (seconds
//                                each way, at a steady speed), or a figure eight (seconds a
//                                loop; up and down twice per side to side), until the next dot,
//                                path, world or "dot 0 0 off". Reply: "ok"
//   world <x> <y> <z> [<state> [<progress>]]
//                                a dot fixed in the room: the point is in the head frame now
//                                (metres, +x right, +y up, -z forward), placed in the room with
//                                the headset's pose, facing the head as it moves; the same point
//                                again changes only the state. Reply "ok <room x> <y> <z>"
//                                (standing universe)
//   distance <metres>            the panel and its dot that far away (0.25 to 10), the same size
//                                in degrees (the Depth bounty)
//   ipd                          "ok <metres>": SteamVR's IPD setting now (--no-vr: 0.0630)
//   log start <dir> / log stop   append to <dir>/targets.jsonl, poses.jsonl and ipd.jsonl (below)
//                                until stopped; "log stop" replies "ok <targets> <poses> <ipd>"
// targets.jsonl, the ground truth: what the dot did, when. A row for each frame the dot moves in
// (path, world) and for each change (dot, state, background, distance): {"t": when the frame
// reaches the eyes (estimated: the next vsync after the change, plus SteamVR's vsync to photons),
// "set": when the panel set it, both CLOCK_MONOTONIC_RAW seconds, the eye frames' clock;
// "frame": SteamVR's vsync count; "dot": "fixed|path|world|off", "yaw", "pitch" (head-relative
// degrees, for world from the head's predicted pose), "dist" (metres), "state", "progress",
// "bg"}. poses.jsonl: the headset's pose 250 times a second, {"t", "hmd": {"m": 3x4 row-major,
// standing universe, "r": ETrackingResult, "ok"}}. ipd.jsonl: SteamVR's IPD setting at the start
// and when it changes, {"t", "ipd": metres}, looked at 30 times a second as well as on its event.
//
// Each picture goes into the next of three shared buffers (linear DMA-BUFs SteamVR imported
// once, the size of the biggest panel; the texture bounds show the part in use), and the panel
// switches to it, as screens/keyboard.cpp does. SetOverlayRaw, which uploads a new texture each
// time, flickered on every change of the full calibration's 1024x768 picture, and in a live
// test the headset kept showing an old picture after the panel had drawn new ones (2026-10-01).
// It's only the fallback. A "show" makes the panel visible once its first picture is in.
//
// Options: --watch-stdin (quit when stdin closes: the service runs it), --socket NAME,
// --overlay KEY (frametop.gazepanel; the eye recorder runs a copy of its own next to the gaze
// service's), --distance METRES, --no-vr (no SteamVR: for tests, at a made-up 90 Hz vsync; with
// --dump DIR each picture goes there as a PAM file). Runs in the dev container (gaze/build.sh
// builds it into gaze/build).
#include <openvr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kQuickDeg = 16;      // QUICK_DEG: the one-dot check's square
constexpr double kFullDeg = 64;       // FULL_DEG: the full calibration's width (4:3)
constexpr double kFitDeg = 40;        // FIT_DEG: the headset fit check's width (4:3)
constexpr double kFiveDeg = 40;       // FIVE_DEG: the five-dot check's width (4:3), past its dots
constexpr double kNoteDeg = 30;       // NOTE_DEG: the slip notice's strip (4:1)
constexpr double kNotePitch = -16;    // NOTE_PITCH: the strip's centre, degrees below straight ahead
constexpr int kQuickPx = 320, kFullW = 1024, kFullH = 768, kFitW = 800, kFitH = 600, kFiveW = 800, kFiveH = 600;
constexpr int kNoteW = 600, kNoteH = 150;
constexpr double kDotDeg = 1.8;       // the rec mode's dot overlay: the ring's 1.5 degrees, and a margin
constexpr int kDotPx = 96;
constexpr int64_t kPosePeriodNs = 4'000'000;  // 250 Hz, as the hand panel
constexpr int64_t kIpdPeriodNs = 33'000'000;
std::atomic<bool> g_stop{false};
std::mutex g_vr;  // around OpenVR calls once the pose log's thread makes them too

int64_t RawNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// ---------------------------------------------------------------- text (as screens/keyboard.cpp)

stbtt_fontinfo g_font;
std::vector<unsigned char> g_fontData;
bool g_fontOk = false;
constexpr int kMaxW = kFullW, kMaxH = kFullH;  // the buffers' size: the biggest panel
struct Glyph {
    std::vector<unsigned char> bitmap;
    int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};
std::map<std::pair<uint32_t, int>, Glyph> g_glyphs;

void LoadFont() {
    std::string path;
    if (FILE *p = popen("fc-match -f '%{file}' 'Noto Sans' 2>/dev/null", "r")) {
        char buf[512];
        if (std::fgets(buf, sizeof buf, p)) path = buf;
        pclose(p);
    }
    if (path.empty()) path = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf";
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        g_fontData.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        g_fontOk = std::fread(g_fontData.data(), 1, g_fontData.size(), f) == g_fontData.size() &&
                   stbtt_InitFont(&g_font, g_fontData.data(), stbtt_GetFontOffsetForIndex(g_fontData.data(), 0));
        std::fclose(f);
    }
    if (!g_fontOk) std::fprintf(stderr, "ft-gazepanel: no font (%s); no text\n", path.c_str());
}

const Glyph &GetGlyph(uint32_t cp, int size) {
    auto [it, fresh] = g_glyphs.try_emplace({cp, size});
    Glyph &g = it->second;
    if (fresh) {
        const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
        unsigned char *b = stbtt_GetCodepointBitmap(&g_font, 0, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
        if (b) g.bitmap.assign(b, b + size_t(g.w) * g.h), stbtt_FreeBitmap(b, nullptr);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &adv, &lsb);
        g.advance = int(std::lround(adv * scale));
    }
    return g;
}

std::vector<uint32_t> Codepoints(const std::string &s) {
    std::vector<uint32_t> out;
    for (const unsigned char *p = (const unsigned char *)s.c_str(); *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*p & 0xC0) == 0x80; --more) c = c << 6 | (*p++ & 0x3F);
        out.push_back(c);
    }
    return out;
}

int TextWidth(const std::string &s, int size) {
    if (!g_fontOk) return 0;
    int width = 0;
    for (uint32_t cp : Codepoints(s)) width += GetGlyph(cp, size).advance;
    return width;
}

// s as lines no wider than width (the hand panel's): "|" starts a new line, and lines break
// between words.
std::vector<std::string> Wrap(const std::string &s, int size, int width) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    for (size_t at = 0; at <= s.size();) {
        const size_t bar = std::min(s.find('|', at), s.size());
        const std::string para = s.substr(at, bar - at);
        std::string line;
        for (size_t w = 0; w < para.size();) {
            const size_t space = std::min(para.find(' ', w), para.size());
            const std::string word = para.substr(w, space - w);
            w = space + 1;
            if (word.empty()) continue;
            const std::string longer = line.empty() ? word : line + " " + word;
            if (line.empty() || TextWidth(longer, size) <= width) {
                line = longer;
            } else {
                out.push_back(line);
                line = word;
            }
        }
        out.push_back(line);
        at = bar + 1;
    }
    return out;
}

// ---------------------------------------------------------------- the picture

struct EyeCard {
    std::string word = "no data";
    double r = 0.6, g = 0.6, b = 0.6, signal = -1, seen = -1;
};

struct Panel {
    bool full = false, fit = false, rec = false, strip = false;  // strip: "show note"
    std::string step, big, action, keys;  // rec
    bool recOn = false, paused = false;
    double countdown = -1;
    EyeCard eyes[2];
    std::vector<std::string> hints;
    int w = kQuickPx, h = kQuickPx;
    double wDeg = kQuickDeg;  // across
    std::vector<uint8_t> px;
    double bg = 0.05;
    std::string title, text, note;
    bool dotOn = false;
    double dotYaw = 0, dotPitch = 0, progress = 0;
    std::string state = "off";
};

void Blend(Panel &p, int x, int y, double r, double g, double b, double a) {
    if (x < 0 || y < 0 || x >= p.w || y >= p.h || a <= 0) return;
    uint8_t *q = &p.px[(size_t(y) * p.w + x) * 4];
    a = std::min(a, 1.0);
    q[0] = uint8_t(std::lround(q[0] + (r * 255 - q[0]) * a));
    q[1] = uint8_t(std::lround(q[1] + (g * 255 - q[1]) * a));
    q[2] = uint8_t(std::lround(q[2] + (b * 255 - q[2]) * a));
    q[3] = uint8_t(std::lround(q[3] + (255 - q[3]) * a));
}

// A filled disc, or (inner > 0) a ring from inner to outer radius, anti-aliased; with sweep < 1,
// only that share of the ring, clockwise from the top.
void Disc(Panel &p, double cx, double cy, double outer, double inner, double r, double g, double b, double a,
          double sweep = 1) {
    const int x0 = int(cx - outer - 1), x1 = int(cx + outer + 1), y0 = int(cy - outer - 1), y1 = int(cy + outer + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d = std::hypot(dx, dy);
            double cover = std::clamp(outer - d + 0.5, 0.0, 1.0);
            if (inner > 0) cover = std::min(cover, std::clamp(d - inner + 0.5, 0.0, 1.0));
            if (sweep < 1) {
                double ang = std::atan2(dx, -dy) / (2 * M_PI);  // 0 at the top, clockwise
                if (ang < 0) ang += 1;
                if (ang > sweep) continue;
            }
            Blend(p, x, y, r, g, b, a * cover);
        }
}

void Rect(Panel &p, int x0, int y0, int x1, int y1, double r, double g, double b, double a) {
    for (int y = std::max(y0, 0); y < std::min(y1, p.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, p.w); ++x) Blend(p, x, y, r, g, b, a);
}

// Text centred on (x, cy), or starting at x (left).
void Text(Panel &p, const std::string &s, int size, int x, int cy, double r, double g, double b, bool left = false) {
    if (!g_fontOk || s.empty()) return;
    const auto cps = Codepoints(s);
    int width = 0;
    for (uint32_t cp : cps) width += GetGlyph(cp, size).advance;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    if (!left) x -= width / 2;
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : cps) {
        const Glyph &gl = GetGlyph(cp, size);
        for (int gy = 0; gy < gl.h; ++gy)
            for (int gx = 0; gx < gl.w; ++gx)
                Blend(p, x + gl.xoff + gx, baseline + gl.yoff + gy, r, g, b, gl.bitmap[size_t(gy) * gl.w + gx] / 255.0);
        x += gl.advance;
    }
}

void Text(Panel &p, const std::string &s, int size, int x, int cy, double lum, bool left = false) {
    Text(p, s, size, x, cy, lum, lum, lum, left);
}

// The headset fit check (see the top): a card per eye, then the hints.
void DrawFit(Panel &p, double pxPerDeg, int textSize, double faint) {
    const int margin = int(p.w * 0.06), cw = int(p.w * 0.41), ch = int(p.h * 0.32), top = int(p.h * 0.12);
    const int pad = int(pxPerDeg * 0.9), big = int(pxPerDeg * 1.6), small = int(pxPerDeg * 0.8);
    for (int k = 0; k < 2; ++k) {
        const EyeCard &e = p.eyes[k];
        const int x0 = k == 0 ? margin : p.w - margin - cw;
        Rect(p, x0, top, x0 + cw, top + ch, 1, 1, 1, 0.07);
        Text(p, k ? "Right eye" : "Left eye", textSize, x0 + pad, top + pad + textSize / 2, faint, true);
        Text(p, e.word, big, x0 + pad, top + int(ch * 0.40), e.r, e.g, e.b, true);
        // The tracker's signal: a bar, red to green.
        const int by = top + int(ch * 0.62), bh = std::max(4, int(pxPerDeg * 0.35)), bw = cw - 2 * pad;
        Text(p, "Signal", small, x0 + pad, by - small, faint, true);
        Rect(p, x0 + pad, by, x0 + pad + bw, by + bh, 1, 1, 1, 0.15);
        if (e.signal >= 0) {
            const double v = std::clamp(e.signal, 0.0, 1.0);
            const double r = v < 0.5 ? 1.0 : 1.0 - 1.3 * (v - 0.5), g = v < 0.5 ? 0.3 + 0.9 * v : 0.75 + 0.5 * (v - 0.5);
            Rect(p, x0 + pad, by, x0 + pad + int(bw * v), by + bh, r, g, 0.35, 0.95);
        }
        char seen[64] = "Seen: not yet";
        if (e.seen >= 0) std::snprintf(seen, sizeof seen, "Seen %d%% of the last 10 s", int(std::lround(e.seen * 100)));
        Text(p, seen, small, x0 + pad, top + ch - pad, faint, true);
    }
    int y = top + ch + pad * 2;
    for (const std::string &line : p.hints) {
        Text(p, line, textSize, margin, y, faint, true);
        y += int(textSize * 1.4);
    }
}

// Head-relative direction -> panel pixel: the panel is a plane `d` in front of the headset.
void ToPixel(const Panel &p, double yaw, double pitch, double &x, double &y) {
    const double yr = yaw * M_PI / 180, pr = pitch * M_PI / 180;
    const double half = std::tan(p.wDeg * M_PI / 360);  // half the width, per unit of distance
    const double X = -std::tan(yr), Y = std::tan(pr) / std::cos(yr);
    x = (X / (2 * half) + 0.5) * p.w;
    y = (0.5 - Y / (2 * half) * p.w / p.h) * p.h;
}

void DrawDot(Panel &p, double x, double y, double pxPerDeg, const std::string &state, double progress, double ink);

// The eye recorder's panel (see the top). Its dot is the other overlay's.
void DrawRec(Panel &p, double pxPerDeg) {
    const bool light = p.bg > 0.5;
    const double faint = light ? 0.2 : 0.75, ink = light ? 0.05 : 0.95;
    const int titleSize = int(pxPerDeg * 1.5), textSize = int(pxPerDeg * 1.2), small = int(pxPerDeg * 0.9);
    const int bigSize = int(pxPerDeg * 4.0), margin = int(p.w * 0.05);
    const int titleY = int(titleSize * 1.2);
    Text(p, p.title, titleSize, p.w / 2, titleY, ink);
    if (!p.step.empty()) {
        const int sw = TextWidth(p.step, small);
        Text(p, p.step, small, p.w - margin - sw, titleY, faint, true);
        if (p.recOn) {
            const int rx = p.w - margin - sw - small;
            Disc(p, rx, titleY, small * 0.35, 0, 0.9, 0.15, 0.1, 1);
            Text(p, "Rec", small, rx - small - TextWidth("Rec", small), titleY, 1.0, 0.35, 0.3, true);
        }
    } else if (p.recOn) {
        Disc(p, p.w - margin - small * 0.35, titleY, small * 0.35, 0, 0.9, 0.15, 0.1, 1);
    }
    int y = titleY + int(titleSize * 1.4);
    for (const std::string &line : Wrap(p.text, textSize, int(p.w * 0.8))) {
        Text(p, line, textSize, p.w / 2, y, faint);
        y += int(textSize * 1.35);
    }
    Text(p, p.big, bigSize, p.w / 2, p.h / 2, ink);
    if (light) Text(p, p.action, textSize, p.w / 2, int(p.h * 0.68), 0.0, 0.35, 0.45);
    else Text(p, p.action, textSize, p.w / 2, int(p.h * 0.68), 0.35, 0.85, 1.0);
    if (light) Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 0.7, 0.12, 0.05);
    else Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 1.0, 0.62, 0.3);
    Text(p, p.keys, small, p.w / 2, p.h - int(small * 1.6), faint);
    if (p.countdown >= 0) {
        const int bh = std::max(3, int(pxPerDeg * 0.2));
        Rect(p, 0, p.h - bh, p.w, p.h, ink, ink, ink, 0.12);
        Rect(p, 0, p.h - bh, int(p.w * std::clamp(p.countdown, 0.0, 1.0)), p.h, 0.35, 0.85, 1.0, 0.9);
    }
    if (p.paused) {
        Rect(p, 0, 0, p.w, p.h, 0, 0, 0, 0.55);
        Text(p, "Paused", bigSize, p.w / 2, p.h / 2, 1.0);
    }
}

void Draw(Panel &p) {
    p.px.assign(size_t(p.w) * p.h * 4, 0);
    const double pxPerDeg = p.w / p.wDeg;
    if (p.rec) {
        for (size_t i = 0; i < p.px.size(); i += 4)
            p.px[i] = p.px[i + 1] = p.px[i + 2] = uint8_t(std::lround(p.bg * 255)), p.px[i + 3] = 255;
        DrawRec(p, pxPerDeg);
        return;
    }
    if (p.full) {
        for (size_t i = 0; i < p.px.size(); i += 4)
            p.px[i] = p.px[i + 1] = p.px[i + 2] = uint8_t(std::lround(p.bg * 255)), p.px[i + 3] = 255;
    } else {
        // The quick check and the fit check: a dim rounded panel, see-through, so it's clear of what's behind.
        const double r = std::min(p.w, p.h) * 0.12;
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x) {
                const double dx = std::max({r - x - 0.5, x + 0.5 - (p.w - r), 0.0});
                const double dy = std::max({r - y - 0.5, y + 0.5 - (p.h - r), 0.0});
                const double cover = std::clamp(r - std::hypot(dx, dy) + 0.5, 0.0, 1.0);
                Blend(p, x, y, 0.06, 0.06, 0.07, 0.82 * cover);
            }
    }
    const bool light = p.full && p.bg > 0.5;  // a dark dot on the bright round
    const double ink = light ? 0.0 : 1.0, faint = light ? 0.2 : 0.75;
    if (p.strip) {
        // The slip notice: the title and the hint, together in the middle of the strip.
        Text(p, p.title, int(pxPerDeg * 1.4), p.w / 2, int(p.h * 0.36), ink);
        Text(p, p.text, int(pxPerDeg * 1.05), p.w / 2, int(p.h * 0.68), faint);
        return;
    }
    const int titleSize = int(pxPerDeg * (p.full ? 1.5 : 1.1)), textSize = int(pxPerDeg * (p.full ? 1.2 : 0.9));
    Text(p, p.title, titleSize, p.w / 2, int(titleSize * 1.2), faint);
    Text(p, p.text, textSize, p.w / 2, p.h - int(textSize * 1.3), faint);
    // Under the full calibration's lowest dots (RING degrees down) and over the text.
    if (light) Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 0.7, 0.12, 0.05);
    else Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 1.0, 0.62, 0.3);
    if (p.fit) DrawFit(p, pxPerDeg, textSize, faint);
    if (!p.dotOn || p.state == "off") return;
    double x, y;
    ToPixel(p, p.dotYaw, p.dotPitch, x, y);
    DrawDot(p, x, y, pxPerDeg, p.state, p.progress, ink);
}

// The dot at (x, y): its look for each state.
void DrawDot(Panel &p, double x, double y, double pxPerDeg, const std::string &state, double progress, double ink) {
    const double core = 0.22 * pxPerDeg;
    if (state == "look") {
        // Still, so the panel looks solid and is drawn again only when something changes.
        const double rr = 0.75 * pxPerDeg;
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, ink, ink, ink, 0.6);
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    } else if (state == "capture") {
        const double rr = 0.75 * pxPerDeg;
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, ink, ink, ink, 0.25);
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, 0.3, 0.85, 1.0, 1, std::clamp(progress, 0.0, 1.0));
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    } else if (state == "done") {
        Disc(p, x, y, 0.75 * pxPerDeg, 0, 0.25, 0.85, 0.4, 0.9);
        Disc(p, x, y, core, 0, 1, 1, 1, 1);
    } else if (state == "fail") {
        Disc(p, x, y, 0.75 * pxPerDeg, 0.6 * pxPerDeg, 0.95, 0.35, 0.3, 0.9);
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    }
}


// ---------------------------------------------------------------- the buffers (see the top)

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
};

struct Buffers {
    int w = kMaxW, h = kMaxH;  // the biggest picture they take
    int drm = -1;
    gbm_device *gbm = nullptr;
    Buffer b[3];
    int next = 0;
    bool ok = false;

    bool Make() {
        std::lock_guard<std::mutex> lk(g_vr);
        drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm >= 0) gbm = gbm_create_device(drm);
        for (Buffer &x : b) {
            // ABGR8888 is R, G, B, A in memory, like Panel::px.
            if (gbm) x.bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!x.bo || (x.fd = gbm_bo_get_fd(x.bo)) < 0) break;
            vr::DmabufAttributes_t a{};
            a.unWidth = w, a.unHeight = h;
            a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
            a.unFormat = DRM_FORMAT_ABGR8888;
            a.ulModifier = DRM_FORMAT_MOD_LINEAR;
            a.unPlaneCount = 1;
            a.plane[0].unOffset = gbm_bo_get_offset(x.bo, 0);
            a.plane[0].unStride = gbm_bo_get_stride(x.bo);
            a.plane[0].nFd = x.fd;
            if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &x.handle)) x.handle = 0;
            if (!x.handle) break;
        }
        ok = b[2].handle != 0;
        if (!ok) {
            std::fprintf(stderr, "ft-gazepanel: no shared buffers; falling back to SetOverlayRaw (it flickers)\n");
            DropLocked();
        }
        return ok;
    }

    void Drop() {
        std::lock_guard<std::mutex> lk(g_vr);
        DropLocked();
    }

    void DropLocked() {
        for (Buffer &x : b) {
            if (x.handle) vr::VRIPCResourceManager()->UnrefResource(x.handle);
            if (x.fd >= 0) close(x.fd);
            if (x.bo) gbm_bo_destroy(x.bo);
            x = Buffer{};
        }
        if (gbm) gbm_device_destroy(gbm);
        if (drm >= 0) close(drm);
        gbm = nullptr, drm = -1, ok = false;
    }

    // p's picture to the overlay: into the next buffer, premultiplied (the overlay's flag says
    // so), then the overlay switches to it.
    void Present(vr::IVROverlay *ov, vr::VROverlayHandle_t h, const Panel &p) {
        vr::EVROverlayError e;
        if (!ok) {
            std::lock_guard<std::mutex> lk(g_vr);
            e = ov->SetOverlayRaw(h, const_cast<uint8_t *>(p.px.data()), uint32_t(p.w), uint32_t(p.h), 4);
        } else {
            Buffer &x = b[next];
            next = (next + 1) % 3;
            uint32_t stride = 0;
            void *mapping = nullptr;
            auto *dst = static_cast<uint8_t *>(gbm_bo_map(x.bo, 0, 0, p.w, p.h, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
            if (!dst) {
                std::fprintf(stderr, "ft-gazepanel: can't map a buffer\n");
                return;
            }
            for (int y = 0; y < p.h; ++y) {
                const uint8_t *src = &p.px[size_t(y) * p.w * 4];
                uint8_t *row = dst + size_t(y) * stride;
                for (int i = 0; i < p.w * 4; i += 4) {
                    const unsigned a = src[i + 3];
                    row[i] = uint8_t(src[i] * a / 255), row[i + 1] = uint8_t(src[i + 1] * a / 255);
                    row[i + 2] = uint8_t(src[i + 2] * a / 255), row[i + 3] = uint8_t(a);
                }
            }
            gbm_bo_unmap(x.bo, mapping);
            std::lock_guard<std::mutex> lk(g_vr);
            const vr::VRTextureBounds_t bounds{0, 0, float(p.w) / w, float(p.h) / this->h};
            ov->SetOverlayTextureBounds(h, &bounds);
            vr::Texture_t tex = {&x.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
            e = ov->SetOverlayTexture(h, &tex);
        }
        if (e != vr::VROverlayError_None)
            std::fprintf(stderr, "ft-gazepanel: the picture didn't go to SteamVR: %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
};

// ---------------------------------------------------------------- the rec mode's logs (see the top)

void PutFloat(FILE *f, double v) { std::fprintf(f, "%.6g", std::isfinite(v) ? v : 0.0); }
double Sec(int64_t ns) { return double(ns) / 1e9; }

// A JSON-lines file the main thread writes.
struct Lines {
    FILE *f = nullptr;
    long rows = 0;
    bool Open(const std::string &path) {
        Close();
        if (!(f = std::fopen(path.c_str(), "ae"))) return false;
        std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
        rows = 0;
        return true;
    }
    long Close() {
        if (!f) return rows;
        if (std::fflush(f) != 0 || std::ferror(f)) std::fprintf(stderr, "ft-gazepanel: writing a log failed\n");
        std::fclose(f);
        f = nullptr;
        return rows;
    }
};

// The headset's pose 250 times a second (as the hand panel's, head only), on a thread of its
// own, so a picture or a command never delays a sample.
struct PoseLog {
    FILE *f = nullptr;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<long> samples{0};

    bool Start(const std::string &path, bool vr) {
        Stop();
        if (!(f = std::fopen(path.c_str(), "ae"))) return false;
        std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
        samples = 0;
        if (vr) run = true, th = std::thread([this] { Run(); });
        return true;
    }

    long Stop() {
        if (!f) return 0;
        run = false;
        if (th.joinable()) th.join();
        if (std::fflush(f) != 0 || std::ferror(f)) std::fprintf(stderr, "ft-gazepanel: writing the pose log failed\n");
        std::fclose(f);
        f = nullptr;
        return samples;
    }

    void Run() {
        auto *sys = vr::VRSystem();
        vr::TrackedDevicePose_t pose{};
        int64_t next = RawNs(), lastFlush = next;
        while (run) {
            int64_t before, after;
            {
                std::lock_guard<std::mutex> lk(g_vr);
                before = RawNs();
                sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
                after = RawNs();
            }
            const int64_t t = before + (after - before) / 2;
            std::fprintf(f, "{\"t\": %.6f, \"hmd\": ", Sec(t));
            if (!pose.bDeviceIsConnected) {
                std::fputs("null}\n", f);
            } else {
                std::fputs("{\"m\": [", f);
                for (int k = 0; k < 12; ++k) {
                    if (k) std::fputs(", ", f);
                    PutFloat(f, pose.mDeviceToAbsoluteTracking.m[k / 4][k % 4]);
                }
                std::fprintf(f, "], \"r\": %d, \"ok\": %s}}\n", int(pose.eTrackingResult), pose.bPoseIsValid ? "true" : "false");
            }
            ++samples;
            if (t - lastFlush >= 1'000'000'000) std::fflush(f), lastFlush = t;
            next += kPosePeriodNs;
            const int64_t now = RawNs();
            if (next < now - kPosePeriodNs) next = now;
            const timespec ts{time_t(next / 1'000'000'000), long(next % 1'000'000'000)};
            clock_nanosleep(CLOCK_MONOTONIC_RAW, TIMER_ABSTIME, &ts, nullptr);
        }
    }
};

// ---------------------------------------------------------------- the rec mode's dot

// A head-relative direction (yaw +left, pitch +up; see ToPixel) as a unit vector in the head
// frame (+x right, +y up, -z forward), and back.
void Direction(double yaw, double pitch, double v[3]) {
    const double yr = yaw * M_PI / 180, pr = pitch * M_PI / 180;
    v[0] = -std::sin(yr) * std::cos(pr), v[1] = std::sin(pr), v[2] = -std::cos(yr) * std::cos(pr);
}

void Angles(const double v[3], double &yaw, double &pitch) {
    yaw = std::atan2(-v[0], -v[2]) * 180 / M_PI;
    pitch = std::atan2(v[1], std::hypot(v[0], v[2])) * 180 / M_PI;
}

// An overlay's transform at `at`, its face (+z) turned toward `eye`, upright (the hand panel's).
vr::HmdMatrix34_t Facing(const double at[3], const double eye[3]) {
    auto norm = [](double v[3]) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-9) return false;
        v[0] /= n, v[1] /= n, v[2] /= n;
        return true;
    };
    double z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    if (!norm(z)) z[0] = z[1] = 0, z[2] = 1;
    double x[3] = {z[2], 0, -z[0]};
    if (!norm(x)) x[0] = 1, x[1] = x[2] = 0;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    vr::HmdMatrix34_t m{};
    for (int r = 0; r < 3; ++r)
        m.m[r][0] = float(x[r]), m.m[r][1] = float(y[r]), m.m[r][2] = float(z[r]), m.m[r][3] = float(at[r]);
    return m;
}

// m's point p in the room -> the device frame (m is device to room, rotation orthonormal).
void ToDevice(const vr::HmdMatrix34_t &m, const double p[3], double out[3]) {
    const double d[3] = {p[0] - m.m[0][3], p[1] - m.m[1][3], p[2] - m.m[2][3]};
    for (int c = 0; c < 3; ++c) out[c] = m.m[0][c] * d[0] + m.m[1][c] * d[1] + m.m[2][c] * d[2];
}

void ToRoom(const vr::HmdMatrix34_t &m, const double p[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m.m[r][0] * p[0] + m.m[r][1] * p[1] + m.m[r][2] * p[2] + m.m[r][3];
}

struct Dot {
    std::string mode = "off";  // off, fixed, path, world
    double yaw = 0, pitch = 0;
    std::string state = "look";
    double progress = 0;
    // path
    std::string kind;
    double a[4] = {0, 0, 0, 0}, seconds = 1;
    int64_t start = -1;  // the path's first frame's photon time (ns), set on that frame
    // world
    double room[3] = {0, 0, 0}, head[3] = {0, 0, 0};  // where it is, and the head-frame point it came from
    // drawn: what the overlay's picture shows, and whether it's visible
    std::string drawnState;
    double drawnProgress = -1, drawnBg = -1;
    bool shown = false;
    bool changed = true;  // log a row on the next frame
};

// Where a path's dot is t seconds after it started.
void PathAt(const Dot &d, double t, double &yaw, double &pitch) {
    const double ph = t / std::max(d.seconds, 0.05);
    if (d.kind == "circle") {
        yaw = d.a[0] + d.a[2] * std::sin(2 * M_PI * ph), pitch = d.a[1] + d.a[2] * std::cos(2 * M_PI * ph);
    } else if (d.kind == "line") {
        double f = std::fmod(ph, 2.0);
        if (f > 1) f = 2 - f;
        yaw = d.a[0] + (d.a[2] - d.a[0]) * f, pitch = d.a[1] + (d.a[3] - d.a[1]) * f;
    } else {  // eight
        yaw = d.a[0] + d.a[2] * std::sin(2 * M_PI * ph), pitch = d.a[1] + d.a[3] * std::sin(4 * M_PI * ph);
    }
}

// --no-vr: a picture as a PAM file (RGBA, straight alpha), as the hand panel's.
void Dump(const Panel &p, const std::string &path) {
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", p.w, p.h);
        std::fwrite(p.px.data(), 1, p.px.size(), f);
        std::fclose(f);
    }
}

// The headset's pose `ahead` seconds from now; --no-vr's is at the room's origin, looking down -z.
vr::TrackedDevicePose_t HeadPose(bool noVr, float ahead) {
    vr::TrackedDevicePose_t pose{};
    if (noVr) {
        pose.bPoseIsValid = pose.bDeviceIsConnected = true;
        pose.mDeviceToAbsoluteTracking.m[0][0] = pose.mDeviceToAbsoluteTracking.m[1][1] = pose.mDeviceToAbsoluteTracking.m[2][2] = 1;
    } else {
        std::lock_guard<std::mutex> lk(g_vr);
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, ahead, &pose, 1);
    }
    return pose;
}

bool Is(const char *buf, const char *word, const char **rest = nullptr) {
    const size_t n = std::strlen(word);
    if (std::strncmp(buf, word, n) || (buf[n] && buf[n] != ' ')) return false;
    if (rest) *rest = buf[n] ? buf + n + 1 : buf + n;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    bool watchStdin = false, noVr = false;
    std::string sockName = "ft_gazepanel", overlayKey = "frametop.gazepanel", dumpDir;
    double distance = 1.5;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--watch-stdin")) watchStdin = true;
        else if (!std::strcmp(argv[i], "--no-vr")) noVr = true;
        else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) sockName = argv[++i];
        else if (!std::strcmp(argv[i], "--overlay") && i + 1 < argc) overlayKey = argv[++i];
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else if (!std::strcmp(argv[i], "--distance") && i + 1 < argc) distance = std::clamp(std::atof(argv[++i]), 0.5, 5.0);
        else {
            std::fprintf(stderr, "usage: %s [--watch-stdin] [--socket NAME] [--overlay KEY] [--distance METRES] "
                                 "[--no-vr [--dump DIR]]\n", argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    if (watchStdin)
        std::thread([] {
            char c[256];
            while (read(0, c, sizeof c) > 0) {
            }
            g_stop = true;
        }).detach();

    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, sockName.data(), std::min(sockName.size(), sizeof addr.sun_path - 2));
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sockName.size())) != 0) {
        std::fprintf(stderr, "ft-gazepanel: @%s is taken (another copy running?)\n", sockName.c_str());
        return 1;
    }

    vr::IVROverlay *ov = nullptr;
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid, dotH = vr::k_ulOverlayHandleInvalid;
    double period = 1 / 90.0, toPhotons = 0;
    if (!noVr) {
        // As Frametop's other SteamVR clients: background first, so we never start vrserver.
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, vr::VRApplication_Background);
        if (err == vr::VRInitError_None) {
            vr::VR_Shutdown();
            vr::VR_Init(&err, vr::VRApplication_Overlay);
        }
        if (err != vr::VRInitError_None) {
            std::fprintf(stderr, "ft-gazepanel: SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            return 1;
        }
        ov = vr::VROverlay();
        if (ov->CreateOverlay(overlayKey.c_str(), "Frametop gaze calibration", &h) != vr::VROverlayError_None ||
            ov->CreateOverlay((overlayKey + ".dot").c_str(), "Frametop gaze dot", &dotH) != vr::VROverlayError_None) {
            std::fprintf(stderr, "ft-gazepanel: can't create the overlay (another copy running?)\n");
            return 1;
        }
        ov->SetOverlaySortOrder(h, 250);  // in front of Frametop's screens and the pointer's dot
        ov->SetOverlaySortOrder(dotH, 251);
        auto *sys = vr::VRSystem();
        vr::ETrackedPropertyError pe;
        const float hz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &pe);
        if (pe == vr::TrackedProp_Success && hz > 30) period = 1.0 / hz;
        const float ph = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float, &pe);
        if (pe == vr::TrackedProp_Success && ph >= 0 && ph < 0.1) toPhotons = ph;
    }
    LoadFont();
    Buffers buffers, dotBuffers;
    dotBuffers.w = dotBuffers.h = kDotPx;
    if (!noVr && buffers.Make()) ov->SetOverlayFlag(h, vr::VROverlayFlags_IsPremultiplied, true);
    if (!noVr && dotBuffers.Make()) ov->SetOverlayFlag(dotH, vr::VROverlayFlags_IsPremultiplied, true);
    if (!dumpDir.empty()) mkdir(dumpDir.c_str(), 0755);

    Panel p;
    Panel dotPic;  // the rec mode's dot overlay's picture
    dotPic.w = dotPic.h = kDotPx, dotPic.wDeg = kDotDeg;
    Dot dot;
    Lines targets, ipdLog;
    PoseLog poses;
    float ipdNow = -1;
    int64_t ipdAt = 0;
    int pictures = 0;
    bool visible = false, dirty = false, shown = false;  // shown: SteamVR shows it (after its first picture)
    auto place = [&] {
        if (noVr) return;
        const double half = std::tan(p.wDeg * M_PI / 360);
        vr::HmdMatrix34_t m{};
        // Turned down about x by the strip's pitch (none for the others), facing the eyes.
        const double tilt = p.strip ? kNotePitch * M_PI / 180 : 0, c = std::cos(tilt), sn = std::sin(tilt);
        m.m[0][0] = 1;
        m.m[1][1] = float(c), m.m[1][2] = float(-sn), m.m[2][1] = float(sn), m.m[2][2] = float(c);
        m.m[1][3] = float(distance * sn), m.m[2][3] = float(-distance * c);
        std::lock_guard<std::mutex> lk(g_vr);
        ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        ov->SetOverlayWidthInMeters(h, float(2 * distance * half));
    };
    auto hideDot = [&] {
        if (dot.shown && !noVr) {
            std::lock_guard<std::mutex> lk(g_vr);
            ov->HideOverlay(dotH);
        }
        dot.shown = false;
    };
    // The next frame's vsync count and when it reaches the eyes (RAW ns).
    auto nextFrame = [&](int64_t now, uint64_t &frame) {
        double since = 0;
        frame = 0;
        if (!noVr) {
            float s = 0;
            std::lock_guard<std::mutex> lk(g_vr);
            if (vr::VRSystem()->GetTimeSinceLastVsync(&s, &frame)) since = s;
        } else {
            const int64_t per = int64_t(period * 1e9);
            since = double(now % per) / 1e9, frame = uint64_t(now / per);
        }
        frame += 1;
        return now + int64_t((period - std::min(since, period) + toPhotons) * 1e9);
    };
    auto ipdLook = [&](int64_t now, const char *why) {
        if (noVr || !ipdLog.f) return;
        vr::ETrackedPropertyError pe;
        float v;
        {
            std::lock_guard<std::mutex> lk(g_vr);
            v = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe);
        }
        ipdAt = now;
        if (pe != vr::TrackedProp_Success || v == ipdNow) return;
        ipdNow = v;
        std::fprintf(ipdLog.f, "{\"t\": %.6f, \"ipd\": %.5f, \"from\": \"%s\"}\n", Sec(now), v, why);
        ++ipdLog.rows;
    };
    // The rec mode's dot, once a frame: where it is at the next frame's photons, the overlay
    // moved there, and a targets row when it moved or changed.
    auto dotTick = [&](int64_t now) {
        if (!p.rec || !visible || dot.mode == "off") {
            if (dot.changed && targets.f && p.rec) {
                uint64_t frame;
                const int64_t t = nextFrame(now, frame);
                std::fprintf(targets.f, "{\"t\": %.6f, \"set\": %.6f, \"frame\": %llu, \"dot\": \"off\", \"dist\": %.3f, \"bg\": %.3f}\n",
                             Sec(t), Sec(now), (unsigned long long)frame, distance, p.bg);
                ++targets.rows;
            }
            dot.changed = false;
            hideDot();
            return;
        }
        uint64_t frame;
        const int64_t t = nextFrame(now, frame);
        double yaw = dot.yaw, pitch = dot.pitch, dist = distance;
        bool moved = false;
        vr::HmdMatrix34_t m{};
        bool absolute = false;
        if (dot.mode == "path") {
            if (dot.start < 0) dot.start = t;
            PathAt(dot, Sec(t - dot.start), yaw, pitch);
            moved = true;
        }
        if (dot.mode == "world") {
            // The head where it will be at the photons, and the dot's direction from it.
            const vr::TrackedDevicePose_t pose = HeadPose(noVr, float(Sec(t - now)));
            if (pose.bPoseIsValid) {
                double v[3];
                ToDevice(pose.mDeviceToAbsoluteTracking, dot.room, v);
                dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (dist > 1e-6) {
                    for (double &c : v) c /= dist;
                    Angles(v, yaw, pitch);
                }
                const double eye[3] = {pose.mDeviceToAbsoluteTracking.m[0][3], pose.mDeviceToAbsoluteTracking.m[1][3],
                                       pose.mDeviceToAbsoluteTracking.m[2][3]};
                m = Facing(dot.room, eye);
                absolute = true;
            }
            moved = true;
        } else {
            double v[3];
            Direction(yaw, pitch, v);
            const double at[3] = {v[0] * dist, v[1] * dist, v[2] * dist}, eye[3] = {0, 0, 0};
            m = Facing(at, eye);
        }
        const bool redraw = dot.state != dot.drawnState || dot.progress != dot.drawnProgress || p.bg != dot.drawnBg;
        if (redraw) {
            dotPic.px.assign(size_t(dotPic.w) * dotPic.h * 4, 0);
            DrawDot(dotPic, dotPic.w / 2.0, dotPic.h / 2.0, dotPic.w / dotPic.wDeg, dot.state, dot.progress, p.bg > 0.5 ? 0.0 : 1.0);
            dot.drawnState = dot.state, dot.drawnProgress = dot.progress, dot.drawnBg = p.bg;
            if (!noVr) dotBuffers.Present(ov, dotH, dotPic);
        }
        if (!noVr && (moved || dot.changed || redraw)) {
            std::lock_guard<std::mutex> lk(g_vr);
            if (absolute) ov->SetOverlayTransformAbsolute(dotH, vr::TrackingUniverseStanding, &m);
            else ov->SetOverlayTransformTrackedDeviceRelative(dotH, vr::k_unTrackedDeviceIndex_Hmd, &m);
            ov->SetOverlayWidthInMeters(dotH, float(2 * dist * std::tan(kDotDeg * M_PI / 360)));
            if (!dot.shown) ov->ShowOverlay(dotH);
        }
        dot.shown = true;
        if (targets.f && (moved || dot.changed || redraw)) {
            std::fprintf(targets.f, "{\"t\": %.6f, \"set\": %.6f, \"frame\": %llu, \"dot\": \"%s\", \"yaw\": %.4f, \"pitch\": %.4f, "
                                    "\"dist\": %.3f, \"state\": \"%s\", \"progress\": %.3f, \"bg\": %.3f}\n",
                         Sec(t), Sec(now), (unsigned long long)frame, dot.mode.c_str(), yaw, pitch, dist, dot.state.c_str(),
                         dot.progress, p.bg);
            ++targets.rows;
        }
        dot.changed = false;
    };
    std::fprintf(stderr, "ft-gazepanel running: @%s, %.2f m%s\n", sockName.c_str(), distance, noVr ? ", no SteamVR" : "");

    while (!g_stop) {
        char buf[512];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;  // from echo or socat
            buf[n] = 0;
            std::string reply = "ok";
            char word[16] = "", state[16] = "";
            double a = 0, b = 0, c = 0, d = 0, e = 0;
            int rest = 0;
            const char *arg = nullptr;
            if (!std::strncmp(buf, "show ", 5)) {
                p.rec = !std::strcmp(buf + 5, "rec");
                p.full = !std::strcmp(buf + 5, "full");
                p.fit = !std::strcmp(buf + 5, "fit");
                p.strip = !std::strcmp(buf + 5, "note");
                const bool five = !std::strcmp(buf + 5, "five"), big = p.full || p.rec;
                p.w = big ? kFullW : p.fit ? kFitW : five ? kFiveW : p.strip ? kNoteW : kQuickPx;
                p.h = big ? kFullH : p.fit ? kFitH : five ? kFiveH : p.strip ? kNoteH : kQuickPx;
                p.wDeg = big ? kFullDeg : p.fit ? kFitDeg : five ? kFiveDeg : p.strip ? kNoteDeg : kQuickDeg;
                p.title.clear(), p.text.clear(), p.note.clear(), p.dotOn = false, p.state = "off";
                p.eyes[0] = p.eyes[1] = EyeCard{}, p.hints.clear();
                p.step.clear(), p.big.clear(), p.action.clear(), p.keys.clear();
                p.recOn = p.paused = false, p.countdown = -1;
                dot.mode = "off", dot.changed = true;
                place();
                visible = dirty = true;  // shown with its first picture
            } else if (!std::strcmp(buf, "hide")) {
                if (!noVr) {
                    std::lock_guard<std::mutex> lk(g_vr);
                    ov->HideOverlay(h);
                }
                hideDot();
                visible = shown = false;
                dot.mode = "off", dot.changed = true;
            } else if (std::sscanf(buf, "bg %lf", &a) == 1) {
                p.bg = std::clamp(a, 0.0, 1.0), dirty = true, dot.changed = true;
            } else if (std::sscanf(buf, "dot %lf %lf %15s %lf", &a, &b, state, &c) >= 3) {
                p.dotYaw = a, p.dotPitch = b, p.state = state, p.progress = c;
                p.dotOn = std::strcmp(state, "off") != 0;
                if (p.rec) {
                    dot.mode = p.dotOn ? "fixed" : "off", dot.yaw = a, dot.pitch = b, dot.state = state, dot.progress = c;
                    dot.changed = true;
                } else {
                    dirty = true;
                }
            } else if (Is(buf, "path", &arg)) {
                char kind[16] = "";
                double v[5] = {0, 0, 0, 0, 0};
                const int got = std::sscanf(arg, "%15s %lf %lf %lf %lf %lf", kind, &v[0], &v[1], &v[2], &v[3], &v[4]);
                const int want = !std::strcmp(kind, "circle") ? 5 : (!std::strcmp(kind, "line") || !std::strcmp(kind, "eight")) ? 6 : 0;
                if (!p.rec) {
                    reply = "error path is for show rec";
                } else if (!want || got != want || v[want - 2] <= 0) {
                    reply = "error path circle|line|eight <numbers> <seconds>";
                } else {
                    dot.mode = "path", dot.kind = kind, dot.seconds = v[want - 2], dot.start = -1;
                    for (int k = 0; k < 4; ++k) dot.a[k] = k < want - 2 ? v[k] : 0;
                    dot.state = "look", dot.progress = 0, dot.changed = true;
                }
            } else if (Is(buf, "world", &arg)) {
                double v[3];
                char st[16] = "look";
                double prog = 0;
                if (!std::strcmp(arg, "off")) {
                    if (dot.mode == "world") dot.mode = "off", dot.changed = true;
                } else if (!p.rec) {
                    reply = "error world is for show rec";
                } else if (std::sscanf(arg, "%lf %lf %lf %15s %lf", &v[0], &v[1], &v[2], st, &prog) < 3) {
                    reply = "error world <x> <y> <z> [<state> [<progress>]]";
                } else {
                    const vr::TrackedDevicePose_t pose = HeadPose(noVr, 0);
                    if (!pose.bPoseIsValid) {
                        reply = "error no headset pose";
                    } else {
                        if (dot.mode != "world" || v[0] != dot.head[0] || v[1] != dot.head[1] || v[2] != dot.head[2])
                            ToRoom(pose.mDeviceToAbsoluteTracking, v, dot.room);
                        std::copy(v, v + 3, dot.head);
                        dot.mode = "world", dot.state = st, dot.progress = prog, dot.changed = true;
                        char r[96];
                        std::snprintf(r, sizeof r, "ok %.4f %.4f %.4f", dot.room[0], dot.room[1], dot.room[2]);
                        reply = r;
                    }
                }
            } else if (std::sscanf(buf, "distance %lf", &a) == 1) {
                distance = std::clamp(a, 0.25, 10.0), dot.changed = true;
                place();
            } else if (Is(buf, "ipd")) {
                float v = 0.063f;
                vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
                if (!noVr) {
                    std::lock_guard<std::mutex> lk(g_vr);
                    v = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe);
                }
                char r[48];
                std::snprintf(r, sizeof r, "ok %.5f", v);
                reply = pe == vr::TrackedProp_Success ? r : "error no IPD from SteamVR";
            } else if (Is(buf, "log", &arg)) {
                if (!std::strncmp(arg, "start ", 6)) {
                    const std::string dir = arg + 6;
                    if (!targets.Open(dir + "/targets.jsonl") || !ipdLog.Open(dir + "/ipd.jsonl") ||
                        !poses.Start(dir + "/poses.jsonl", !noVr)) {
                        targets.Close(), ipdLog.Close(), poses.Stop();
                        reply = "error can't write in " + dir;
                    } else {
                        ipdNow = -1, dot.changed = true;
                        ipdLook(RawNs(), "start");
                    }
                } else if (!std::strcmp(arg, "stop")) {
                    char r[96];
                    const long tr = targets.Close(), ir = ipdLog.Close(), pr = poses.Stop();
                    std::snprintf(r, sizeof r, "ok %ld %ld %ld", tr, pr, ir);
                    reply = r;
                } else {
                    reply = "error log start <dir> | log stop";
                }
            } else if (int k; std::sscanf(buf, "eye %d %lf %lf %lf %lf %lf %n", &k, &a, &b, &c, &d, &e, &rest) >= 6 &&
                       rest > 0 && (k == 0 || k == 1)) {
                p.eyes[k] = EyeCard{buf + rest, a, b, c, d, e}, dirty = true;
            } else if (!std::strncmp(buf, "hints", 5)) {
                p.hints.clear();
                std::string s = buf[5] == ' ' ? buf + 6 : "";
                for (size_t at = 0; !s.empty() && at <= s.size();) {
                    const size_t bar = std::min(s.find('|', at), s.size());
                    p.hints.push_back(s.substr(at, bar - at));
                    at = bar + 1;
                }
                dirty = true;
            } else if (Is(buf, "title", &arg)) {
                p.title = arg, dirty = true;
            } else if (Is(buf, "text", &arg)) {
                p.text = arg, dirty = true;
            } else if (Is(buf, "note", &arg)) {
                p.note = arg, dirty = true;
            } else if (Is(buf, "step", &arg)) {
                p.step = arg, dirty = true;
            } else if (Is(buf, "big", &arg)) {
                p.big = arg, dirty = true;
            } else if (Is(buf, "action", &arg)) {
                p.action = arg, dirty = true;
            } else if (Is(buf, "keys", &arg)) {
                p.keys = arg, dirty = true;
            } else if (Is(buf, "rec", &arg) && (!std::strcmp(arg, "on") || !std::strcmp(arg, "off"))) {
                p.recOn = !std::strcmp(arg, "on"), dirty = true;
            } else if (Is(buf, "paused", &arg) && (!std::strcmp(arg, "on") || !std::strcmp(arg, "off"))) {
                p.paused = !std::strcmp(arg, "on"), dirty = true;
            } else if (Is(buf, "countdown", &arg)) {
                p.countdown = !std::strcmp(arg, "off") ? -1 : std::clamp(std::atof(arg), 0.0, 1.0), dirty = true;
            } else if (std::sscanf(buf, "%15s", word) == 1 && !std::strcmp(word, "ping")) {
                reply = visible ? "ok shown" : "ok hidden";
            } else {
                reply = "error unknown command";
            }
            if (fromLen > offsetof(sockaddr_un, sun_path))
                sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
        if (!noVr) {
            vr::VREvent_t ev;
            std::unique_lock<std::mutex> lk(g_vr);
            while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
                if (ev.eventType == vr::VREvent_Quit) {
                    vr::VRSystem()->AcknowledgeQuit_Exiting();
                    g_stop = true;
                } else if (ev.eventType == vr::VREvent_IpdChanged && ipdLog.f) {
                    ipdNow = ev.data.ipd.ipdMeters;
                    std::fprintf(ipdLog.f, "{\"t\": %.6f, \"ipd\": %.5f, \"from\": \"event\"}\n", Sec(RawNs()), ipdNow);
                    ++ipdLog.rows;
                }
            }
        }
        if (visible && dirty) {
            Draw(p);
            ++pictures;
            if (!dumpDir.empty()) Dump(p, dumpDir + "/" + std::to_string(pictures) + ".pam");
            if (!noVr) {
                buffers.Present(ov, h, p);
                std::lock_guard<std::mutex> lk(g_vr);
                if (!shown) ov->ShowOverlay(h);
            }
            shown = true;
            dirty = false;
        }
        const int64_t now = RawNs();
        dotTick(now);
        if (ipdLog.f && now - ipdAt >= kIpdPeriodNs) ipdLook(now, "look");
        // Until a command comes, or the next frame while the rec mode's dot moves, or 10 ms while
        // shown (SteamVR's events). Hidden, it waits up to a second: it woke 20 to 30 times a
        // second for nothing, the main cost left with gaze idle. A closed stdin (--watch-stdin)
        // wakes it too, so quitting doesn't wait.
        double wait = visible ? 0.010 : 1.0;
        if (visible && p.rec && (dot.mode == "path" || dot.mode == "world")) {
            uint64_t frame;
            // Just after the next vsync: the compositor has taken this frame's transform.
            wait = std::max(0.001, Sec(nextFrame(now, frame) - now) - toPhotons + 0.001);
        }
        if (ipdLog.f) wait = std::min(wait, Sec(kIpdPeriodNs));
        pollfd fds[2] = {{sock, POLLIN, 0}, {0, POLLIN, 0}};
        const timespec ts{time_t(wait), long((wait - std::floor(wait)) * 1e9)};
        ppoll(fds, watchStdin ? 2 : 1, &ts, nullptr);
    }
    targets.Close(), ipdLog.Close(), poses.Stop();
    if (!noVr) {
        std::lock_guard<std::mutex> lk(g_vr);
        ov->DestroyOverlay(dotH);
        ov->DestroyOverlay(h);
    }
    buffers.Drop(), dotBuffers.Drop();
    if (!noVr) vr::VR_Shutdown();
    return 0;
}
