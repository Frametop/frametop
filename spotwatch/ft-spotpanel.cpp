// ft-spotpanel: spotwatch's search panel (spotwatch/ft-spotwatch drives it; docs/spotwatch.md).
// A SteamVR overlay that opens straight ahead of where your head points and stays there in the
// room: a glass search pill, and under it a card of results, or of an app's launch options.
// It's drawn on the CPU and holds no state of its own beyond the picture: ft-spotwatch sends it
// everything to show, and it sends back what the lasers and the 3D mouse do on it.
//
// "Glass" is faux: SteamVR's overlays have alpha but no backdrop blur, so the pill and the card
// are a translucent dark tint with a soft top-to-bottom sheen, a bright rim fading down their
// edges, and a shadow outside them.
//
// Control socket: abstract unix datagram "@ft_spotpanel" (--socket NAME); a sender with an
// address gets "ok" or "error ...":
//   show          place it in front of you (head direction, at --distance) and show it with
//                 its next picture
//   show toast    the same, for a toast: the lasers pass through it
//   hide
//   state\n<lines>   what to show, the whole of it, one line each, fields split by tabs:
//       query <text>                   what's typed (empty: the placeholder)
//       toast <text>                   just this, in the pill (where a pinned panel is)
//       hint <text>                    a line in the card when there are no rows
//       sel <row>                      the selected row (-1: none)
//       row <kind> <icon> <title> <subtitle> <badge>
//                                      kind: item, header (a menu's top row: back on a click),
//                                      or section (a label, not selectable); icon: the path of
//                                      a 64x64 RGBA file ft-spotwatch made, or "-"
//   ping          -> "ok shown" or "ok hidden"
// To ft-spotwatch (@ft_spotwatch, --notify NAME): "hover <row>", "click <row>", "menu <row>"
// (a right click), "back" (the header), "scroll <+1|-1>", and "outside" (a click on the panel
// but on no row or the pill, which the panel's clear part catches: it closes the search).
//
// Pictures go into the next of three shared buffers (linear DMA-BUFs SteamVR imported once),
// as screens/keyboard.cpp and gaze/panel/ft-gazepanel.cpp do; SetOverlayRaw is the fallback.
//
// Options: --watch-stdin (quit when stdin closes: ft-spotwatch runs it), --socket NAME,
// --notify NAME, --distance METRES, --width METRES; --render FILE draws the state on stdin
// (the lines after "state") into FILE as raw RGBA, 1120x720, and quits: no SteamVR (a look at
// the picture: spotwatch/tools/render.py). Runs in the container (screens/build.sh
// builds it into screens/build).
#include <openvr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

// The canvas: the pill along the top, the card under it, the rest clear. It keeps one size, so
// the pill stays where it opened as the card grows and shrinks.
constexpr int kW = 1120, kH = 720;
constexpr int kMargin = 24;                         // room for the shadow
constexpr int kPillTop = 20, kPillH = 104;          // the pill
constexpr int kGap = 14;                            // between the pill and the card
constexpr int kCardTop = kPillTop + kPillH + kGap;  // the card
constexpr int kPad = 10;                            // inside the card, around the rows
constexpr int kRowH = 74;
constexpr int kMaxRows = (kH - kCardTop - 2 * kPad - 16) / kRowH;  // 7
constexpr int kIcon = 64;                           // icon files are this square
std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------- text (as screens/keyboard.cpp)

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
    if (!g_fontOk) std::fprintf(stderr, "ft-spotpanel: no font (%s); no text\n", path.c_str());
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

int Width(const std::vector<uint32_t> &cps, int size) {
    int w = 0;
    for (uint32_t cp : cps) w += GetGlyph(cp, size).advance;
    return w;
}

// ---------------------------------------------------------------- the picture

struct Canvas {
    std::vector<uint8_t> px = std::vector<uint8_t>(size_t(kW) * kH * 4);
};

struct Color {
    double r, g, b;
};
constexpr Color kWhite{1, 1, 1}, kFaint{0.72, 0.74, 0.80}, kDim{0.55, 0.57, 0.63}, kAccent{0.30, 0.56, 1.0};

void Blend(Canvas &c, int x, int y, Color k, double a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0) return;
    uint8_t *q = &c.px[(size_t(y) * kW + x) * 4];
    a = std::min(a, 1.0);
    // Over: straight alpha in, straight alpha out.
    const double da = q[3] / 255.0, oa = a + da * (1 - a);
    if (oa <= 0) return;
    const double src[3] = {k.r * 255, k.g * 255, k.b * 255};
    for (int i = 0; i < 3; ++i) q[i] = uint8_t(std::lround((src[i] * a + q[i] * da * (1 - a)) / oa));
    q[3] = uint8_t(std::lround(oa * 255));
}

// Signed distance from (px, py) to a rounded rectangle (negative inside).
double SdRound(double px, double py, double x0, double y0, double x1, double y1, double r) {
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    const double qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

double Smooth(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// A plain rounded rectangle (the selection, the hover).
void Round(Canvas &c, double x0, double y0, double x1, double y1, double r, Color k, double a) {
    for (int y = int(y0) - 1; y <= int(y1) + 1; ++y)
        for (int x = int(x0) - 1; x <= int(x1) + 1; ++x) {
            const double d = SdRound(x + 0.5, y + 0.5, x0, y0, x1, y1, r);
            Blend(c, x, y, k, a * std::clamp(0.5 - d, 0.0, 1.0));
        }
}

// A pane of faux glass (see the top): shadow outside, tinted fill with a sheen, a rim.
void Glass(Canvas &c, double x0, double y0, double x1, double y1, double r) {
    constexpr double kShadow = 18, kDrop = 7;
    for (int y = int(y0 - kShadow); y <= int(y1 + kShadow + kDrop); ++y)
        for (int x = int(x0 - kShadow); x <= int(x1 + kShadow); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double d = SdRound(px, py, x0, y0, x1, y1, r);
            const double t = std::clamp((py - y0) / (y1 - y0), 0.0, 1.0);
            if (d > -0.5) {  // the shadow, only where the glass isn't
                const double ds = SdRound(px, py - kDrop, x0, y0, x1, y1, r);
                Blend(c, x, y, {0, 0, 0}, 0.32 * (1 - Smooth(-4, kShadow, ds)) * std::clamp(d + 0.5, 0.0, 1.0));
            }
            const double cover = std::clamp(0.5 - d, 0.0, 1.0);
            if (cover <= 0) continue;
            const Color fill{0.20 - 0.09 * t, 0.21 - 0.09 * t, 0.26 - 0.10 * t};
            Blend(c, x, y, fill, 0.72 * cover);
            Blend(c, x, y, kWhite, 0.07 * (1 - t) * (1 - t) * cover);  // the sheen, brightest at the top
            const double rim = std::clamp(1.2 - std::fabs(d + 1.2), 0.0, 1.0);
            Blend(c, x, y, kWhite, rim * (0.42 * (1 - t) + 0.10 * t));
        }
}

// Text from x (left), centred on cy, cut with an ellipsis at maxW. Returns its right edge.
int Text(Canvas &c, const std::string &s, int size, int x, int cy, Color k, double a = 1, int maxW = 1 << 20) {
    if (!g_fontOk || s.empty()) return x;
    auto cps = Codepoints(s);
    if (Width(cps, size) > maxW) {
        const int ell = GetGlyph(0x2026, size).advance;
        while (!cps.empty() && Width(cps, size) + ell > maxW) cps.pop_back();
        while (!cps.empty() && cps.back() == ' ') cps.pop_back();
        cps.push_back(0x2026);
    }
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : cps) {
        const Glyph &gl = GetGlyph(cp, size);
        for (int gy = 0; gy < gl.h; ++gy)
            for (int gx = 0; gx < gl.w; ++gx)
                Blend(c, x + gl.xoff + gx, baseline + gl.yoff + gy, k, a * gl.bitmap[size_t(gy) * gl.w + gx] / 255.0);
        x += gl.advance;
    }
    return x;
}

int TextWidth(const std::string &s, int size) { return g_fontOk ? Width(Codepoints(s), size) : 0; }

// The search glass in the pill: a ring and a handle.
void Magnifier(Canvas &c, double cx, double cy, Color k) {
    const double R = 13, w = 3.2;
    const double hx0 = cx + R * 0.72, hy0 = cy + R * 0.72, hx1 = cx + R * 1.55, hy1 = cy + R * 1.55;
    for (int y = int(cy - R - 4); y <= int(hy1 + 4); ++y)
        for (int x = int(cx - R - 4); x <= int(hx1 + 4); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double ring = std::fabs(std::hypot(px - cx, py - cy) - R) - w / 2;
            // Distance to the handle's segment.
            const double vx = hx1 - hx0, vy = hy1 - hy0;
            const double t = std::clamp(((px - hx0) * vx + (py - hy0) * vy) / (vx * vx + vy * vy), 0.0, 1.0);
            const double handle = std::hypot(px - hx0 - t * vx, py - hy0 - t * vy) - w * 0.65;
            Blend(c, x, y, k, std::clamp(0.5 - std::min(ring, handle), 0.0, 1.0));
        }
}

// Icons: 64x64 RGBA files ft-spotwatch rasterized, kept once loaded.
std::map<std::string, std::vector<uint8_t>> g_icons;

const std::vector<uint8_t> *Icon(const std::string &path) {
    if (path.empty() || path == "-") return nullptr;
    auto it = g_icons.find(path);
    if (it == g_icons.end()) {
        std::vector<uint8_t> px(size_t(kIcon) * kIcon * 4);
        FILE *f = std::fopen(path.c_str(), "rb");
        const bool ok = f && std::fread(px.data(), 1, px.size(), f) == px.size();
        if (f) std::fclose(f);
        if (!ok) px.clear();
        if (g_icons.size() > 256) g_icons.clear();
        it = g_icons.emplace(path, std::move(px)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

// An icon at (x, y), size pixels square, bilinear.
void DrawIcon(Canvas &c, const std::vector<uint8_t> &px, int x, int y, int size) {
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const double u = std::clamp((i + 0.5) * kIcon / size - 0.5, 0.0, kIcon - 1.0);
            const double v = std::clamp((j + 0.5) * kIcon / size - 0.5, 0.0, kIcon - 1.0);
            const int u0 = int(u), v0 = int(v), u1 = std::min(u0 + 1, kIcon - 1), v1 = std::min(v0 + 1, kIcon - 1);
            const double fu = u - u0, fv = v - v0;
            double acc[4] = {0, 0, 0, 0};
            const int us[2] = {u0, u1}, vs[2] = {v0, v1};
            for (int b = 0; b < 2; ++b)
                for (int a = 0; a < 2; ++a) {
                    const double wgt = (a ? fu : 1 - fu) * (b ? fv : 1 - fv);
                    const uint8_t *p = &px[(size_t(vs[b]) * kIcon + us[a]) * 4];
                    const double al = p[3] / 255.0 * wgt;  // premultiplied while mixing
                    acc[0] += p[0] * al, acc[1] += p[1] * al, acc[2] += p[2] * al, acc[3] += al;
                }
            if (acc[3] <= 0) continue;
            Blend(c, x + i, y + j, {acc[0] / acc[3] / 255, acc[1] / acc[3] / 255, acc[2] / acc[3] / 255}, acc[3]);
        }
}

struct Row {
    std::string kind, icon, title, sub, badge;
};

struct State {
    std::string query, hint, toast;
    int sel = -1, hover = -1;
    std::vector<Row> rows;
};

int CardBottom(const State &s) {
    if (!s.toast.empty()) return kCardTop;
    const int n = std::min(int(s.rows.size()), kMaxRows);
    if (n == 0) return s.hint.empty() ? kCardTop : kCardTop + 2 * kPad + kRowH;
    return kCardTop + 2 * kPad + n * kRowH;
}

void Draw(Canvas &c, const State &s) {
    const double px0 = kMargin, px1 = kW - kMargin, py0 = kPillTop, py1 = kPillTop + kPillH, cy = (py0 + py1) / 2;
    const int bottom = CardBottom(s);
    // The glass (the slow part) is drawn again only when the card's height changes.
    static Canvas glass;
    static int glassBottom = -1;
    const int key = s.toast.empty() ? bottom : -2;
    if (glassBottom != key) {
        std::fill(glass.px.begin(), glass.px.end(), 0);
        Glass(glass, px0, py0, px1, py1, kPillH / 2.0);
        if (s.toast.empty()) Magnifier(glass, px0 + 52, cy - 3, kFaint);
        if (bottom != kCardTop) Glass(glass, px0, kCardTop, px1, bottom, 28);
        glassBottom = key;
    }
    c.px = glass.px;
    if (!s.toast.empty()) {
        const int size = 36, w = std::min(TextWidth(s.toast, size), int(px1 - px0) - 80);
        Text(c, s.toast, size, kW / 2 - w / 2, int(cy), kWhite, 1, int(px1 - px0) - 80);
        return;
    }
    const int tx = int(px0) + 92, tmax = int(px1 - px0) - 92 - 40, tsize = 44;
    if (s.query.empty()) {
        Text(c, "Spotwatch Search", tsize, tx, int(cy), kDim);
        Round(c, tx - 2, cy - 24, tx + 1, cy + 24, 1.5, kAccent, 0.95);  // the caret
    } else {
        // Long queries show their end, where you type.
        std::string q = s.query;
        auto cps = Codepoints(q);
        while (cps.size() > 1 && Width(cps, tsize) > tmax) cps.erase(cps.begin());
        std::string shown;
        for (uint32_t cp : cps) {
            char b[5] = {};
            if (cp < 0x80) b[0] = char(cp);
            else if (cp < 0x800) b[0] = char(0xC0 | cp >> 6), b[1] = char(0x80 | (cp & 0x3F));
            else if (cp < 0x10000)
                b[0] = char(0xE0 | cp >> 12), b[1] = char(0x80 | (cp >> 6 & 0x3F)), b[2] = char(0x80 | (cp & 0x3F));
            else
                b[0] = char(0xF0 | cp >> 18), b[1] = char(0x80 | (cp >> 12 & 0x3F)), b[2] = char(0x80 | (cp >> 6 & 0x3F)),
                b[3] = char(0x80 | (cp & 0x3F));
            shown += b;
        }
        const int end = Text(c, shown, tsize, tx, int(cy), kWhite);
        Round(c, end + 3, cy - 24, end + 6, cy + 24, 1.5, kAccent, 0.95);
    }
    // The card.
    if (bottom == kCardTop) return;
    if (s.rows.empty()) {
        Text(c, s.hint, 30, int(px0) + 32, kCardTop + kPad + kRowH / 2, kDim, 1, int(px1 - px0) - 64);
        return;
    }
    const int n = std::min(int(s.rows.size()), kMaxRows);
    for (int i = 0; i < n; ++i) {
        const Row &r = s.rows[size_t(i)];
        const int y0 = kCardTop + kPad + i * kRowH, mid = y0 + kRowH / 2;
        const double rx0 = px0 + kPad, rx1 = px1 - kPad;
        if (r.kind == "section") {
            Text(c, r.title, 22, int(rx0) + 22, mid + 10, kDim, 1, int(rx1 - rx0) - 44);
            Round(c, rx0 + 22, y0 + 10, rx1 - 22, y0 + 11, 0.5, kWhite, 0.12);
            continue;
        }
        if (i == s.sel) Round(c, rx0, y0 + 2, rx1, y0 + kRowH - 2, 18, kAccent, 0.62);
        else if (i == s.hover) Round(c, rx0, y0 + 2, rx1, y0 + kRowH - 2, 18, kWhite, 0.08);
        int x = int(rx0) + 16;
        if (r.kind == "header") {
            x = Text(c, "‹", 40, x, mid - 3, kFaint) + 12;  // back
        }
        if (const auto *icon = Icon(r.icon)) DrawIcon(c, *icon, x, mid - 26, 52);
        x += 52 + 18;
        const int badgeW = r.badge.empty() ? 0 : TextWidth(r.badge, 22) + 24;
        const int maxW = int(rx1) - 16 - badgeW - x;
        if (r.sub.empty()) {
            Text(c, r.title, 30, x, mid, kWhite, 1, maxW);
        } else {
            Text(c, r.title, 30, x, mid - 13, kWhite, 1, maxW);
            Text(c, r.sub, 21, x, mid + 18, i == s.sel ? Color{0.88, 0.91, 1.0} : kFaint, 1, maxW);
        }
        if (badgeW) Text(c, r.badge, 22, int(rx1) - 16 - badgeW + 12, mid, i == s.sel ? kWhite : kDim);
    }
}

// What's at canvas pixel (x, y): a row (>= 0), the pill (-2), or nothing (-1).
int HitTest(const State &s, double x, double y) {
    if (x >= kMargin && x < kW - kMargin && y >= kPillTop && y < kPillTop + kPillH) return -2;
    if (x < kMargin + kPad || x >= kW - kMargin - kPad) return -1;
    const int n = std::min(int(s.rows.size()), kMaxRows);
    const int i = int(std::floor((y - kCardTop - kPad) / kRowH));
    if (y < kCardTop + kPad || i < 0 || i >= n) return y >= kCardTop && y < CardBottom(s) ? -3 : -1;
    return i;
}

// ---------------------------------------------------------------- the buffers (as ft-gazepanel)

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
};

struct Buffers {
    int drm = -1;
    gbm_device *gbm = nullptr;
    Buffer b[3];
    int next = 0;
    bool ok = false;

    bool Make() {
        drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm >= 0) gbm = gbm_create_device(drm);
        for (Buffer &x : b) {
            if (gbm) x.bo = gbm_bo_create(gbm, kW, kH, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!x.bo || (x.fd = gbm_bo_get_fd(x.bo)) < 0) break;
            vr::DmabufAttributes_t a{};
            a.unWidth = kW, a.unHeight = kH;
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
            std::fprintf(stderr, "ft-spotpanel: no shared buffers; falling back to SetOverlayRaw (it flickers)\n");
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

    void Present(vr::IVROverlay *ov, vr::VROverlayHandle_t h, const Canvas &c) {
        vr::EVROverlayError e;
        if (!ok) {
            e = ov->SetOverlayRaw(h, const_cast<uint8_t *>(c.px.data()), uint32_t(kW), uint32_t(kH), 4);
        } else {
            Buffer &x = b[next];
            next = (next + 1) % 3;
            uint32_t stride = 0;
            void *mapping = nullptr;
            auto *dst = static_cast<uint8_t *>(gbm_bo_map(x.bo, 0, 0, kW, kH, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
            if (!dst) {
                std::fprintf(stderr, "ft-spotpanel: can't map a buffer\n");
                return;
            }
            for (int y = 0; y < kH; ++y) {
                const uint8_t *src = &c.px[size_t(y) * kW * 4];
                uint8_t *row = dst + size_t(y) * stride;
                for (int i = 0; i < kW * 4; i += 4) {
                    const unsigned a = src[i + 3];
                    row[i] = uint8_t(src[i] * a / 255), row[i + 1] = uint8_t(src[i + 1] * a / 255);
                    row[i + 2] = uint8_t(src[i + 2] * a / 255), row[i + 3] = uint8_t(a);
                }
            }
            gbm_bo_unmap(x.bo, mapping);
            vr::Texture_t tex = {&x.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
            e = ov->SetOverlayTexture(h, &tex);
        }
        if (e != vr::VROverlayError_None)
            std::fprintf(stderr, "ft-spotpanel: the picture didn't go to SteamVR: %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
};

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) return out;
        at = next + 1;
    }
}

void ParseState(const std::string &text, State &s) {
    const int hover = s.hover;
    s = State{};
    s.hover = hover;
    for (const std::string &line : Split(text, '\n')) {
        const auto f = Split(line, '\t');
        if (f[0] == "query" && f.size() > 1) s.query = f[1];
        else if (f[0] == "toast" && f.size() > 1) s.toast = f[1];
        else if (f[0] == "hint" && f.size() > 1) s.hint = f[1];
        else if (f[0] == "sel" && f.size() > 1) s.sel = std::atoi(f[1].c_str());
        else if (f[0] == "row" && f.size() >= 6) s.rows.push_back({f[1], f[2], f[3], f[4], f[5]});
    }
    if (s.hover >= int(s.rows.size())) s.hover = -1;
}

}  // namespace

int main(int argc, char **argv) {
    bool watchStdin = false;
    std::string sockName = "ft_spotpanel", notifyName = "ft_spotwatch", renderPath;
    double distance = 1.0, width = 0.56;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--watch-stdin")) watchStdin = true;
        else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) sockName = argv[++i];
        else if (!std::strcmp(argv[i], "--notify") && i + 1 < argc) notifyName = argv[++i];
        else if (!std::strcmp(argv[i], "--distance") && i + 1 < argc) distance = std::clamp(std::atof(argv[++i]), 0.4, 3.0);
        else if (!std::strcmp(argv[i], "--width") && i + 1 < argc) width = std::clamp(std::atof(argv[++i]), 0.2, 2.0);
        else if (!std::strcmp(argv[i], "--render") && i + 1 < argc) renderPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--watch-stdin] [--socket NAME] [--notify NAME] [--distance M] [--width M]\n",
                         argv[0]);
            return 2;
        }
    }
    if (!renderPath.empty()) {
        std::string text;
        char chunk[4096];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, stdin)) > 0;) text.append(chunk, n);
        LoadFont();
        State state;
        ParseState(text, state);
        Canvas canvas;
        Draw(canvas, state);
        FILE *f = std::fopen(renderPath.c_str(), "wb");
        const bool ok = f && std::fwrite(canvas.px.data(), 1, canvas.px.size(), f) == canvas.px.size();
        if (f) std::fclose(f);
        return ok ? 0 : 1;
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
        std::fprintf(stderr, "ft-spotpanel: @%s is taken (another copy running?)\n", sockName.c_str());
        return 1;
    }
    sockaddr_un notify{};
    notify.sun_family = AF_UNIX;
    std::memcpy(notify.sun_path + 1, notifyName.data(), std::min(notifyName.size(), sizeof notify.sun_path - 2));
    const socklen_t notifyLen = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + notifyName.size());
    auto tell = [&](const std::string &msg) {
        sendto(sock, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&notify), notifyLen);
    };

    // As Frametop's other SteamVR clients: background first, so we never start vrserver.
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_None) {
        vr::VR_Shutdown();
        vr::VR_Init(&err, vr::VRApplication_Overlay);
    }
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "ft-spotpanel: SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    if (ov->CreateOverlay("frametop.spotwatch", "Frametop spotwatch", &h) != vr::VROverlayError_None) {
        std::fprintf(stderr, "ft-spotpanel: can't create the overlay (another copy running?)\n");
        return 1;
    }
    // Over the screens and their controls (10), under Frametop's keyboard (15), which may open
    // with it, and the 3D mouse's dot (200).
    ov->SetOverlaySortOrder(h, 14);
    ov->SetOverlayWidthInMeters(h, float(width));
    ov->SetOverlayInputMethod(h, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t mouseScale{{float(kW), float(kH)}};
    ov->SetOverlayMouseScale(h, &mouseScale);
    ov->SetOverlayFlag(h, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    LoadFont();
    Buffers buffers;
    if (buffers.Make()) ov->SetOverlayFlag(h, vr::VROverlayFlags_IsPremultiplied, true);

    Canvas canvas;
    State state;
    bool visible = false, dirty = false, shown = false;  // shown: SteamVR shows it (after its first picture)
    // Straight ahead of the head, at `distance`, upright and facing you; the pill's middle on
    // that line, the card hanging under it.
    auto place = [&]() -> bool {
        vr::TrackedDevicePose_t poses[1];
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, 1);
        if (!poses[0].bPoseIsValid) return false;
        const vr::HmdMatrix34_t &m = poses[0].mDeviceToAbsoluteTracking;
        double f[3] = {-m.m[0][2], -m.m[1][2], -m.m[2][2]};
        // Keep it upright: no more than 60 degrees up or down.
        const double flat = std::hypot(f[0], f[2]);
        if (flat < 1e-6) return false;
        double pitch = std::clamp(std::atan2(f[1], flat), -M_PI / 3, M_PI / 3);
        const double hx = f[0] / flat, hz = f[2] / flat;
        f[0] = hx * std::cos(pitch), f[1] = std::sin(pitch), f[2] = hz * std::cos(pitch);
        const double x[3] = {-hz, 0, hx};  // right, level
        const double z[3] = {-f[0], -f[1], -f[2]};  // towards you
        const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        const double mpp = width / kW, down = (kH / 2.0 - (kPillTop + kPillH / 2.0)) * mpp;
        vr::HmdMatrix34_t t{};
        for (int r = 0; r < 3; ++r) {
            t.m[r][0] = float(x[r]), t.m[r][1] = float(y[r]), t.m[r][2] = float(z[r]);
            t.m[r][3] = float(m.m[r][3] + f[r] * distance - y[r] * down);
        }
        ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &t);
        return true;
    };
    std::fprintf(stderr, "ft-spotpanel running: @%s, %.2f m away, %.2f m wide\n", sockName.c_str(), distance, width);

    std::vector<char> buf(65536);
    while (!g_stop) {
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf.data(), buf.size() - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[size_t(n)] = 0;
            const std::string cmd(buf.data(), size_t(n));
            std::string reply = "ok";
            if (cmd == "show" || cmd == "show toast") {
                if (place()) {
                    state.hover = -1;
                    visible = dirty = true;  // shown with its next picture
                    const bool toast = cmd != "show";
                    ov->SetOverlayInputMethod(h, toast ? vr::VROverlayInputMethod_None : vr::VROverlayInputMethod_Mouse);
                    ov->SetOverlayFlag(h, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, !toast);
                } else {
                    reply = "error no head pose (headset off?)";
                }
            } else if (cmd == "hide") {
                ov->SetOverlayFlag(h, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
                ov->HideOverlay(h);
                visible = shown = false;
            } else if (!cmd.compare(0, 6, "state\n") || cmd == "state") {
                ParseState(cmd.size() > 6 ? cmd.substr(6) : "", state);
                dirty = true;
            } else if (cmd == "ping") {
                reply = visible ? "ok shown" : "ok hidden";
            } else {
                reply = "error unknown command";
            }
            if (fromLen > offsetof(sockaddr_un, sun_path))
                sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                g_stop = true;
            }
        while (ov->PollNextOverlayEvent(h, &ev, sizeof ev)) {
            if (!visible) continue;
            const double x = ev.data.mouse.x, y = kH - ev.data.mouse.y;  // OpenVR's mouse origin is bottom left
            switch (ev.eventType) {
                case vr::VREvent_MouseMove: {
                    int k = HitTest(state, x, y);
                    if (k >= 0 && state.rows[size_t(k)].kind == "section") k = -1;
                    if (k != state.hover) {
                        state.hover = k >= 0 ? k : -1, dirty = true;
                        if (k >= 0) tell("hover " + std::to_string(k));
                    }
                    break;
                }
                case vr::VREvent_MouseButtonDown: {
                    const int k = HitTest(state, x, y);
                    const bool right = ev.data.mouse.button == vr::VRMouseButton_Right;
                    if (k >= 0 && state.rows[size_t(k)].kind == "header") tell("back");
                    else if (k >= 0 && state.rows[size_t(k)].kind != "section")
                        tell((right ? "menu " : "click ") + std::to_string(k));
                    else if (k == -1) tell("outside");
                    break;
                }
                case vr::VREvent_ScrollDiscrete:
                    if (ev.data.scroll.ydelta != 0) tell(ev.data.scroll.ydelta > 0 ? "scroll -1" : "scroll +1");
                    break;
                case vr::VREvent_FocusLeave:
                    if (state.hover >= 0) state.hover = -1, dirty = true;
                    break;
                default:
                    break;
            }
        }
        if (visible && dirty) {
            Draw(canvas, state);
            buffers.Present(ov, h, canvas);
            if (!shown) ov->ShowOverlay(h), shown = true;
            dirty = false;
        }
        // Until a command comes, or 11 ms while shown (SteamVR's events). Hidden, up to a second.
        pollfd fds[2] = {{sock, POLLIN, 0}, {0, POLLIN, 0}};
        poll(fds, watchStdin ? 2 : 1, visible ? 11 : 1000);
    }
    ov->DestroyOverlay(h);
    buffers.Drop();
    vr::VR_Shutdown();
    return 0;
}
