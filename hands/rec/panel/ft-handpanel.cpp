// ft-handpanel: the hand recorder's headset panel (hands/rec/DESIGN.md). A SteamVR overlay
// fixed to the headset that tells the person what to do with their hands while the cameras
// record. It sits --distance ahead (1.2 m), centred UP_DEG above straight ahead, so the hands
// stay clear below it, WIDTH_DEG across (4:3), dim and see-through like the gaze quick check.
// It's drawn on the CPU and takes no input: the session runner (hands/rec/session.py) drives it.
//
// It also does the two jobs that need SteamVR's poses: the "touch the dot" target, a second
// overlay (frametop.handpanel.target) fixed in the room, and the pose log, the head and the
// controllers 250 times a second into poses.jsonl, which goes with each take.
//
// Control socket: abstract unix datagram "@ft_handpanel" (--socket NAME); a sender with an
// address gets "ok ..." or "error ...". UTF-8; "|" starts a new line in text:
//   show / hide                  the panel (a "show" makes it visible with its first picture;
//                                the target is separate: "target off" hides it)
//   title <text>                 the big line at the top (empty to clear)
//   step <text>                  a small line at the top right, e.g. "Section 3 of 11"
//   text <text>                  the instruction: large, wrapped to the panel's width, centred
//   note <text>                  an orange warning line under the instruction (empty to clear)
//   countdown <0..1> | off       a thin bar along the bottom: the share of the prompt's time left
//   split <at> <label>;... / split off
//                                ticks on the countdown's bar where a step changes pose, each at
//                                <at> (the share of time left then, 0..1) with "Switch to <label>"
//                                above it; lit cyan once the bar has come down to it
//   hands <left> <right>         the "Left hand" and "Right hand" chips: seen (green), lost
//                                (orange) or off (hidden)
//   bar <target 0..1> <current 0..1|-1> [<near label>|<far label>] / bar off
//                                the near/far bar: a track with a ring at the target and a dot
//                                at the hand's position (-1: not seen). The labels are split at
//                                "|", or else at the first space (default "Near" and "Far")
//   paused on|off                "Paused" over the picture
//   image <path> [mirror|both] / image off
//                                the pose picture (a PNG, square, RGBA, a right hand as the wearer
//                                sees it) in a column on the left, the text moving right: mirror
//                                flips it (a left hand), both shows a flipped copy on its left
//   demo <path> [mirror] [nokey] / demo restart / demo off
//                                a demo clip (an animated GIF, hands/rec/demos) playing in a wider
//                                column on the left, looped, with the key to its colours under it
//                                (light: palm, dark: back of hand; nokey: none, for a clip of
//                                real hands from hands/rec/demos-recorded); it takes the place of the
//                                image. A new path starts it from its first frame, the same one
//                                again leaves it playing (mirror only flips it), restart starts
//                                it again. It stands still while paused
//   where <position|-> <distance|-> / where off
//                                a small diagram under the picture: where to hold the hands, a
//                                front view (centre, left, right, up, down; or the bar's chest,
//                                desk, eye), and how far out (near, mid, far)
//   action <text>                a cyan line under the instruction: "Ready? Press Space ..."
//   big <text>                   large, under the instruction: the countdown's 3, 2, 1, then "Hold"
//   keys <text>                  a small faint line along the bottom: the window's keys
//   rec on|off                   a red "Rec" dot by the step line while recording
//   strip <cue> <path>|<mode>|<label>;... / strip off
//                                a sweep's row of pose pictures under the text (path "-": none,
//                                mode "-" or "mirror"), each with its label; the one at index
//                                cue (from 0; -1: none) lit, framed in cyan, the others dim. The
//                                where-to diagram, if any, goes at the row's right end. It takes
//                                the place of the left column (image is ignored meanwhile)
//   target <x> <y> <z> [show|hold <0..1>|done] / target off
//                                the touch target, about 2 cm across. The point is in the head
//                                frame (metres, +x right, +y up, -z forward). The first command
//                                with a new point places it in the room with the headset's pose
//                                at that moment, facing the headset, and it stays there; the
//                                same point again changes only the state. hold draws a ring
//                                filling to the share given, done turns it green. "target off"
//                                hides it and forgets the point. Reply: "ok <room x> <y> <z>"
//                                (standing universe)
//   layout full|top              full: the panel as above. top: only a band along its top (title,
//                                step, Rec, big, two lines of text, a note, the time-left bar),
//                                the rest clear, so a scene's windows below it can be seen
//   win <id> place <x> <y> <z> <w> <h> [<yaw> <pitch>] [<look>]
//                                a scene's window (at most 4; id: a short name): w x h metres,
//                                centred at x y z in the head frame made level (the head's forward
//                                direction, flattened; +y up), facing the eye, turned by yaw and
//                                pitch degrees. It stays where it was put in the room. look: plain
//                                (faint text lines), grid:CxR (1-6 buttons each way) or list:N
//                                (1-10 rows). Its own overlay, frametop.handpanel.win.<id>, made
//                                at first use. Reply: "ok <12 floats>", its pose in the room
//                                (standing universe, row-major 3x4)
//   win <id> lit <part>[+<part>...] [<colour>] [<label>]
//                                light parts of it: none, bar (the title bar), left, right, top,
//                                bottom (edges), tl, tr, bl, br (corners), button:N or item:N
//                                (from 0). colour: cyan (default), purple, orange (a button drawn
//                                hollow with a cross: don't touch) or green. label: a short mark
//                                beside the first part ("_" for a space)
//   win <id> ghost <dx> <dy> <dz> <yaw> <pitch> <roll> [<scale>] / win <id> ghost off
//                                a dashed outline of the window, moved and turned in the window's
//                                own frame and scaled: where to take it. Reply: "ok <12 floats>"
//   win <id> off / win clear     hide one window (and its ghost) / all of them
//   poses start <path> / poses stop
//                                log poses, appending to path (poses.jsonl, DESIGN.md) at
//                                250 Hz until stopped. "poses stop" replies "ok <samples>"
//   devices                      "ok hmd <r> left <r|-> right <r|->": ETrackingResult now
//                                (- for no device in that role)
//   head                         "ok <12 floats>": the headset's pose now (standing universe,
//                                row-major 3x4)
//   ping                         "ok shown" or "ok hidden"
//
// Each picture goes into the next of three shared buffers (linear DMA-BUFs SteamVR imported
// once), and the overlay switches to it, as gaze/panel/ft-gazepanel.cpp does: SetOverlayRaw
// flickered and lagged there, so it's only the fallback. The target has its own three small
// buffers, and so does each window and ghost (made again only when its size changes). Pictures are drawn only when a command changed something; while a demo clip plays,
// each of its frames redraws only the clip's rectangle, and copies only that to a buffer that
// already has the rest.
//
// The pose log runs on a thread of its own, so a slow picture or command never delays a
// sample. OpenVR's client isn't documented as thread-safe, so both threads take g_vr around
// every OpenVR call.
//
// Options: --watch-stdin (quit when stdin closes: the session runs it), --socket NAME,
// --distance METRES, --up DEG and --width DEG (UP_DEG and WIDTH_DEG: raise it out of a
// recorded headset view), --no-vr. --no-vr is for testing without a headset: no SteamVR at all; each
// picture's text goes to stdout instead (and with --dump DIR, the pictures to DIR/panel.pam,
// DIR/target.pam, DIR/win-<id>.pam and DIR/ghost-<id>.pam), the windows are placed as if the
// headset were at the room's origin, "poses start" writes nothing, "devices" and "head" reply with an error,
// and "target" places the point as if the headset were at the room's origin.
// Runs in the dev container (hands/rec/build.sh builds it into hands/rec/build).
#include <openvr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // helpers only other formats use
#include "stb_image.h"
#pragma GCC diagnostic pop

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kWidthDeg = 36;  // WIDTH_DEG: the panel's width (4:3)
constexpr double kUpDeg = 12;     // UP_DEG: the panel's centre above straight ahead
constexpr int kW = 1024, kH = 768;
constexpr double kPxPerDeg = kW / kWidthDeg;
constexpr int kTargetPx = 128;
constexpr double kTargetM = 0.03;      // the target overlay's width: the dot and its ring
constexpr double kTargetDotM = 0.02;   // the dot itself
constexpr int64_t kPosePeriodNs = 4'000'000;  // 250 Hz
std::atomic<bool> g_stop{false};
std::mutex g_vr;  // around every OpenVR call: the main thread and the pose log's thread make them

int64_t NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// ---------------------------------------------------------------- text (as ft-gazepanel)

stbtt_fontinfo g_font;
std::vector<unsigned char> g_fontData;
bool g_fontOk = false;
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
    if (!g_fontOk) std::fprintf(stderr, "ft-handpanel: no font (%s); no text\n", path.c_str());
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

// s as lines no wider than width: "|" starts a new line, and lines break between words. A
// word wider than the panel gets a line of its own and runs over.
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

// ---------------------------------------------------------------- the pictures

struct Picture {
    int w, h;
    std::vector<uint8_t> px;  // R, G, B, A (straight alpha)
};

void Blend(Picture &p, int x, int y, double r, double g, double b, double a) {
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
void Disc(Picture &p, double cx, double cy, double outer, double inner, double r, double g, double b, double a,
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

void Rect(Picture &p, int x0, int y0, int x1, int y1, double r, double g, double b, double a) {
    for (int y = std::max(y0, 0); y < std::min(y1, p.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, p.w); ++x) Blend(p, x, y, r, g, b, a);
}

// A rectangle with rounded corners of radius rad, anti-aliased: the panel itself, the chips.
void RoundRect(Picture &p, int x0, int y0, int x1, int y1, double rad, double r, double g, double b, double a) {
    for (int y = std::max(y0, 0); y < std::min(y1, p.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, p.w); ++x) {
            const double dx = std::max({x0 + rad - x - 0.5, x + 0.5 - (x1 - rad), 0.0});
            const double dy = std::max({y0 + rad - y - 0.5, y + 0.5 - (y1 - rad), 0.0});
            Blend(p, x, y, r, g, b, a * std::clamp(rad - std::hypot(dx, dy) + 0.5, 0.0, 1.0));
        }
}

enum Align { kLeft, kCentre, kRight };

// Text vertically centred on cy; x is its left end, middle or right end.
void Text(Picture &p, const std::string &s, int size, int x, int cy, double r, double g, double b, Align align = kCentre) {
    if (!g_fontOk || s.empty()) return;
    const auto cps = Codepoints(s);
    int width = 0;
    for (uint32_t cp : cps) width += GetGlyph(cp, size).advance;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    if (align == kCentre) x -= width / 2;
    else if (align == kRight) x -= width;
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : cps) {
        const Glyph &gl = GetGlyph(cp, size);
        for (int gy = 0; gy < gl.h; ++gy)
            for (int gx = 0; gx < gl.w; ++gx)
                Blend(p, x + gl.xoff + gx, baseline + gl.yoff + gy, r, g, b, gl.bitmap[size_t(gy) * gl.w + gx] / 255.0);
        x += gl.advance;
    }
}

void Text(Picture &p, const std::string &s, int size, int x, int cy, double lum, Align align = kCentre) {
    Text(p, s, size, x, cy, lum, lum, lum, align);
}

// A pose picture as loaded (straight alpha), and scaled copies of it as drawn.
struct Image {
    std::string path;
    Picture src{0, 0, {}};
    std::map<std::pair<int, bool>, Picture> scaled;  // (side, mirrored) -> picture
};

// A PNG into img; false (and img empty) if it can't be read.
bool LoadImage(Image &img, const std::string &path) {
    img = Image{};
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return false;
    img.path = path;
    img.src = Picture{w, h, std::vector<uint8_t>(px, px + size_t(w) * h * 4)};
    stbi_image_free(px);
    return true;
}

// The picture fitted into a side x side square (centred, aspect kept), averaging the source
// pixels each output pixel covers, weighted by alpha so transparent edges don't darken.
const Picture &Scaled(Image &img, int side, bool mirror) {
    auto [it, fresh] = img.scaled.try_emplace({side, mirror}, Picture{side, side, {}});
    Picture &out = it->second;
    if (!fresh) return out;
    out.px.assign(size_t(side) * side * 4, 0);
    const Picture &src = img.src;
    const double scale = std::max(src.w, src.h) / double(side);  // source pixels per output pixel
    const double ox = (side - src.w / scale) / 2, oy = (side - src.h / scale) / 2;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const double sx0 = (x - ox) * scale, sy0 = (y - oy) * scale;
            const int x0 = std::max(0, int(std::floor(sx0))), y0 = std::max(0, int(std::floor(sy0)));
            const int x1 = std::min(src.w, int(std::ceil(sx0 + scale))), y1 = std::min(src.h, int(std::ceil(sy0 + scale)));
            double r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t *q = &src.px[(size_t(sy) * src.w + (mirror ? src.w - 1 - sx : sx)) * 4];
                    const double qa = q[3] / 255.0;
                    r += q[0] * qa, g += q[1] * qa, b += q[2] * qa, a += qa, ++count;
                }
            if (!count || a <= 0) continue;
            uint8_t *o = &out.px[(size_t(y) * side + x) * 4];
            o[0] = uint8_t(std::lround(r / a)), o[1] = uint8_t(std::lround(g / a)), o[2] = uint8_t(std::lround(b / a));
            o[3] = uint8_t(std::lround(255 * a / count));
        }
    return out;
}

void DrawPicture(Picture &dst, const Picture &src, int x0, int y0, double opacity = 1) {
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const uint8_t *q = &src.px[(size_t(y) * src.w + x) * 4];
            if (q[3]) Blend(dst, x0 + x, y0 + y, q[0] / 255.0, q[1] / 255.0, q[2] / 255.0, opacity * q[3] / 255.0);
        }
}

// A sweep's strip: its pictures, loaded once each (a strip changes only its lit one as it goes).
struct StripItem {
    std::string path, mode, label;
};
std::map<std::string, Image> g_stripImages;  // path -> picture (empty if it can't be read)

Image &StripImage(const std::string &path) {
    auto [it, fresh] = g_stripImages.try_emplace(path);
    if (fresh && !LoadImage(it->second, path))
        std::fprintf(stderr, "ft-handpanel: can't read %s: %s\n", path.c_str(), stbi_failure_reason());
    return it->second;
}

// The where-to diagram's cells: a front view, 3 x 3, (column, row) from the top left.
// A demo clip: an animated GIF decoded a frame at a time as it plays, so a long clip holds
// three frames in memory, not all of them. stb_image keeps its GIF decoder to itself, but this
// file compiles it, so it steps stbi__gif_load_next as stbi_load_gif_from_memory does.
struct Demo {
    std::string path;
    bool mirror = false;
    bool key = true;                  // the key to the skeleton clips' colours under it
    std::vector<uint8_t> file;
    stbi__context ctx{};
    stbi__gif g{};
    int w = 0, h = 0, frame = -1;     // frame: from 0 at the clip's start
    int loops = 0;                    // times it went back to the start by itself
    std::vector<uint8_t> cur, prev;   // this frame and the one before (GIF's "restore to previous")
    int64_t due = 0;                  // when the next frame shows (NowNs)
    Picture scaled{0, 0, {}};         // this frame as drawn
    bool fresh = false;               // scaled is out of date

    Demo() = default;
    Demo(const Demo &) = delete;
    Demo &operator=(const Demo &) = delete;
    ~Demo() { Free(); }

    void Free() {
        STBI_FREE(g.out), STBI_FREE(g.history), STBI_FREE(g.background);
        std::memset(&g, 0, sizeof g);
    }

    bool Load(const std::string &p) {
        FILE *f = std::fopen(p.c_str(), "rb");
        if (!f) return stbi__err("can't open file", "can't open file"), false;
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
        path = p;
        return Start(NowNs());
    }

    // From the first frame again.
    bool Start(int64_t now) {
        Free();
        stbi__start_mem(&ctx, file.data(), int(file.size()));
        cur.clear(), prev.clear(), frame = -1;
        return Next(now, false);
    }

    // The next frame into cur, the first again after the last. false: the file is no GIF, or broken.
    bool Next(int64_t now, bool loop = true) {
        int comp = 0;
        stbi_uc *u = stbi__gif_load_next(&ctx, &g, &comp, 4, prev.empty() ? nullptr : prev.data());
        if (u == reinterpret_cast<stbi_uc *>(&ctx)) return loop && frame >= 0 && (++loops, Start(now));  // the end
        if (!u) return false;
        w = g.w, h = g.h, ++frame;
        prev.swap(cur);
        cur.assign(u, u + size_t(w) * h * 4);
        // As browsers: a delay under 20 ms is 100 ms.
        due = now + int64_t(g.delay >= 20 ? g.delay : 100) * 1'000'000;
        fresh = true;
        return true;
    }
};

// The demo's frame drawn opaque at x0, y0 in dw x dh, averaging the source pixels each output
// pixel covers (the frame is scaled again only when it or the size changed).
void DrawDemo(Picture &pic, Demo &d, int x0, int y0, int dw, int dh) {
    if (d.cur.empty() || dw <= 0 || dh <= 0) return;
    if (d.fresh || d.scaled.w != dw || d.scaled.h != dh) {
        d.scaled = Picture{dw, dh, std::vector<uint8_t>(size_t(dw) * dh * 4)};
        const double sx = double(d.w) / dw, sy = double(d.h) / dh;
        for (int y = 0; y < dh; ++y) {
            const int ya = std::min(d.h - 1, int(y * sy)), yb = std::clamp(int(std::ceil((y + 1) * sy)), ya + 1, d.h);
            for (int x = 0; x < dw; ++x) {
                const int xa = std::min(d.w - 1, int(x * sx)), xb = std::clamp(int(std::ceil((x + 1) * sx)), xa + 1, d.w);
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int yy = ya; yy < yb; ++yy)
                    for (int xx = xa; xx < xb; ++xx) {
                        const uint8_t *q = &d.cur[(size_t(yy) * d.w + (d.mirror ? d.w - 1 - xx : xx)) * 4];
                        r += q[0], g += q[1], b += q[2], ++n;
                    }
                uint8_t *o = &d.scaled.px[(size_t(y) * dw + x) * 4];
                o[0] = uint8_t(r / n), o[1] = uint8_t(g / n), o[2] = uint8_t(b / n), o[3] = 255;
            }
        }
        d.fresh = false;
    }
    for (int y = 0; y < dh; ++y) {
        if (y0 + y < 0 || y0 + y >= pic.h) continue;
        const int xa = std::max(0, -x0), xb = std::min(dw, pic.w - x0);
        if (xb > xa)
            std::memcpy(&pic.px[(size_t(y0 + y) * pic.w + x0 + xa) * 4], &d.scaled.px[(size_t(y) * dw + xa) * 4],
                        size_t(xb - xa) * 4);
    }
}

bool WhereCell(const std::string &pos, int &col, int &row) {
    col = 1, row = 1;
    if (pos == "left") col = 0;
    else if (pos == "right") col = 2;
    else if (pos == "up" || pos == "eye") row = 0;
    else if (pos == "down" || pos == "desk") row = 2;
    else if (pos != "centre" && pos != "center" && pos != "chest") return false;
    return true;
}

const char *WhereLabel(const std::string &pos) {
    if (pos == "left") return "To your left";
    if (pos == "right") return "To your right";
    if (pos == "up") return "Up high";
    if (pos == "down") return "Down low";
    if (pos == "chest") return "Chest height";
    if (pos == "desk") return "Desk height";
    if (pos == "eye") return "Eye level";
    return "In front";
}

int DistanceStep(const std::string &dist) { return dist == "near" ? 0 : dist == "mid" ? 1 : dist == "far" ? 2 : -1; }

const char *DistanceLabel(int k) { return k == 0 ? "Close" : k == 1 ? "Halfway out" : "Arm out"; }

constexpr double kCyan[3] = {0.3, 0.85, 1.0};
// The demo clips' palm and back of the hand (hands/rec/demos): the key's swatches.
constexpr double kPalm[3] = {0xd5 / 255.0, 0xd1 / 255.0, 0xfe / 255.0};
constexpr double kBack[3] = {0x6a / 255.0, 0x62 / 255.0, 0xd8 / 255.0};

// The diagram in x0..x0+w, y0..y0+h: the front view on the left, the distance on the right
// (or either alone, centred), each with its caption under it.
void DrawWhere(Picture &pic, int x0, int y0, int w, int h, const std::string &pos, const std::string &dist) {
    const double ppd = kPxPerDeg;
    int col, row;
    const bool grid = WhereCell(pos, col, row);
    const int step = DistanceStep(dist);
    const int parts = int(grid) + int(step >= 0);
    if (!parts) return;
    const int capSize = int(ppd * 0.75), capH = int(capSize * 1.6), partW = w / parts;
    const int boxH = h - capH;
    int px = x0;
    if (grid) {
        const int gw = std::min(partW - int(ppd * 0.6), boxH * 4 / 3), gh = gw * 3 / 4;
        const int gx = px + (partW - gw) / 2, gy = y0 + (boxH - gh) / 2;
        RoundRect(pic, gx, gy, gx + gw, gy + gh, ppd * 0.3, 1, 1, 1, 0.08);
        for (int k = 1; k < 3; ++k) {
            Rect(pic, gx + gw * k / 3 - 1, gy + 3, gx + gw * k / 3 + 1, gy + gh - 3, 1, 1, 1, 0.18);
            Rect(pic, gx + 3, gy + gh * k / 3 - 1, gx + gw - 3, gy + gh * k / 3 + 1, 1, 1, 1, 0.18);
        }
        const int pad = std::max(3, int(ppd * 0.12));
        RoundRect(pic, gx + gw * col / 3 + pad, gy + gh * row / 3 + pad, gx + gw * (col + 1) / 3 - pad,
                  gy + gh * (row + 1) / 3 - pad, ppd * 0.18, kCyan[0], kCyan[1], kCyan[2], 0.9);
        Text(pic, WhereLabel(pos), capSize, px + partW / 2, y0 + boxH + capH / 2, 0.85);
        px += partW;
    }
    if (step >= 0) {
        // Seen from the side: the head on the left, the arm's reach to the right, three marks.
        const int cy = y0 + boxH / 2, head = int(ppd * 0.55);
        const int hx = px + int(ppd * 0.4) + head, ax0 = hx + head + int(ppd * 0.3), ax1 = px + partW - int(ppd * 0.5);
        Disc(pic, hx, cy, head, 0, 1, 1, 1, 0.75);
        RoundRect(pic, hx + head / 3, cy - head / 2, hx + head + int(ppd * 0.2), cy + head / 3, 3, 0.25, 0.25, 0.28, 1);
        Rect(pic, ax0, cy - 1, ax1, cy + 1, 1, 1, 1, 0.3);
        for (int k = 0; k < 3; ++k) {
            const double x = ax0 + (ax1 - ax0) * (k + 1) / 3.0;
            if (k == step) Disc(pic, x, cy, ppd * 0.38, 0, kCyan[0], kCyan[1], kCyan[2], 0.95);
            else Disc(pic, x, cy, ppd * 0.16, 0, 1, 1, 1, 0.45);
        }
        Text(pic, DistanceLabel(step), capSize, px + partW / 2, y0 + boxH + capH / 2, 0.85);
    }
}

struct Panel {
    Picture pic{kW, kH, {}};
    std::string title, step, text, note, action, big, keys;
    double countdown = -1;  // the share of the prompt's time left, or -1: off
    std::vector<std::pair<double, std::string>> splits;  // the countdown's ticks: share left, the pose after
    std::string hands[2] = {"off", "off"};
    bool barOn = false;
    double barTarget = 0, barCurrent = -1;
    std::string nearLabel = "Near", farLabel = "Far";
    bool paused = false, rec = false;
    bool compact = false;            // layout top: only a band at the top, clear of a scene's windows
    Image image;                     // the pose picture, if image.path isn't empty
    std::string imageMode;           // "", "mirror" or "both"
    std::string wherePos, whereDist; // the diagram's, "" for none
    std::vector<StripItem> strip;    // a sweep's pictures, or none
    int stripCue = -1;               // the lit one
    std::unique_ptr<Demo> demo;      // the demo clip, or none
    int demoX = 0, demoY = 0, demoW = 0, demoH = 0;  // where the last Draw put it (demoW 0: nowhere)
    // What the last Draw laid out, for --no-vr's printout.
    std::vector<std::string> lines, noteLines;
    double textDeg = 0;
};

// The countdown's ticks (a step's pose changes): a line across the bar at each, "Switch to ..."
// over it, kept inside the bar's span; cyan once the bar has come down to it. labelY is the
// label's centre line.
void DrawSplits(Picture &pic, const Panel &p, int x0, int width, int y0, int y1, int labelY, int size) {
    const double ppd = kPxPerDeg;
    for (const auto &[at, label] : p.splits) {
        const bool reached = p.countdown >= 0 && p.countdown <= at + 1e-6;
        const int x = x0 + int(std::lround(width * at)), lw = std::max(3, int(ppd * 0.12));
        Rect(pic, x - lw / 2, y0 - int(ppd * 0.3), x - lw / 2 + lw, y1 + int(ppd * 0.15), 1, 1, 1, reached ? 0.95 : 0.7);
        const std::string text = "Switch to " + label;
        const int w = TextWidth(text, size);
        const int cx = std::clamp(x, x0 + w / 2, std::max(x0 + w / 2, x0 + width - w / 2));
        if (reached) Text(pic, text, size, cx, labelY, kCyan[0], kCyan[1], kCyan[2]);
        else Text(pic, text, size, cx, labelY, 0.85);
    }
}

void Draw(Panel &p) {
    // The dim rounded panel, see-through so it's clear of what's behind (the quick check's look,
    // with the same corner radius in degrees). It never changes, so it's drawn once.
    static Picture back{kW, kH, {}};
    if (back.px.empty()) {
        back.px.assign(size_t(kW) * kH * 4, 0);
        RoundRect(back, 0, 0, kW, kH, kPxPerDeg * 1.9, 0.06, 0.06, 0.07, 0.82);
    }
    Picture &pic = p.pic;
    pic.px = back.px;
    p.demoW = 0;
    const double ppd = kPxPerDeg, faint = 0.75;
    const int margin = int(ppd * 1.6), width = kW - 2 * margin;
    if (p.compact) {
        // A scene's step: a band along the top with the cue, the hold's word and the time
        // left; the rest is clear, so the windows below show through nothing.
        pic.px.assign(size_t(kW) * kH * 4, 0);
        const int splitSize = int(ppd * 0.8), splitH = p.splits.empty() || p.countdown < 0 ? 0 : int(splitSize * 1.5);
        const int bandH = int(ppd * 6.2) + splitH, m = int(ppd * 0.9), stepSize = int(ppd * 0.8);
        RoundRect(pic, 0, 0, kW, bandH, ppd * 1.2, 0.06, 0.06, 0.07, 0.85);
        const int cy0 = m + stepSize / 2;
        Text(pic, p.step, stepSize, kW - m, cy0, faint, kRight);
        if (p.rec) {
            const int rx = kW - m - TextWidth(p.step, stepSize) - (p.step.empty() ? 0 : int(ppd * 0.6));
            Text(pic, "Rec", stepSize, rx, cy0, 1.0, 0.45, 0.4, kRight);
            Disc(pic, rx - TextWidth("Rec", stepSize) - ppd * 0.4, cy0, ppd * 0.22, 0, 0.95, 0.2, 0.15, 1);
        }
        Text(pic, p.title, stepSize, m, cy0, faint, kLeft);
        const int bigW = p.big.empty() ? 0 : int(ppd * 6), size = int(ppd * 1.15), lh = int(size * 1.25);
        const int x0 = m + bigW, x1 = kW - m, noteSize = int(ppd * 0.95);
        p.lines = Wrap(p.text, size, x1 - x0);
        if (p.lines.size() > 2) p.lines.resize(2);
        p.noteLines = Wrap(p.note, noteSize, x1 - x0);
        if (p.noteLines.size() > 1) p.noteLines.resize(1);
        p.textDeg = 1.15;
        const int foot = bandH - m, block = int(p.lines.size()) * lh + int(p.noteLines.size()) * int(noteSize * 1.3);
        int y = cy0 + stepSize / 2 + (foot - int(ppd * 0.5) - splitH - cy0 - stepSize / 2 - block) / 2;
        if (!p.big.empty()) Text(pic, p.big, int(ppd * 2.2), m + bigW / 2, (cy0 + foot) / 2, kCyan[0], kCyan[1], kCyan[2]);
        for (const std::string &line : p.lines) Text(pic, line, size, (x0 + x1) / 2, y + lh / 2, 1.0), y += lh;
        for (const std::string &line : p.noteLines)
            Text(pic, line, noteSize, (x0 + x1) / 2, y + int(noteSize * 0.65), 1.0, 0.62, 0.3), y += int(noteSize * 1.3);
        if (p.countdown >= 0) {
            const int th = std::max(4, int(ppd * 0.18));
            Rect(pic, m, foot - th, kW - m, foot, 1, 1, 1, 0.15);
            Rect(pic, m, foot - th, m + int(std::lround((kW - 2 * m) * p.countdown)), foot, kCyan[0], kCyan[1], kCyan[2], 0.95);
            if (splitH) DrawSplits(pic, p, m, kW - 2 * m, foot - th, foot, foot - th - int(ppd * 0.3) - splitH / 2, splitSize);
        }
        if (p.paused) {
            const int size = int(ppd * 1.6);
            RoundRect(pic, 0, 0, kW, bandH, ppd * 1.2, 0.02, 0.02, 0.03, 0.8);
            Text(pic, "Paused", size, kW / 2, bandH / 2, 1.0);
        }
        return;
    }

    // The title and the step share the top line, a red "Rec" by the step while recording; a
    // faint rule under them.
    int top = margin;
    if (!p.title.empty() || !p.step.empty() || p.rec) {
        const int titleSize = int(ppd * 1.6), stepSize = int(ppd * 0.9), cy = margin + titleSize / 2;
        Text(pic, p.step, stepSize, kW - margin, cy, faint, kRight);
        if (p.rec) {
            const int rx = kW - margin - TextWidth(p.step, stepSize) - (p.step.empty() ? 0 : int(ppd * 0.8));
            Text(pic, "Rec", stepSize, rx, cy, 1.0, 0.45, 0.4, kRight);
            Disc(pic, rx - TextWidth("Rec", stepSize) - ppd * 0.45, cy, ppd * 0.25, 0, 0.95, 0.2, 0.15, 1);
        }
        Text(pic, p.title, titleSize, margin, cy, 1.0, kLeft);
        top = cy + int(titleSize * 0.85);
        Rect(pic, margin, top, kW - margin, top + 2, 1, 1, 1, 0.15);
        top += int(ppd * 0.8);
    }

    // From the bottom up: the keys, the countdown, the chips, the near/far bar. The rest gets what's left.
    int bottom = kH - margin, foot = kH - int(ppd * 0.9);
    if (!p.keys.empty()) {
        Text(pic, p.keys, int(ppd * 0.7), kW / 2, foot, 0.55);
        foot -= int(ppd * 1.0);
        bottom = std::min(bottom, foot - int(ppd * 0.4));
    }
    if (p.countdown >= 0) {
        const int y = foot, th = std::max(4, int(ppd * 0.22));
        Rect(pic, margin, y, kW - margin, y + th, 1, 1, 1, 0.15);
        Rect(pic, margin, y, margin + int(std::lround(width * p.countdown)), y + th, kCyan[0], kCyan[1], kCyan[2], 0.95);
        bottom = std::min(bottom, y - int(ppd * 0.6));
        if (!p.splits.empty()) {
            const int size = int(ppd * 0.9), ly = y - int(ppd * 0.35) - size * 3 / 4;
            DrawSplits(pic, p, margin, width, y, y + th, ly, size);
            bottom = std::min(bottom, ly - size - int(ppd * 0.3));
        }
    }
    if (p.hands[0] != "off" || p.hands[1] != "off") {
        const int ch = int(ppd * 1.25), size = int(ppd * 0.85), cy = bottom - ch / 2, gap = int(ppd * 0.6);
        for (int k = 0; k < 2; ++k) {
            if (p.hands[k] == "off") continue;
            const char *label = k ? "Right hand" : "Left hand";
            const int cw = TextWidth(label, size) + 2 * int(ppd * 0.8);
            const int x0 = k ? kW / 2 + gap / 2 : kW / 2 - gap / 2 - cw;
            if (p.hands[k] == "seen") RoundRect(pic, x0, cy - ch / 2, x0 + cw, cy + ch / 2, ch / 2.0, 0.2, 0.62, 0.32, 0.95);
            else RoundRect(pic, x0, cy - ch / 2, x0 + cw, cy + ch / 2, ch / 2.0, 0.95, 0.5, 0.12, 0.95);
            Text(pic, label, size, x0 + cw / 2, cy, 1.0);
        }
        bottom = cy - ch / 2 - int(ppd * 0.7);
    }
    if (p.barOn) {
        // The ring is where the hand should be, the dot where it is: put the dot in the ring.
        const int labelSize = int(ppd * 0.85), ly = bottom - labelSize / 2, ty = ly - int(ppd * 1.2);
        const int x0 = margin + int(ppd), x1 = kW - margin - int(ppd), th = std::max(6, int(ppd * 0.3));
        RoundRect(pic, x0, ty - th / 2, x1, ty + th / 2, th / 2.0, 1, 1, 1, 0.2);
        Text(pic, p.nearLabel, labelSize, x0, ly, faint, kLeft);
        Text(pic, p.farLabel, labelSize, x1, ly, faint, kRight);
        const double ring = ppd * 0.65;
        Disc(pic, x0 + (x1 - x0) * p.barTarget, ty, ring, ring - ppd * 0.14, 1, 1, 1, 0.95);
        if (p.barCurrent >= 0) Disc(pic, x0 + (x1 - x0) * p.barCurrent, ty, ppd * 0.4, 0, kCyan[0], kCyan[1], kCyan[2], 0.95);
        bottom = ty - int(ring) - int(ppd * 0.8);
    }

    // A sweep's strip: a row of pictures along the bottom of what's left, labels under them, the
    // diagram at its right end; the text goes above it.
    int textX0 = margin, textX1 = kW - margin;
    int col, row;
    const bool demoOn = p.demo && p.strip.empty();
    const bool imageOn = !p.image.path.empty() && p.strip.empty() && !demoOn;
    const bool whereOn = WhereCell(p.wherePos, col, row) || DistanceStep(p.whereDist) >= 0;
    if (!p.strip.empty()) {
        const int n = int(p.strip.size()), gap = int(ppd * 0.6), whereW = whereOn ? int(ppd * 9) : 0;
        const int labelSize = int(ppd * 0.85), labelH = int(labelSize * 1.6), frame = std::max(3, int(ppd * 0.14));
        const int rowW = width - (whereOn ? whereW + gap : 0);
        const int side = std::max(8, std::min({int(ppd * 6), (rowW - (n - 1) * gap) / n, (bottom - top) / 2 - labelH}));
        const int y0 = bottom - side - labelH, totalW = n * side + (n - 1) * gap;
        int x = margin + (rowW - totalW) / 2;
        for (int k = 0; k < n; ++k, x += side + gap) {
            const StripItem &it = p.strip[k];
            const bool lit = k == p.stripCue;
            RoundRect(pic, x - frame, y0 - frame, x + side + frame, y0 + side + frame, ppd * 0.5, kCyan[0], kCyan[1],
                      kCyan[2], lit ? 0.95 : 0);
            RoundRect(pic, x, y0, x + side, y0 + side, ppd * 0.4, 0.16, 0.16, 0.18, 1);
            Image *img = it.path.empty() ? nullptr : &StripImage(it.path);
            if (img && !img->path.empty()) DrawPicture(pic, Scaled(*img, side, it.mode == "mirror"), x, y0, lit ? 1.0 : 0.4);
            if (lit) Text(pic, it.label, labelSize, x + side / 2, y0 + side + labelH / 2 + frame, kCyan[0], kCyan[1], kCyan[2]);
            else Text(pic, it.label, labelSize, x + side / 2, y0 + side + labelH / 2 + frame, 0.6);
        }
        if (whereOn) DrawWhere(pic, kW - margin - whereW, y0 + (side + labelH - int(ppd * 4.4)) / 2, whereW, int(ppd * 4.4),
                               p.wherePos, p.whereDist);
        bottom = y0 - frame - int(ppd * 0.8);
    } else if (demoOn) {
        // The demo clip in a wider column, the key to its colours under it (on one line if it
        // fits; none for a clip of real hands), the text to the right.
        const int colW = int(ppd * 16), gap = int(ppd * 0.6), keySize = int(ppd * 0.75), keyLh = int(keySize * 1.6);
        const int sw = std::max(1, p.demo->w), sh = std::max(1, p.demo->h), swatch = keySize, sgap = int(ppd * 0.3);
        const std::string light = "Light: palm", dark = "Dark: back of hand";
        const int lightW = swatch + sgap + TextWidth(light, keySize), darkW = swatch + sgap + TextWidth(dark, keySize);
        const bool keyOn = p.demo->key, oneLine = lightW + int(ppd) + darkW <= colW;
        const int room = bottom - top - (keyOn ? gap + (oneLine ? 1 : 2) * keyLh : 0);
        int dw = colW, dh = dw * sh / sw;
        if (dh > room) dh = std::max(0, room), dw = dh * sw / sh;
        int y = top + std::max(0, (room - dh) / 2);
        const int x = margin + (colW - dw) / 2;
        RoundRect(pic, x - gap / 2, y - gap / 2, x + dw + gap / 2, y + dh + gap / 2, ppd * 0.6, 1, 1, 1, 0.05);
        DrawDemo(pic, *p.demo, x, y, dw, dh);
        p.demoX = x, p.demoY = y, p.demoW = dw, p.demoH = dh;
        y += dh + gap;
        auto key = [&](const std::string &label, const double *c, int kx, int cy) {
            RoundRect(pic, kx, cy - swatch / 2, kx + swatch, cy + swatch / 2, swatch * 0.25, c[0], c[1], c[2], 1);
            Text(pic, label, keySize, kx + swatch + sgap, cy, 0.85, kLeft);
        };
        if (keyOn && oneLine) {
            const int kx = margin + (colW - lightW - int(ppd) - darkW) / 2;
            key(light, kPalm, kx, y + keyLh / 2);
            key(dark, kBack, kx + lightW + int(ppd), y + keyLh / 2);
        } else if (keyOn) {
            const int kx = margin + (colW - std::max(lightW, darkW)) / 2;
            key(light, kPalm, kx, y + keyLh / 2);
            key(dark, kBack, kx, y + keyLh + keyLh / 2);
        }
        textX0 = margin + colW + int(ppd * 1.2);
    } else if (imageOn || whereOn) {
        const int colW = int(ppd * 14), gap = int(ppd * 0.6);
        const int whereH = whereOn ? int(ppd * 4.4) : 0, between = imageOn && whereOn ? gap : 0;
        const bool both = p.imageMode == "both";
        const int side = imageOn ? std::max(0, std::min(both ? (colW - gap) / 2 : colW, bottom - top - whereH - between)) : 0;
        int y = top + std::max(0, (bottom - top - side - between - whereH) / 2);
        if (side > 0) {
            const int pw = both ? 2 * side + gap : side, x = margin + (colW - pw) / 2;
            RoundRect(pic, x - gap / 2, y - gap / 2, x + pw + gap / 2, y + side + gap / 2, ppd * 0.6, 1, 1, 1, 0.05);
            if (both) {
                DrawPicture(pic, Scaled(p.image, side, true), x, y);
                DrawPicture(pic, Scaled(p.image, side, false), x + side + gap, y);
            } else {
                DrawPicture(pic, Scaled(p.image, side, p.imageMode == "mirror"), x, y);
            }
            y += side + between;
        }
        if (whereOn) DrawWhere(pic, margin, y, colW, whereH, p.wherePos, p.whereDist);
        textX0 = margin + colW + int(ppd * 1.2);
    }
    const int textW = textX1 - textX0, textCx = (textX0 + textX1) / 2;
    // Beside a demo clip the text is its list of motions: from the left, not centred.
    const int textX = demoOn ? textX0 : textCx;
    const Align textAlign = demoOn ? kLeft : kCentre;

    // The instruction, the note, the big countdown and the action line, centred together in
    // the space between. A long instruction gets smaller, down to 1 degree a line, before it runs over.
    const int noteSize = int(ppd * 1.1), noteLh = int(noteSize * 1.3), gapY = int(ppd * 0.6);
    const int bigSize = int(ppd * 3.0), bigLh = int(bigSize * 1.1), actionSize = int(ppd * 1.05), actionLh = int(actionSize * 1.4);
    p.noteLines = Wrap(p.note, noteSize, textW);
    const std::vector<std::string> actionLines = Wrap(p.action, actionSize, textW);
    int size = 0, lh = 0, block = 0;
    for (p.textDeg = 1.4;; p.textDeg -= 0.1) {
        size = int(ppd * p.textDeg), lh = int(size * 1.3);
        p.lines = Wrap(p.text, size, textW);
        block = int(p.lines.size()) * lh + (p.noteLines.empty() ? 0 : gapY + int(p.noteLines.size()) * noteLh) +
                (p.big.empty() ? 0 : gapY + bigLh) + (actionLines.empty() ? 0 : gapY + int(actionLines.size()) * actionLh);
        if (block <= bottom - top || p.textDeg < 1.05) break;
    }
    int y = top + std::max(0, (bottom - top - block) / 2);
    bool first = true;
    auto gapBefore = [&] { if (!first) y += gapY; first = false; };
    if (!p.lines.empty()) {
        first = false;
        for (const std::string &line : p.lines) Text(pic, line, size, textX, y + lh / 2, 1.0, textAlign), y += lh;
    }
    if (!p.noteLines.empty()) {
        gapBefore();
        for (const std::string &line : p.noteLines)
            Text(pic, line, noteSize, textX, y + noteLh / 2, 1.0, 0.62, 0.3, textAlign), y += noteLh;
    }
    if (!p.big.empty()) {
        gapBefore();
        Text(pic, p.big, bigSize, textX, y + bigLh / 2, kCyan[0], kCyan[1], kCyan[2], textAlign), y += bigLh;
    }
    if (!actionLines.empty()) {
        gapBefore();
        for (const std::string &line : actionLines)
            Text(pic, line, actionSize, textX, y + actionLh / 2, kCyan[0], kCyan[1], kCyan[2], textAlign), y += actionLh;
    }

    if (p.paused) {
        // The picture stays faintly behind, so it's clear what resumes; the word sits on a
        // solid pill, so the text behind doesn't run through it.
        const int size = int(ppd * 2.4), pw = TextWidth("Paused", size) + 2 * int(ppd * 1.2), ph = int(size * 1.6);
        RoundRect(pic, 0, 0, kW, kH, kPxPerDeg * 1.9, 0.02, 0.02, 0.03, 0.75);
        RoundRect(pic, (kW - pw) / 2, (kH - ph) / 2, (kW + pw) / 2, (kH + ph) / 2, ph / 2.0, 0.12, 0.12, 0.14, 1);
        Text(pic, "Paused", size, kW / 2, kH / 2, 1.0);
    }
}

struct Target {
    Picture pic{kTargetPx, kTargetPx, {}};
    bool on = false, placed = false;
    double head[3] = {0, 0, 0}, room[3] = {0, 0, 0};
    std::string state = "show";
    double progress = 0;
};

// The dot: lit from the upper left, so it reads as a small ball, with a dark outline so it
// shows against a bright room. hold adds a ring around it filling clockwise; done turns it green.
void DrawTarget(Target &t) {
    Picture &pic = t.pic;
    pic.px.assign(size_t(pic.w) * pic.h * 4, 0);
    const double c = pic.w / 2.0, rad = pic.w * kTargetDotM / kTargetM / 2, outline = pic.w * 0.025;
    const bool done = t.state == "done";
    const double cr = done ? 0.3 : 1.0, cg = done ? 0.9 : 1.0, cb = done ? 0.4 : 1.0;
    const double lx = -0.45, ly = -0.55, lz = std::sqrt(1 - lx * lx - ly * ly);
    Disc(pic, c, c, rad + outline, 0, 0.05, 0.05, 0.06, 0.95);
    for (int y = 0; y < pic.h; ++y)
        for (int x = 0; x < pic.w; ++x) {
            const double dx = (x + 0.5 - c) / rad, dy = (y + 0.5 - c) / rad, d2 = dx * dx + dy * dy;
            const double cover = std::clamp((1 - std::sqrt(d2)) * rad + 0.5, 0.0, 1.0);
            if (cover <= 0) continue;
            const double nz = std::sqrt(std::max(0.0, 1 - d2));
            const double shade = 0.55 + 0.45 * std::max(0.0, dx * lx + dy * ly + nz * lz);
            Blend(pic, x, y, cr * shade, cg * shade, cb * shade, cover);
        }
    if (t.state == "hold") {
        const double r0 = rad + outline + pic.w * 0.05, r1 = r0 + pic.w * 0.07;
        Disc(pic, c, c, r1 + outline, r0 - outline, 0.05, 0.05, 0.06, 0.6);
        Disc(pic, c, c, r1, r0, 1, 1, 1, 0.3);
        Disc(pic, c, c, r1, r0, 0.3, 0.85, 1.0, 1, std::clamp(t.progress, 0.0, 1.0));
    }
}

// ---------------------------------------------------------------- the scene's windows

// A bounty's scene (DESIGN.md, "Scenes"): plain window shapes fixed in the room, with a part
// lit to say where to tap or grab, and a ghost outline to say where to take it. They take no
// input and never move by themselves: the person acts on them, the cameras record.
constexpr int kWinMax = 4;
constexpr int kWinPx = 768;     // a window's picture: its longer side
constexpr int kGhostPx = 384;   // a ghost's
constexpr double kWinEdgeM = 0.03;    // a lit edge's band
constexpr double kWinCornerM = 0.07;  // a lit corner's arms

struct Win {
    std::string id;
    bool on = false;
    double w = 0.4, h = 0.3;      // metres
    std::string look = "plain";   // plain, grid:CxR (buttons), list:N (rows)
    int cols = 0, rows = 0;
    std::string lit = "none", colour = "cyan", label;
    vr::HmdMatrix34_t pose{};     // in the room (standing universe)
    Picture pic{0, 0, {}};
    bool dirty = false, shown = false;
    bool ghostOn = false, ghostDirty = false, ghostShown = false;
    vr::HmdMatrix34_t ghostPose{};
    double ghostScale = 1;
    Picture ghostPic{0, 0, {}};
    vr::VROverlayHandle_t oh = vr::k_ulOverlayHandleInvalid, gh = vr::k_ulOverlayHandleInvalid;
};

const double *Colour(const std::string &name) {
    static const double cyan[3] = {0.3, 0.85, 1.0}, purple[3] = {0.78, 0.48, 1.0}, orange[3] = {1.0, 0.56, 0.16},
                        green[3] = {0.32, 0.9, 0.42};
    return name == "purple" ? purple : name == "orange" ? orange : name == "green" ? green : cyan;
}

bool WinLook(Win &w, const std::string &look) {
    int a = 0, b = 0;
    if (look == "plain") w.cols = w.rows = 0;
    else if (std::sscanf(look.c_str(), "grid:%dx%d", &a, &b) == 2 && a >= 1 && a <= 6 && b >= 1 && b <= 6) w.cols = a, w.rows = b;
    else if (std::sscanf(look.c_str(), "list:%d", &a) == 1 && a >= 1 && a <= 10) w.cols = 1, w.rows = a;
    else return false;
    w.look = look;
    return true;
}

bool WinPart1(const std::string &part) {
    static const char *parts[] = {"none", "bar", "left", "right", "top", "bottom", "tl", "tr", "bl", "br"};
    for (const char *p : parts)
        if (part == p) return true;
    int n = -1;
    return (std::sscanf(part.c_str(), "button:%d", &n) == 1 || std::sscanf(part.c_str(), "item:%d", &n) == 1) && n >= 0;
}

// The parts of "a+b+...": a two-hand grab lights two corners or both edges at once.
std::vector<std::string> WinParts(const std::string &lit) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= lit.size();) {
        const size_t end = std::min(lit.find('+', at), lit.size());
        out.push_back(lit.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

bool WinPart(const std::string &lit) {
    for (const std::string &part : WinParts(lit))
        if (!WinPart1(part)) return false;
    return true;
}

// A window as it shows: a dark body, a title bar, its content (buttons, list rows or faint
// lines of text), and the lit part in its colour, with its label.
void DrawWin(Win &w) {
    const int pw = w.w >= w.h ? kWinPx : std::max(16, int(std::lround(kWinPx * w.w / w.h)));
    const int ph = w.w >= w.h ? std::max(16, int(std::lround(kWinPx * w.h / w.w))) : kWinPx;
    Picture &pic = w.pic;
    pic.w = pw, pic.h = ph;
    pic.px.assign(size_t(pw) * ph * 4, 0);
    const double s = pw / w.w;  // pixels a metre
    const double *c = Colour(w.colour);
    const int rad = int(0.012 * s), bar = int(std::min(0.035, 0.16 * w.h) * s), m = int(0.012 * s);
    const std::vector<std::string> parts = WinParts(w.lit);
    auto isLit = [&parts](const std::string &part) { return std::find(parts.begin(), parts.end(), part) != parts.end(); };
    const bool litBar = isLit("bar");
    RoundRect(pic, 0, 0, pw, ph, rad, 0.11, 0.12, 0.15, 0.94);
    RoundRect(pic, 0, 0, pw, bar + rad, rad, litBar ? c[0] : 0.22, litBar ? c[1] : 0.23, litBar ? c[2] : 0.27, 1);
    Rect(pic, 0, bar, pw, bar + rad, 0.11, 0.12, 0.15, 1);
    for (int k = 0; k < 3; ++k)  // the title bar's three dots, and a short title
        Disc(pic, pw - m - bar * 0.3 - k * bar * 0.55, bar / 2.0, bar * 0.14, 0, litBar ? 0.05 : 0.6, litBar ? 0.05 : 0.6,
             litBar ? 0.06 : 0.62, 0.9);
    Rect(pic, m, bar / 2 - std::max(1, bar / 12), m + int(0.08 * s), bar / 2 + std::max(1, bar / 12), litBar ? 0.05 : 0.6,
         litBar ? 0.05 : 0.6, litBar ? 0.06 : 0.62, 0.8);
    const int x0 = m, y0 = bar + m, x1 = pw - m, y1 = ph - m;
    int litCell = -1;
    for (const std::string &part : parts)
        if (std::sscanf(part.c_str(), "button:%d", &litCell) == 1 || std::sscanf(part.c_str(), "item:%d", &litCell) == 1) break;
        else litCell = -1;
    const int labelSize = std::max(10, int(0.035 * s));
    if (w.cols > 0) {
        const int gap = int(0.01 * s);
        const int cw = (x1 - x0 - (w.cols - 1) * gap) / w.cols, chh = (y1 - y0 - (w.rows - 1) * gap) / w.rows;
        for (int r = 0; r < w.rows; ++r)
            for (int k = 0; k < w.cols; ++k) {
                const int cx = x0 + k * (cw + gap), cy = y0 + r * (chh + gap), cell = r * w.cols + k;
                const bool lit = cell == litCell;
                const bool hollow = lit && w.colour == "orange";  // a no-go: outlined, with its cross
                if (lit && !hollow) RoundRect(pic, cx, cy, cx + cw, cy + chh, rad, c[0], c[1], c[2], 1);
                else RoundRect(pic, cx, cy, cx + cw, cy + chh, rad, 0.22, 0.23, 0.28, 1);
                if (hollow) {
                    const int b = std::max(3, int(0.005 * s));
                    RoundRect(pic, cx, cy, cx + cw, cy + chh, rad, c[0], c[1], c[2], 1);
                    RoundRect(pic, cx + b, cy + b, cx + cw - b, cy + chh - b, std::max(1, rad - b), 0.16, 0.12, 0.1, 1);
                    Text(pic, w.label.empty() ? "×" : w.label, std::min(chh, cw) * 2 / 3, cx + cw / 2, cy + chh / 2, c[0],
                         c[1], c[2]);
                } else if (lit && !w.label.empty()) {
                    Text(pic, w.label, std::min({chh * 2 / 3, cw / 2, labelSize * 3}), cx + cw / 2, cy + chh / 2, 0.05, 0.05, 0.07);
                } else if (w.rows > 1 && w.cols == 1) {  // a list row: a faint line of text
                    Rect(pic, cx + m, cy + chh / 2 - std::max(1, chh / 10), cx + cw / 2, cy + chh / 2 + std::max(1, chh / 10),
                         0.45, 0.46, 0.5, 0.7);
                }
            }
    } else {
        const int lh = int(0.022 * s);  // faint lines of text: the window has something in it
        for (int y = y0 + lh; y + lh < y1; y += 2 * lh)
            Rect(pic, x0 + m, y, x0 + m + int((x1 - x0 - 2 * m) * (0.55 + 0.4 * ((y / lh) % 3) / 2.0)), y + lh / 2, 0.36,
                 0.37, 0.42, 0.6);
    }
    // A lit edge or corner: a band in the window's colour along it, the label beside the first.
    const int band = int(kWinEdgeM * s), arm = int(kWinCornerM * s);
    int lx = -1, ly = -1;
    auto at = [&lx, &ly](int x, int y) {
        if (lx < 0) lx = x, ly = y;
    };
    for (const std::string &part : parts) {
        if (part == "left") RoundRect(pic, 0, 0, band, ph, rad, c[0], c[1], c[2], 0.95), at(band + labelSize, ph / 2);
        else if (part == "right") RoundRect(pic, pw - band, 0, pw, ph, rad, c[0], c[1], c[2], 0.95), at(pw - band - labelSize, ph / 2);
        else if (part == "top") RoundRect(pic, 0, 0, pw, band, rad, c[0], c[1], c[2], 0.95), at(pw / 2, band + labelSize);
        else if (part == "bottom") RoundRect(pic, 0, ph - band, pw, ph, rad, c[0], c[1], c[2], 0.95), at(pw / 2, ph - band - labelSize);
        else if (part.size() == 2 && (part[0] == 't' || part[0] == 'b') && (part[1] == 'l' || part[1] == 'r')) {
            const bool top = part[0] == 't', left = part[1] == 'l';
            const int ax = left ? 0 : pw - arm, ay = top ? 0 : ph - arm, bx = left ? 0 : pw - band, by = top ? 0 : ph - band;
            RoundRect(pic, ax, by, ax + arm, by + band, rad, c[0], c[1], c[2], 0.95);
            RoundRect(pic, bx, ay, bx + band, ay + arm, rad, c[0], c[1], c[2], 0.95);
            at(left ? arm + labelSize : pw - arm - labelSize, top ? arm / 2 + band : ph - arm / 2 - band);
        } else if (part == "bar") {
            at(pw / 2, bar + labelSize);
        }
    }
    if (lx >= 0 && !w.label.empty()) Text(pic, w.label, labelSize, lx, ly, c[0], c[1], c[2]);
}

// The ghost: where to take the window, as a dashed outline with a faint fill, in its colour.
void DrawGhost(Win &w) {
    const int pw = w.w >= w.h ? kGhostPx : std::max(8, int(std::lround(kGhostPx * w.w / w.h)));
    const int ph = w.w >= w.h ? std::max(8, int(std::lround(kGhostPx * w.h / w.w))) : kGhostPx;
    Picture &pic = w.ghostPic;
    pic.w = pw, pic.h = ph;
    pic.px.assign(size_t(pw) * ph * 4, 0);
    const double *c = Colour(w.colour);
    const int t = std::max(3, pw / 90), dash = std::max(8, pw / 18);
    RoundRect(pic, 0, 0, pw, ph, pw / 40.0, c[0], c[1], c[2], 0.1);
    for (int x = 0; x < pw; x += 2 * dash) {
        Rect(pic, x, 0, std::min(pw, x + dash), t, c[0], c[1], c[2], 0.95);
        Rect(pic, x, ph - t, std::min(pw, x + dash), ph, c[0], c[1], c[2], 0.95);
    }
    for (int y = 0; y < ph; y += 2 * dash) {
        Rect(pic, 0, y, t, std::min(ph, y + dash), c[0], c[1], c[2], 0.95);
        Rect(pic, pw - t, y, pw, std::min(ph, y + dash), c[0], c[1], c[2], 0.95);
    }
}

// --no-vr: a picture as a PAM file (RGBA, straight alpha) for checking the layout by eye.
void Dump(const Picture &pic, const std::string &path) {
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", pic.w, pic.h);
        std::fwrite(pic.px.data(), 1, pic.px.size(), f);
        std::fclose(f);
    }
}

// --no-vr: the state the pictures show, as text.
void Print(const Panel &p, const Target &t, bool visible, int number) {
    std::printf("--- picture %d: panel %s\n", number, visible ? "shown" : "hidden");
    std::printf("title: %s\nstep: %s\n", p.title.c_str(), p.step.c_str());
    std::printf("text: %zu line(s), %.1f deg\n", p.lines.size(), p.textDeg);
    for (const std::string &line : p.lines) std::printf("  | %s\n", line.c_str());
    std::printf("note: %zu line(s)\n", p.noteLines.size());
    for (const std::string &line : p.noteLines) std::printf("  | %s\n", line.c_str());
    if (p.countdown >= 0) std::printf("countdown: %.2f\n", p.countdown);
    else std::printf("countdown: off\n");
    for (const auto &[at, label] : p.splits)
        std::printf("split: %.2f \"Switch to %s\"%s\n", at, label.c_str(), p.countdown >= 0 && p.countdown <= at + 1e-6 ? " (reached)" : "");
    std::printf("hands: left %s, right %s\n", p.hands[0].c_str(), p.hands[1].c_str());
    if (p.barOn)
        std::printf("bar: target %.2f, current %.2f, near \"%s\", far \"%s\"\n", p.barTarget, p.barCurrent,
                    p.nearLabel.c_str(), p.farLabel.c_str());
    else std::printf("bar: off\n");
    std::printf("paused: %s\n", p.paused ? "on" : "off");
    std::printf("big: %s\naction: %s\nkeys: %s\nrec: %s\n", p.big.c_str(), p.action.c_str(), p.keys.c_str(),
                p.rec ? "on" : "off");
    if (!p.image.path.empty())
        std::printf("image: %s %dx%d%s%s\n", p.image.path.c_str(), p.image.src.w, p.image.src.h,
                    p.imageMode.empty() ? "" : " ", p.imageMode.c_str());
    else std::printf("image: off\n");
    if (p.demo)
        std::printf("demo: %s %dx%d frame %d loop %d%s%s, drawn %dx%d\n", p.demo->path.c_str(), p.demo->w, p.demo->h,
                    p.demo->frame, p.demo->loops, p.demo->mirror ? " mirror" : "", p.demo->key ? "" : " nokey", p.demoW, p.demoH);
    else std::printf("demo: off\n");
    if (!p.wherePos.empty() || !p.whereDist.empty())
        std::printf("where: %s %s\n", p.wherePos.empty() ? "-" : p.wherePos.c_str(), p.whereDist.empty() ? "-" : p.whereDist.c_str());
    else std::printf("where: off\n");
    if (!p.strip.empty()) {
        std::printf("strip: cue %d:", p.stripCue);
        for (const StripItem &it : p.strip)
            std::printf(" %s(%s%s)", it.label.c_str(), it.path.empty() ? "no picture" : it.path.c_str(),
                        it.mode.empty() ? "" : (" " + it.mode).c_str());
        std::printf("\n");
    } else {
        std::printf("strip: off\n");
    }
    if (t.on)
        std::printf("target: %s %.2f, head %.3f %.3f %.3f, room %.3f %.3f %.3f\n", t.state.c_str(), t.progress, t.head[0],
                    t.head[1], t.head[2], t.room[0], t.room[1], t.room[2]);
    else std::printf("target: off\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- the buffers (see the top)

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
    uint64_t whole = 0;  // the whole picture it last had (Buffers::whole), 0: none
};

struct Area {
    int x, y, w, h;
};

struct Buffers {
    int w = 0, h = 0;
    int drm = -1;
    gbm_device *gbm = nullptr;
    Buffer b[3];
    int next = 0;
    bool ok = false;
    uint64_t whole = 0;  // counts the pictures presented whole

    bool Make(int width, int height) {
        w = width, h = height;
        drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm >= 0) gbm = gbm_create_device(drm);
        for (Buffer &x : b) {
            // ABGR8888 is R, G, B, A in memory, like Picture::px.
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
            std::fprintf(stderr, "ft-handpanel: no shared buffers; falling back to SetOverlayRaw (it flickers)\n");
            Drop();
        }
        return ok;
    }

    void Drop() {
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

    // A picture to the overlay: into the next buffer, premultiplied (the overlay's flag says
    // so), then the overlay switches to it. The copy runs outside g_vr, so the pose log waits
    // only for the two calls at the end. With area (a demo clip's frame): only that changed
    // since the last whole picture, so a buffer that has that picture gets only the area.
    void Present(vr::IVROverlay *ov, vr::VROverlayHandle_t oh, const Picture &p, const Area *area = nullptr) {
        vr::EVROverlayError e;
        if (!ok) {
            std::lock_guard<std::mutex> lk(g_vr);
            e = ov->SetOverlayRaw(oh, const_cast<uint8_t *>(p.px.data()), uint32_t(p.w), uint32_t(p.h), 4);
        } else {
            Buffer &x = b[next];
            next = (next + 1) % 3;
            if (!area) ++whole;
            Area all{0, 0, p.w, p.h}, a = area && x.whole == whole ? *area : all;
            a.x = std::clamp(a.x, 0, p.w), a.y = std::clamp(a.y, 0, p.h);
            a.w = std::clamp(a.w, 0, p.w - a.x), a.h = std::clamp(a.h, 0, p.h - a.y);
            x.whole = whole;
            uint32_t stride = 0;
            void *mapping = nullptr;
            auto *dst = static_cast<uint8_t *>(gbm_bo_map(x.bo, 0, 0, p.w, p.h, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
            if (!dst) {
                std::fprintf(stderr, "ft-handpanel: can't map a buffer\n");
                return;
            }
            for (int y = a.y; y < a.y + a.h; ++y) {
                const uint8_t *src = &p.px[(size_t(y) * p.w + a.x) * 4];
                uint8_t *row = dst + size_t(y) * stride + size_t(a.x) * 4;
                for (int i = 0; i < a.w * 4; i += 4) {
                    const unsigned a = src[i + 3];
                    row[i] = uint8_t(src[i] * a / 255), row[i + 1] = uint8_t(src[i + 1] * a / 255);
                    row[i + 2] = uint8_t(src[i + 2] * a / 255), row[i + 3] = uint8_t(a);
                }
            }
            gbm_bo_unmap(x.bo, mapping);
            const vr::VRTextureBounds_t bounds{0, 0, float(p.w) / w, float(p.h) / h};
            vr::Texture_t tex = {&x.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
            std::lock_guard<std::mutex> lk(g_vr);
            ov->SetOverlayTextureBounds(oh, &bounds);
            e = ov->SetOverlayTexture(oh, &tex);
        }
        if (e != vr::VROverlayError_None)
            std::fprintf(stderr, "ft-handpanel: the picture didn't go to SteamVR: %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
};

// ---------------------------------------------------------------- the pose log (poses.jsonl)

// Non-finite numbers aren't JSON; SteamVR shouldn't send any, but one would spoil the file.
void PutFloat(FILE *f, float v) { std::fprintf(f, "%.6g", std::isfinite(v) ? double(v) : 0.0); }

void PutDevice(FILE *f, const char *name, const vr::TrackedDevicePose_t *pose) {
    if (!pose || !pose->bDeviceIsConnected) {
        std::fprintf(f, "\"%s\": null", name);
        return;
    }
    std::fprintf(f, "\"%s\": {\"m\": [", name);
    for (int i = 0; i < 12; ++i) {
        if (i) std::fputs(", ", f);
        PutFloat(f, pose->mDeviceToAbsoluteTracking.m[i / 4][i % 4]);
    }
    std::fprintf(f, "], \"r\": %d, \"ok\": %s}", int(pose->eTrackingResult), pose->bPoseIsValid ? "true" : "false");
}

// The thread is the only one touching the file while it runs; Start and Stop, on the main
// thread, open and close it around the thread's life.
struct PoseLog {
    FILE *f = nullptr;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<long> samples{0};

    bool Start(const std::string &path) {
        Stop();
        if (!(f = std::fopen(path.c_str(), "ae"))) return false;
        std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
        samples = 0, run = true;
        th = std::thread([this] { Run(); });
        return true;
    }

    long Stop() {
        if (!f) return 0;
        run = false;
        if (th.joinable()) th.join();
        if (std::fflush(f) != 0 || std::ferror(f)) std::fprintf(stderr, "ft-handpanel: writing the pose log failed\n");
        std::fclose(f);
        f = nullptr;
        return samples;
    }

    void Run() {
        auto *sys = vr::VRSystem();
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::TrackedDeviceIndex_t left = vr::k_unTrackedDeviceIndexInvalid, right = left;
        int64_t next = NowNs(), lastFlush = next, lastRoles = next - 1'000'000'000;
        while (run) {
            int64_t before, after;
            {
                std::lock_guard<std::mutex> lk(g_vr);
                // Roles change rarely (a controller turned on, hands swapped): four looks a second.
                if (next - lastRoles >= 250'000'000) {
                    left = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
                    right = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
                    lastRoles = next;
                }
                before = NowNs();
                sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
                after = NowNs();
            }
            const int64_t t = before + (after - before) / 2;
            std::fprintf(f, "{\"t\": %lld, ", (long long)t);
            PutDevice(f, "hmd", &poses[vr::k_unTrackedDeviceIndex_Hmd]);
            std::fputs(", ", f);
            PutDevice(f, "left", left < vr::k_unMaxTrackedDeviceCount ? &poses[left] : nullptr);
            std::fputs(", ", f);
            PutDevice(f, "right", right < vr::k_unMaxTrackedDeviceCount ? &poses[right] : nullptr);
            std::fputs("}\n", f);
            ++samples;
            if (t - lastFlush >= 1'000'000'000) std::fflush(f), lastFlush = t;
            // On a fixed beat; after a stall (a suspended process, a slow OpenVR call), pick up
            // from now rather than writing a burst of samples to catch up.
            next += kPosePeriodNs;
            const int64_t now = NowNs();
            if (next < now - kPosePeriodNs) next = now;
            const timespec ts{time_t(next / 1'000'000'000), long(next % 1'000'000'000)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
    }
};

// ---------------------------------------------------------------- placing the target

// The headset's pose now, in the standing universe; false if SteamVR has none.
bool HeadPose(vr::HmdMatrix34_t &m) {
    vr::TrackedDevicePose_t pose{};
    std::lock_guard<std::mutex> lk(g_vr);
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
    m = pose.mDeviceToAbsoluteTracking;
    return pose.bDeviceIsConnected && pose.bPoseIsValid;
}

// An overlay's transform at `at`, its face (+z) turned toward `eye`, upright in the room.
vr::HmdMatrix34_t Facing(const double at[3], const double eye[3]) {
    auto norm = [](double v[3]) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-9) return false;
        v[0] /= n, v[1] /= n, v[2] /= n;
        return true;
    };
    double z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    if (!norm(z)) z[0] = z[1] = 0, z[2] = 1;
    double x[3] = {z[2], 0, -z[0]};  // up (0, 1, 0) x z
    if (!norm(x)) x[0] = 1, x[1] = x[2] = 0;  // straight above or below: any upright x will do
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    vr::HmdMatrix34_t m{};
    for (int r = 0; r < 3; ++r)
        m.m[r][0] = float(x[r]), m.m[r][1] = float(y[r]), m.m[r][2] = float(z[r]), m.m[r][3] = float(at[r]);
    return m;
}

// a then b: 3x4 transforms, each with an unwritten last row of 0 0 0 1.
vr::HmdMatrix34_t Mul(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b) {
    vr::HmdMatrix34_t m{};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            m.m[r][k] = a.m[r][0] * b.m[0][k] + a.m[r][1] * b.m[1][k] + a.m[r][2] * b.m[2][k] + (k == 3 ? a.m[r][3] : 0);
    return m;
}

// Moved by (x, y, z), then turned by yaw (about +y), pitch (about +x) and roll (about +z), in
// degrees, in that order: a window's or a ghost's pose in its parent's frame.
vr::HmdMatrix34_t Local(double x, double y, double z, double yaw, double pitch, double roll) {
    const double a = yaw * M_PI / 180, b = pitch * M_PI / 180, c = roll * M_PI / 180;
    const double ry[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
    const double rx[3][3] = {{1, 0, 0}, {0, std::cos(b), -std::sin(b)}, {0, std::sin(b), std::cos(b)}};
    const double rz[3][3] = {{std::cos(c), -std::sin(c), 0}, {std::sin(c), std::cos(c), 0}, {0, 0, 1}};
    double t[3][3], r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = t[i][0] * rz[0][j] + t[i][1] * rz[1][j] + t[i][2] * rz[2][j];
    vr::HmdMatrix34_t m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(r[i][0]), m.m[i][1] = float(r[i][1]), m.m[i][2] = float(r[i][2]);
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}

std::string PoseReply(const vr::HmdMatrix34_t &m) {
    char out[256];
    int len = std::snprintf(out, sizeof out, "ok");
    for (int i = 0; i < 12; ++i) len += std::snprintf(out + len, sizeof out - len, " %.5f", m.m[i / 4][i % 4]);
    return out;
}

// --no-vr: the windows as text.
void PrintWins(const Win *wins) {
    for (int k = 0; k < kWinMax; ++k) {
        const Win &w = wins[k];
        if (!w.on) continue;
        std::printf("win %s: %.2f x %.2f m %s at %.3f %.3f %.3f, lit %s %s%s%s\n", w.id.c_str(), w.w, w.h, w.look.c_str(),
                    w.pose.m[0][3], w.pose.m[1][3], w.pose.m[2][3], w.lit.c_str(), w.colour.c_str(),
                    w.label.empty() ? "" : " ", w.label.c_str());
        if (w.ghostOn)
            std::printf("ghost %s: at %.3f %.3f %.3f, scale %.2f\n", w.id.c_str(), w.ghostPose.m[0][3], w.ghostPose.m[1][3],
                        w.ghostPose.m[2][3], w.ghostScale);
    }
    std::fflush(stdout);
}

// The command word: buf starts with word, then a space or the end; rest is what follows.
bool Is(const char *buf, const char *word, const char **rest = nullptr) {
    const size_t n = std::strlen(word);
    if (std::strncmp(buf, word, n) || (buf[n] && buf[n] != ' ')) return false;
    if (rest) *rest = buf[n] ? buf + n + 1 : buf + n;
    return true;
}

bool HandState(const std::string &s) { return s == "seen" || s == "lost" || s == "off"; }

}  // namespace

int main(int argc, char **argv) {
    bool watchStdin = false, noVr = false;
    std::string sockName = "ft_handpanel", dumpDir;
    double distance = 1.2, upDeg = kUpDeg, widthDeg = kWidthDeg;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--watch-stdin")) watchStdin = true;
        else if (!std::strcmp(argv[i], "--no-vr")) noVr = true;
        else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) sockName = argv[++i];
        else if (!std::strcmp(argv[i], "--distance") && i + 1 < argc) distance = std::clamp(std::atof(argv[++i]), 0.5, 5.0);
        else if (!std::strcmp(argv[i], "--up") && i + 1 < argc) upDeg = std::clamp(std::atof(argv[++i]), -60.0, 60.0);
        else if (!std::strcmp(argv[i], "--width") && i + 1 < argc) widthDeg = std::clamp(std::atof(argv[++i]), 5.0, 90.0);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--watch-stdin] [--socket NAME] [--distance METRES] [--up DEG] [--width DEG] [--no-vr [--dump DIR]]\n",
                         argv[0]);
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
        std::fprintf(stderr, "ft-handpanel: @%s is taken (another copy running?)\n", sockName.c_str());
        return 1;
    }

    vr::IVROverlay *ov = nullptr;
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid, th = vr::k_ulOverlayHandleInvalid;
    Buffers buffers, targetBuffers;
    if (!noVr) {
        // As Frametop's other SteamVR clients: background first, so we never start vrserver.
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, vr::VRApplication_Background);
        if (err == vr::VRInitError_None) {
            vr::VR_Shutdown();
            vr::VR_Init(&err, vr::VRApplication_Overlay);
        }
        if (err != vr::VRInitError_None) {
            std::fprintf(stderr, "ft-handpanel: SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            return 1;
        }
        ov = vr::VROverlay();
        if (ov->CreateOverlay("frametop.handpanel", "Frametop hand recorder", &h) != vr::VROverlayError_None ||
            ov->CreateOverlay("frametop.handpanel.target", "Frametop hand recorder target", &th) != vr::VROverlayError_None) {
            std::fprintf(stderr, "ft-handpanel: can't create the overlays (another copy running?)\n");
            return 1;
        }
        ov->SetOverlaySortOrder(h, 250);  // in front of Frametop's screens and the pointer's dot
        ov->SetOverlaySortOrder(th, 250);
        if (buffers.Make(kW, kH)) ov->SetOverlayFlag(h, vr::VROverlayFlags_IsPremultiplied, true);
        if (targetBuffers.Make(kTargetPx, kTargetPx)) ov->SetOverlayFlag(th, vr::VROverlayFlags_IsPremultiplied, true);
        // Fixed to the head: distance along a line UP_DEG above straight ahead, tilted back by
        // as much so it faces the eyes square on.
        const double up = upDeg * M_PI / 180;
        vr::HmdMatrix34_t m{};
        m.m[0][0] = 1;
        m.m[1][1] = float(std::cos(up)), m.m[1][2] = float(-std::sin(up));
        m.m[2][1] = float(std::sin(up)), m.m[2][2] = float(std::cos(up));
        m.m[1][3] = float(distance * std::sin(up)), m.m[2][3] = float(-distance * std::cos(up));
        ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        ov->SetOverlayWidthInMeters(h, float(2 * distance * std::tan(widthDeg * M_PI / 360)));
        ov->SetOverlayWidthInMeters(th, float(kTargetM));
    }
    LoadFont();

    Panel p;
    Target t;
    PoseLog poseLog;
    bool visible = false, dirty = false, shown = false;  // shown: SteamVR shows it (after its first picture)
    bool targetDirty = false, targetShown = false;
    int pictures = 0;
    Win wins[kWinMax];
    Buffers winBuffers[kWinMax], ghostBuffers[kWinMax];
    bool winsChanged = false;  // --no-vr: print them

    // A window's slot by id, or (place) a free one; -1: none.
    auto winSlot = [&wins](const std::string &id, bool make) {
        for (int k = 0; k < kWinMax; ++k)
            if (wins[k].on && wins[k].id == id) return k;
        if (make)
            for (int k = 0; k < kWinMax; ++k)
                if (!wins[k].on) return k;
        return -1;
    };
    auto winOff = [&](Win &w) {
        if (ov) {
            std::lock_guard<std::mutex> lk(g_vr);
            if (w.shown) ov->HideOverlay(w.oh);
            if (w.ghostShown) ov->HideOverlay(w.gh);
        }
        const vr::VROverlayHandle_t oh = w.oh, gh = w.gh;
        w = Win{};
        w.oh = oh, w.gh = gh;
        winsChanged = true;
    };
    // win <id> place|lit|ghost|off ..., win clear: the reply.
    auto winCommand = [&](const char *rest) -> std::string {
        char id[32] = "", verb[16] = "";
        int used = 0;
        if (!std::strcmp(rest, "clear")) {
            for (Win &w : wins)
                if (w.on) winOff(w);
            return "ok";
        }
        if (std::sscanf(rest, "%31s %15s %n", id, verb, &used) < 2)
            return "error usage: win <id> place|lit|ghost|off ... | win clear";
        const char *args = rest + used;
        const std::string v = verb;
        const int k = winSlot(id, v == "place");
        if (k < 0) return v == "place" ? "error no free window (at most 4)" : std::string("error no window ") + id;
        Win &w = wins[k];
        if (v == "off") {
            winOff(w);
            return "ok";
        }
        if (v == "place") {
            // place <x> <y> <z> <w> <h> [<yaw> <pitch>] [<look>]: the centre in the head's frame now, levelled
            double x, y, z, ww, hh, yaw = 0, pitch = 0;
            char look[32] = "plain";
            int n = 0;
            if (std::sscanf(args, "%lf %lf %lf %lf %lf %n", &x, &y, &z, &ww, &hh, &n) < 5 || ww < 0.02 || hh < 0.02 || ww > 3 || hh > 3)
                return "error usage: win <id> place <x> <y> <z> <w> <h> [<yaw> <pitch>] [<look>]";
            const char *more = args + n;
            int n2 = 0;
            if (std::sscanf(more, "%lf %lf %n", &yaw, &pitch, &n2) == 2) more += n2;
            std::sscanf(more, "%31s", look);
            Win next = w;
            if (!WinLook(next, look)) return std::string("error look ") + look + ": plain, grid:CxR or list:N";
            vr::HmdMatrix34_t head{};
            head.m[0][0] = head.m[1][1] = head.m[2][2] = 1;  // --no-vr: the head at the room's origin
            if (!noVr && !HeadPose(head)) return "error no head pose";
            // The head's frame made level: turned with the head, not tipped with it, so a window
            // "ahead" stays at the same height whether the person looks down at their lap or up.
            const double fx = head.m[0][2], fz = head.m[2][2], fn = std::hypot(fx, fz);
            if (fn > 0.05) {
                const double z[3] = {fx / fn, 0, fz / fn}, x[3] = {z[2], 0, -z[0]};
                for (int r = 0; r < 3; ++r) head.m[r][0] = float(x[r]), head.m[r][1] = r == 1, head.m[r][2] = float(z[r]);
            }
            const double local[3] = {x, y, z}, eye[3] = {head.m[0][3], head.m[1][3], head.m[2][3]};
            double at[3];
            for (int r = 0; r < 3; ++r) at[r] = head.m[r][0] * local[0] + head.m[r][1] * local[1] + head.m[r][2] * local[2] + head.m[r][3];
            next.id = id, next.on = true, next.w = ww, next.h = hh;
            next.pose = Mul(Facing(at, eye), Local(0, 0, 0, yaw, pitch, 0));
            next.lit = "none", next.label.clear(), next.colour = "cyan";
            next.ghostOn = false;
            if (w.ghostShown && ov) {
                std::lock_guard<std::mutex> lk(g_vr);
                ov->HideOverlay(w.gh);
            }
            next.ghostShown = false;
            next.dirty = true;
            w = next;
            if (ov) {
                if (w.oh == vr::k_ulOverlayHandleInvalid) {
                    const std::string key = "frametop.handpanel.win." + std::to_string(k);
                    std::lock_guard<std::mutex> lk(g_vr);
                    if (ov->CreateOverlay(key.c_str(), "Frametop hand recorder window", &w.oh) != vr::VROverlayError_None)
                        return "error can't create the window's overlay";
                    ov->SetOverlaySortOrder(w.oh, 240);  // under the panel
                }
                std::lock_guard<std::mutex> lk(g_vr);
                ov->SetOverlayTransformAbsolute(w.oh, vr::TrackingUniverseStanding, &w.pose);
                ov->SetOverlayWidthInMeters(w.oh, float(w.w));
            }
            winsChanged = true;
            return PoseReply(w.pose);
        }
        if (v == "lit") {
            // lit <part> [<colour>] [<label>]
            char part[64] = "", colour[16] = "cyan", label[32] = "";
            if (std::sscanf(args, "%63s %15s %31s", part, colour, label) < 1 || !WinPart(part))
                return "error usage: win <id> lit none|bar|left|right|top|bottom|tl|tr|bl|br|button:N|item:N[+...] [<colour>] [<label>]";
            w.lit = part, w.colour = colour, w.label = label;
            for (char &ch : w.label)
                if (ch == '_') ch = ' ';
            w.dirty = winsChanged = true;
            if (w.ghostOn) w.ghostDirty = true;  // the ghost takes the lit colour
            return "ok";
        }
        if (v == "ghost") {
            // ghost <dx> <dy> <dz> <yaw> <pitch> <roll> [<scale>]: in the window's own frame
            if (!std::strcmp(args, "off")) {
                if (ov && w.ghostShown) {
                    std::lock_guard<std::mutex> lk(g_vr);
                    ov->HideOverlay(w.gh);
                }
                w.ghostOn = w.ghostShown = false, winsChanged = true;
                return "ok";
            }
            double dx, dy, dz, yaw, pitch, roll, scale = 1;
            if (std::sscanf(args, "%lf %lf %lf %lf %lf %lf %lf", &dx, &dy, &dz, &yaw, &pitch, &roll, &scale) < 6 || scale < 0.2 || scale > 5)
                return "error usage: win <id> ghost <dx> <dy> <dz> <yaw> <pitch> <roll> [<scale>] | win <id> ghost off";
            w.ghostPose = Mul(w.pose, Local(dx, dy, dz, yaw, pitch, roll));
            w.ghostScale = scale, w.ghostOn = w.ghostDirty = winsChanged = true;
            if (ov) {
                if (w.gh == vr::k_ulOverlayHandleInvalid) {
                    const std::string key = "frametop.handpanel.ghost." + std::to_string(k);
                    std::lock_guard<std::mutex> lk(g_vr);
                    if (ov->CreateOverlay(key.c_str(), "Frametop hand recorder ghost", &w.gh) != vr::VROverlayError_None)
                        return "error can't create the ghost's overlay";
                    ov->SetOverlaySortOrder(w.gh, 245);
                }
                std::lock_guard<std::mutex> lk(g_vr);
                ov->SetOverlayTransformAbsolute(w.gh, vr::TrackingUniverseStanding, &w.ghostPose);
                ov->SetOverlayWidthInMeters(w.gh, float(w.w * scale));
            }
            return PoseReply(w.ghostPose);
        }
        return "error usage: win <id> place|lit|ghost|off ... | win clear";
    };
    std::fprintf(stderr, "ft-handpanel running: @%s, %.2f m%s\n", sockName.c_str(), distance, noVr ? ", no VR" : "");

    while (!g_stop) {
        pollfd pfd{sock, POLLIN, 0};
        int wait = 50;
        if (p.demo && visible && !p.paused)  // up to the demo's next frame
            wait = int(std::clamp<int64_t>((p.demo->due - NowNs()) / 1'000'000 + 1, 0, 50));
        poll(&pfd, 1, wait);
        char buf[4096];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;  // from echo or socat
            buf[n] = 0;
            std::string reply = "ok";
            const char *rest = nullptr;
            double a = 0, b = 0, c = 0;
            int used = 0;
            if (Is(buf, "show")) {
                visible = dirty = true;  // shown with its first picture
            } else if (Is(buf, "hide")) {
                if (ov) {
                    std::lock_guard<std::mutex> lk(g_vr);
                    ov->HideOverlay(h);
                }
                if (noVr && visible) std::printf("--- panel hidden\n"), std::fflush(stdout);
                visible = shown = false;
            } else if (Is(buf, "title", &rest)) {
                p.title = rest, dirty = true;
            } else if (Is(buf, "step", &rest)) {
                p.step = rest, dirty = true;
            } else if (Is(buf, "text", &rest)) {
                p.text = rest, dirty = true;
            } else if (Is(buf, "note", &rest)) {
                p.note = rest, dirty = true;
            } else if (Is(buf, "countdown", &rest)) {
                if (!std::strcmp(rest, "off")) p.countdown = -1, dirty = true;
                else if (std::sscanf(rest, "%lf", &a) == 1) p.countdown = std::clamp(a, 0.0, 1.0), dirty = true;
                else reply = "error usage: countdown <0..1>|off";
            } else if (Is(buf, "split", &rest)) {
                // split <at> <label>;...
                std::vector<std::pair<double, std::string>> items;
                bool ok = std::strcmp(rest, "off") == 0;
                if (!ok) {
                    std::string all = rest;
                    ok = true;
                    for (size_t at = 0; at <= all.size();) {
                        const size_t end = std::min(all.find(';', at), all.size());
                        const std::string item = all.substr(at, end - at);
                        at = end + 1;
                        if (item.empty()) continue;
                        double share = 0;
                        int n = 0;
                        if (std::sscanf(item.c_str(), "%lf %n", &share, &n) < 1 || !n || !item[n]) { ok = false; break; }
                        items.emplace_back(std::clamp(share, 0.0, 1.0), item.substr(n));
                    }
                    ok = ok && !items.empty();
                }
                if (ok) p.splits = items, dirty = true;
                else reply = "error usage: split <at 0..1> <label>;... | split off";
            } else if (Is(buf, "hands", &rest)) {
                char l[16] = "", r[16] = "";
                if (std::sscanf(rest, "%15s %15s", l, r) == 2 && HandState(l) && HandState(r))
                    p.hands[0] = l, p.hands[1] = r, dirty = true;
                else reply = "error usage: hands seen|lost|off seen|lost|off";
            } else if (Is(buf, "bar", &rest)) {
                if (!std::strcmp(rest, "off")) {
                    p.barOn = false, dirty = true;
                } else if (std::sscanf(rest, "%lf %lf%n", &a, &b, &used) == 2) {
                    p.barOn = true, p.barTarget = std::clamp(a, 0.0, 1.0), p.barCurrent = b < 0 ? -1 : std::min(b, 1.0);
                    std::string labels = rest + used;
                    labels.erase(0, std::min(labels.find_first_not_of(' '), labels.size()));
                    if (!labels.empty()) {
                        size_t cut = labels.find('|');
                        if (cut == std::string::npos) cut = labels.find(' ');
                        p.nearLabel = labels.substr(0, cut);
                        p.farLabel = cut == std::string::npos ? "Far" : labels.substr(cut + 1);
                    } else {
                        p.nearLabel = "Near", p.farLabel = "Far";
                    }
                    dirty = true;
                } else {
                    reply = "error usage: bar <target 0..1> <current 0..1|-1> [near label] [far label] | bar off";
                }
            } else if (Is(buf, "paused", &rest)) {
                if (!std::strcmp(rest, "on") || !std::strcmp(rest, "off")) p.paused = rest[1] == 'n', dirty = true;
                else reply = "error usage: paused on|off";
            } else if (Is(buf, "rec", &rest)) {
                if (!std::strcmp(rest, "on") || !std::strcmp(rest, "off")) p.rec = rest[1] == 'n', dirty = true;
                else reply = "error usage: rec on|off";
            } else if (Is(buf, "action", &rest)) {
                p.action = rest, dirty = true;
            } else if (Is(buf, "big", &rest)) {
                p.big = rest, dirty = true;
            } else if (Is(buf, "keys", &rest)) {
                p.keys = rest, dirty = true;
            } else if (Is(buf, "image", &rest)) {
                // image <path> [mirror|both]: the path may hold spaces, the mode is the last word
                std::string path = rest, mode;
                for (const char *m : {" mirror", " both"}) {
                    const size_t n = std::strlen(m);
                    if (path.size() > n && !path.compare(path.size() - n, n, m)) mode = m + 1, path.resize(path.size() - n);
                }
                if (path.empty()) {
                    reply = "error usage: image <path> [mirror|both] | image off";
                } else if (path == "off") {
                    p.image = Image{}, p.imageMode.clear(), dirty = true;
                } else if (path != p.image.path && !LoadImage(p.image, path)) {
                    reply = "error can't read " + path + ": " + stbi_failure_reason();
                    p.imageMode.clear(), dirty = true;  // no picture rather than the last one
                } else {
                    p.imageMode = mode, dirty = true;
                }
            } else if (Is(buf, "demo", &rest)) {
                // demo <path> [mirror] [nokey] / demo restart / demo off: the path may hold spaces
                std::string path = rest;
                auto flag = [&path](const char *word) {
                    const size_t n = std::strlen(word) + 1;
                    if (path.size() <= n || path.compare(path.size() - n, n, std::string(" ") + word)) return false;
                    path.resize(path.size() - n);
                    return true;
                };
                const bool nokey = flag("nokey");
                const bool mirror = flag("mirror");
                if (path.empty()) {
                    reply = "error usage: demo <path> [mirror] [nokey] | demo restart | demo off";
                } else if (path == "off") {
                    p.demo.reset(), dirty = true;
                } else if (path == "restart") {
                    if (p.demo) p.demo->loops = 0;
                    if (p.demo && !p.demo->Start(NowNs())) reply = "error can't read " + p.demo->path, p.demo.reset();
                    dirty = true;
                } else if (!p.demo || path != p.demo->path) {
                    auto d = std::make_unique<Demo>();
                    d->mirror = mirror, d->key = !nokey;
                    if (d->Load(path)) p.demo = std::move(d);
                    else reply = "error can't read " + path + ": " + stbi_failure_reason(), p.demo.reset();
                    dirty = true;  // no clip rather than the last one
                } else if (mirror != p.demo->mirror || nokey == p.demo->key) {
                    p.demo->mirror = mirror, p.demo->key = !nokey, p.demo->fresh = dirty = true;
                }
            } else if (Is(buf, "strip", &rest)) {
                // strip <cue> <path>|<mode>|<label>;...
                int cue = -1, used = 0;
                if (!std::strcmp(rest, "off")) {
                    p.strip.clear(), p.stripCue = -1, dirty = true;
                } else if (std::sscanf(rest, "%d %n", &cue, &used) >= 1 && rest[used]) {
                    std::vector<StripItem> items;
                    std::string all = rest + used;
                    for (size_t at = 0; at <= all.size();) {
                        const size_t end = std::min(all.find(';', at), all.size());
                        const std::string item = all.substr(at, end - at);
                        at = end + 1;
                        if (item.empty()) continue;
                        const size_t a1 = item.find('|'), a2 = a1 == std::string::npos ? a1 : item.find('|', a1 + 1);
                        StripItem it;
                        it.path = item.substr(0, a1);
                        if (a1 != std::string::npos) it.mode = item.substr(a1 + 1, a2 == std::string::npos ? a2 : a2 - a1 - 1);
                        if (a2 != std::string::npos) it.label = item.substr(a2 + 1);
                        if (it.path == "-") it.path.clear();
                        if (it.mode == "-") it.mode.clear();
                        items.push_back(it);
                    }
                    if (items.empty()) {
                        reply = "error usage: strip <cue> <path>|<mode>|<label>;... | strip off";
                    } else {
                        for (const StripItem &it : items)
                            if (!it.path.empty() && StripImage(it.path).path.empty()) reply = "error can't read " + it.path;
                        p.strip = items, p.stripCue = cue, dirty = true;
                    }
                } else {
                    reply = "error usage: strip <cue> <path>|<mode>|<label>;... | strip off";
                }
            } else if (Is(buf, "where", &rest)) {
                char pos[16] = "", dist[16] = "";
                if (!std::strcmp(rest, "off")) {
                    p.wherePos.clear(), p.whereDist.clear(), dirty = true;
                } else if (std::sscanf(rest, "%15s %15s", pos, dist) >= 1) {
                    p.wherePos = std::strcmp(pos, "-") ? pos : "";
                    p.whereDist = *dist && std::strcmp(dist, "-") ? dist : "";
                    dirty = true;
                } else {
                    reply = "error usage: where <position|-> <distance|-> | where off";
                }
            } else if (Is(buf, "target", &rest)) {
                char state[16] = "show";
                double prog = 0;
                const int got = std::sscanf(rest, "%lf %lf %lf %15s %lf", &a, &b, &c, state, &prog);
                const std::string st = state;
                if (!std::strcmp(rest, "off")) {
                    if (ov && targetShown) {
                        std::lock_guard<std::mutex> lk(g_vr);
                        ov->HideOverlay(th);
                    }
                    if (noVr && t.on) targetDirty = true;
                    t.on = t.placed = targetShown = false;
                } else if (got < 3 || (st != "show" && st != "hold" && st != "done") || (st == "hold" && got < 5)) {
                    reply = "error usage: target <x> <y> <z> [show|hold <0..1>|done] | target off";
                } else {
                    // A new point goes into the room where the head is now, and stays there.
                    const bool same = t.placed && a == t.head[0] && b == t.head[1] && c == t.head[2];
                    vr::HmdMatrix34_t m{};
                    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;  // --no-vr: the head at the room's origin
                    if (!same && !noVr && !HeadPose(m)) {
                        reply = "error no head pose";
                    } else {
                        if (!same) {
                            const double head[3] = {a, b, c}, eye[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
                            for (int r = 0; r < 3; ++r) {
                                t.head[r] = head[r];
                                t.room[r] = m.m[r][0] * a + m.m[r][1] * b + m.m[r][2] * c + m.m[r][3];
                            }
                            if (ov) {
                                const vr::HmdMatrix34_t at = Facing(t.room, eye);
                                std::lock_guard<std::mutex> lk(g_vr);
                                ov->SetOverlayTransformAbsolute(th, vr::TrackingUniverseStanding, &at);
                            }
                            t.placed = true;
                        }
                        t.on = true, t.state = st, t.progress = st == "hold" ? std::clamp(prog, 0.0, 1.0) : 0;
                        targetDirty = true;
                        char out[96];
                        std::snprintf(out, sizeof out, "ok %.4f %.4f %.4f", t.room[0], t.room[1], t.room[2]);
                        reply = out;
                    }
                }
            } else if (Is(buf, "poses", &rest)) {
                const char *path = nullptr;
                if (Is(rest, "start", &path) && *path) {
                    // --no-vr writes nothing: there are no poses.
                    if (!noVr && !poseLog.Start(path)) reply = std::string("error can't open ") + path + ": " + std::strerror(errno);
                } else if (Is(rest, "stop")) {
                    reply = "ok " + std::to_string(poseLog.Stop());
                } else {
                    reply = "error usage: poses start <path> | poses stop";
                }
            } else if (Is(buf, "devices")) {
                if (noVr) {
                    reply = "error no VR (--no-vr)";
                } else {
                    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
                    vr::TrackedDeviceIndex_t idx[3] = {vr::k_unTrackedDeviceIndex_Hmd, 0, 0};
                    {
                        std::lock_guard<std::mutex> lk(g_vr);
                        auto *sys = vr::VRSystem();
                        idx[1] = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
                        idx[2] = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
                        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
                    }
                    reply = "ok";
                    const char *names[3] = {"hmd", "left", "right"};
                    for (int k = 0; k < 3; ++k) {
                        const bool there = idx[k] < vr::k_unMaxTrackedDeviceCount && poses[idx[k]].bDeviceIsConnected;
                        reply += std::string(" ") + names[k] + " " + (there ? std::to_string(int(poses[idx[k]].eTrackingResult)) : "-");
                    }
                }
            } else if (Is(buf, "head")) {
                vr::HmdMatrix34_t m;
                if (noVr) {
                    reply = "error no VR (--no-vr)";
                } else if (!HeadPose(m)) {
                    reply = "error no head pose";
                } else {
                    char out[256];
                    int len = std::snprintf(out, sizeof out, "ok");
                    for (int i = 0; i < 12; ++i) len += std::snprintf(out + len, sizeof out - len, " %.6f", m.m[i / 4][i % 4]);
                    reply = out;
                }
            } else if (Is(buf, "win", &rest)) {
                reply = winCommand(rest);
            } else if (Is(buf, "layout", &rest)) {
                if (!std::strcmp(rest, "full") || !std::strcmp(rest, "top")) p.compact = rest[0] == 't', dirty = true;
                else reply = "error usage: layout full|top";
            } else if (Is(buf, "ping")) {
                reply = visible ? "ok shown" : "ok hidden";
            } else {
                reply = "error unknown command";
            }
            if (fromLen > offsetof(sockaddr_un, sun_path))
                sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
        if (!noVr) {
            std::lock_guard<std::mutex> lk(g_vr);
            vr::VREvent_t ev;
            while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
                if (ev.eventType == vr::VREvent_Quit) {
                    vr::VRSystem()->AcknowledgeQuit_Exiting();
                    g_stop = true;
                }
        }
        // The demo's next frame when it's due: drawn into the picture's clip rectangle alone,
        // unless something else changed too (then the whole picture). Hidden or paused, it waits.
        bool frameNow = false;
        if (p.demo && visible && !p.paused && NowNs() >= p.demo->due) {
            const int64_t now = NowNs();
            if (now - p.demo->due > 500'000'000) p.demo->due = now;  // after a wait: no catching up
            if (!p.demo->Next(p.demo->due))
                std::fprintf(stderr, "ft-handpanel: %s: %s\n", p.demo->path.c_str(), stbi_failure_reason()), p.demo.reset(), dirty = true;
            else frameNow = !dirty && p.demoW > 0;
        }
        const bool panelNow = visible && dirty, targetNow = targetDirty;
        if (panelNow) {
            Draw(p);
            if (ov) {
                buffers.Present(ov, h, p.pic);
                std::lock_guard<std::mutex> lk(g_vr);
                if (!shown) ov->ShowOverlay(h), shown = true;
            }
            dirty = false;
        } else if (frameNow) {
            DrawDemo(p.pic, *p.demo, p.demoX, p.demoY, p.demoW, p.demoH);
            const Area area{p.demoX, p.demoY, p.demoW, p.demoH};
            if (ov && shown) buffers.Present(ov, h, p.pic, &area);
        }
        if (targetNow) {
            if (t.on) {
                DrawTarget(t);
                if (ov) {
                    targetBuffers.Present(ov, th, t.pic);
                    std::lock_guard<std::mutex> lk(g_vr);
                    if (!targetShown) ov->ShowOverlay(th), targetShown = true;
                }
            }
            targetDirty = false;
        }
        // The windows and their ghosts that changed. A picture of a new size gets buffers of its size.
        for (int k = 0; k < kWinMax; ++k) {
            Win &w = wins[k];
            if (w.on && w.dirty) {
                DrawWin(w);
                if (ov) {
                    if (winBuffers[k].w != w.pic.w || winBuffers[k].h != w.pic.h) {  // (a failed Make keeps the size: SetOverlayRaw then)
                        std::lock_guard<std::mutex> lk(g_vr);  // Make and Drop call OpenVR
                        winBuffers[k].Drop();
                        if (winBuffers[k].Make(w.pic.w, w.pic.h)) ov->SetOverlayFlag(w.oh, vr::VROverlayFlags_IsPremultiplied, true);
                    }
                    winBuffers[k].Present(ov, w.oh, w.pic);
                    std::lock_guard<std::mutex> lk(g_vr);
                    if (!w.shown) ov->ShowOverlay(w.oh), w.shown = true;
                }
                if (noVr && !dumpDir.empty()) Dump(w.pic, dumpDir + "/win-" + w.id + ".pam");
                w.dirty = false;
            }
            if (w.on && w.ghostOn && w.ghostDirty) {
                DrawGhost(w);
                if (ov) {
                    if (ghostBuffers[k].w != w.ghostPic.w || ghostBuffers[k].h != w.ghostPic.h) {  // (a failed Make keeps the size: SetOverlayRaw then)
                        std::lock_guard<std::mutex> lk(g_vr);  // Make and Drop call OpenVR
                        ghostBuffers[k].Drop();
                        if (ghostBuffers[k].Make(w.ghostPic.w, w.ghostPic.h)) ov->SetOverlayFlag(w.gh, vr::VROverlayFlags_IsPremultiplied, true);
                    }
                    ghostBuffers[k].Present(ov, w.gh, w.ghostPic);
                    std::lock_guard<std::mutex> lk(g_vr);
                    if (!w.ghostShown) ov->ShowOverlay(w.gh), w.ghostShown = true;
                }
                if (noVr && !dumpDir.empty()) Dump(w.ghostPic, dumpDir + "/ghost-" + w.id + ".pam");
                w.ghostDirty = false;
            }
        }
        if (noVr && winsChanged) std::printf("--- windows\n"), PrintWins(wins);
        winsChanged = false;
        if (noVr && (panelNow || targetNow)) {
            Print(p, t, visible, ++pictures);
            if (!dumpDir.empty() && panelNow) Dump(p.pic, dumpDir + "/panel.pam");
            if (!dumpDir.empty() && targetNow && t.on) Dump(t.pic, dumpDir + "/target.pam");
        }
    }
    poseLog.Stop();
    if (ov) {
        ov->DestroyOverlay(h);
        ov->DestroyOverlay(th);
        buffers.Drop();
        targetBuffers.Drop();
        for (int k = 0; k < kWinMax; ++k) {
            if (wins[k].oh != vr::k_ulOverlayHandleInvalid) ov->DestroyOverlay(wins[k].oh);
            if (wins[k].gh != vr::k_ulOverlayHandleInvalid) ov->DestroyOverlay(wins[k].gh);
            winBuffers[k].Drop();
            ghostBuffers[k].Drop();
        }
        vr::VR_Shutdown();
    }
    return 0;
}
