// Frametop's keyboard: the panel ft-screens shows for a text field on the desktop (see
// ft_vr_keyboard_show in vr.cpp). A US laptop layout in five rows, with Esc where Caps Lock
// would be and a Close key by the arrows.
//   - The keys send linux key codes, which ft-screens types into the focused screen through
//     its seat, so they mean what they'd mean on a real keyboard. A key goes down on the
//     press and up on the release, so KWin repeats a held key.
//   - Shift, Ctrl and Alt latch: tap one and it's held for the next key; tap it again to let
//     go. The labels follow Shift.
//   - A grab bar along the top edge moves it, like a screen's bar: press it with any laser
//     (or the 3D mouse) and the keyboard follows that device rigidly until the release.
//   - The panel is drawn on the CPU: rounded keys, labels from Noto Sans (the container's,
//     found with fc-match) rasterized with stb_truetype, and arrows drawn as triangles
//     (Noto Sans has no arrow glyphs). It's drawn again when the key under a pointer, a
//     pressed key, or a latch changes, into the next of three shared buffers (linear
//     DMA-BUFs SteamVR imported once), and the panel switches to it. SetOverlayRaw, which
//     uploads a new texture each time, left the panel blank for a moment on every change;
//     it's only the fallback if the buffers can't be made.
#include "keyboard.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace keyboard {
namespace {

constexpr int kW = 1280, kH = 470;     // texture pixels
constexpr int kGrip = 40;              // the grab bar's band along the top
constexpr double kWidthMetres = 0.7;   // the panel's width in the room
constexpr int kMargin = 12, kGap = 7;  // around the keys, between keys (pixels)
constexpr int kRows = 5, kColumns = 15;  // row width in key units
constexpr uint32_t kHide = 0;          // the Close key's code

struct KeyDef {
    uint32_t code;
    const char *label, *shifted;  // shifted: nullptr = the same
    double units;                 // width
};

// One row after another, 15 units each.
const std::vector<std::vector<KeyDef>> kLayout = {
    {{KEY_GRAVE, "`", "~", 1}, {KEY_1, "1", "!", 1}, {KEY_2, "2", "@", 1}, {KEY_3, "3", "#", 1},
     {KEY_4, "4", "$", 1}, {KEY_5, "5", "%", 1}, {KEY_6, "6", "^", 1}, {KEY_7, "7", "&", 1},
     {KEY_8, "8", "*", 1}, {KEY_9, "9", "(", 1}, {KEY_0, "0", ")", 1}, {KEY_MINUS, "-", "_", 1},
     {KEY_EQUAL, "=", "+", 1}, {KEY_BACKSPACE, "Backspace", nullptr, 2}},
    {{KEY_TAB, "Tab", nullptr, 1.5}, {KEY_Q, "q", "Q", 1}, {KEY_W, "w", "W", 1}, {KEY_E, "e", "E", 1},
     {KEY_R, "r", "R", 1}, {KEY_T, "t", "T", 1}, {KEY_Y, "y", "Y", 1}, {KEY_U, "u", "U", 1},
     {KEY_I, "i", "I", 1}, {KEY_O, "o", "O", 1}, {KEY_P, "p", "P", 1}, {KEY_LEFTBRACE, "[", "{", 1},
     {KEY_RIGHTBRACE, "]", "}", 1}, {KEY_BACKSLASH, "\\", "|", 1.5}},
    {{KEY_ESC, "Esc", nullptr, 1.75}, {KEY_A, "a", "A", 1}, {KEY_S, "s", "S", 1}, {KEY_D, "d", "D", 1},
     {KEY_F, "f", "F", 1}, {KEY_G, "g", "G", 1}, {KEY_H, "h", "H", 1}, {KEY_J, "j", "J", 1},
     {KEY_K, "k", "K", 1}, {KEY_L, "l", "L", 1}, {KEY_SEMICOLON, ";", ":", 1},
     {KEY_APOSTROPHE, "'", "\"", 1}, {KEY_ENTER, "Enter", nullptr, 2.25}},
    {{KEY_LEFTSHIFT, "Shift", nullptr, 1.25}, {KEY_Z, "z", "Z", 1}, {KEY_X, "x", "X", 1},
     {KEY_C, "c", "C", 1}, {KEY_V, "v", "V", 1}, {KEY_B, "b", "B", 1}, {KEY_N, "n", "N", 1},
     {KEY_M, "m", "M", 1}, {KEY_COMMA, ",", "<", 1}, {KEY_DOT, ".", ">", 1}, {KEY_SLASH, "/", "?", 1},
     {KEY_RIGHTSHIFT, "Shift", nullptr, 1.75}, {KEY_UP, "", nullptr, 1}, {KEY_DELETE, "Del", nullptr, 1}},
    {{KEY_LEFTCTRL, "Ctrl", nullptr, 1.5}, {KEY_LEFTALT, "Alt", nullptr, 1.5}, {KEY_SPACE, "", nullptr, 7},
     {kHide, "\u00d7  Close", nullptr, 2}, {KEY_LEFT, "", nullptr, 1}, {KEY_DOWN, "", nullptr, 1},
     {KEY_RIGHT, "", nullptr, 1}},
};

struct Key {
    KeyDef def;
    int x0, y0, x1, y1;  // texture pixels, top left origin
};

enum Mod { Shift, Ctrl, Alt, kMods };
const uint32_t kModCode[kMods] = {KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT};

int ModOf(uint32_t code) {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return Shift;
        case KEY_LEFTCTRL: return Ctrl;
        case KEY_LEFTALT: return Alt;
        default: return -1;
    }
}

vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
bool g_shown = false;
std::vector<Key> g_keys;
std::vector<uint8_t> g_px;
int g_hover = -1;              // key under a pointer
bool g_hoverGrip = false;      // the grab bar is under a pointer
vr::TrackedDeviceIndex_t g_dragDevice = vr::k_unTrackedDeviceIndexInvalid;  // carrying it
vr::HmdMatrix34_t g_dragRel{};   // device -> panel, while carried
vr::HmdMatrix34_t g_pose{};      // where it is
int g_down = -1;               // key held down
bool g_latched[kMods] = {};    // Shift, Ctrl, Alt held for the next key
bool g_dirty = true;           // draw again

// ---------------------------------------------------------------- labels

stbtt_fontinfo g_font;
std::vector<unsigned char> g_fontData;
bool g_fontOk = false;

struct Glyph {
    std::vector<unsigned char> bitmap;
    int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};
std::map<std::pair<uint32_t, int>, Glyph> g_glyphs;  // (codepoint, pixel size)

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
    if (!g_fontOk) std::printf("keyboard: no font (%s); the keys have no labels\n", path.c_str());
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

std::vector<uint32_t> Codepoints(const char *s) {
    std::vector<uint32_t> out;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*p & 0xC0) == 0x80; --more) c = c << 6 | (*p++ & 0x3F);
        out.push_back(c);
    }
    return out;
}

// Text centred on (cx, cy), white at `alpha`.
void DrawText(const char *text, int size, int cx, int cy, uint8_t alpha) {
    if (!g_fontOk || !*text) return;
    const auto cps = Codepoints(text);
    int width = 0;
    for (uint32_t cp : cps) width += GetGlyph(cp, size).advance;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    int x = cx - width / 2;
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : cps) {
        const Glyph &g = GetGlyph(cp, size);
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const int px = x + g.xoff + gx, py = baseline + g.yoff + gy;
                if (px < 0 || py < 0 || px >= kW || py >= kH) continue;
                const double a = g.bitmap[size_t(gy) * g.w + gx] / 255.0 * alpha / 255.0;
                uint8_t *p = &g_px[(size_t(py) * kW + px) * 4];
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] + (255 - p[c]) * a));
            }
        x += g.advance;
    }
}

// ---------------------------------------------------------------- drawing

// A white triangle pointing along (dx, dy) (one of them 0), centred on (cx, cy), `size` across.
void DrawArrow(int cx, int cy, int size, int dx, int dy, uint8_t alpha) {
    const double h = size / 2.0;
    const double pts[3][2] = {{h, 0}, {-h, -h}, {-h, h}};  // tip and base, pointing right
    double v[3][2];
    for (int i = 0; i < 3; ++i) {
        v[i][0] = cx + pts[i][0] * dx - pts[i][1] * dy;
        v[i][1] = cy + pts[i][0] * dy + pts[i][1] * dx;
    }
    auto inside = [&](double x, double y) {
        bool pos = false, neg = false;
        for (int i = 0; i < 3; ++i) {
            const double *a = v[i], *b = v[(i + 1) % 3];
            const double c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
            pos |= c > 0, neg |= c < 0;
        }
        return !(pos && neg);
    };
    for (int y = int(cy - h) - 1; y <= int(cy + h) + 1; ++y)
        for (int x = int(cx - h) - 1; x <= int(cx + h) + 1; ++x) {
            if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
            int n = 0;  // 4x4 samples, for smooth edges
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) n += inside(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
            if (!n) continue;
            const double a = n / 16.0 * alpha / 255.0;
            uint8_t *p = &g_px[(size_t(y) * kW + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] + (255 - p[c]) * a));
        }
}

void Layout() {
    g_keys.clear();
    const double unit = double(kW - 2 * kMargin) / kColumns, row = double(kH - kGrip - kMargin) / kRows;
    for (int r = 0; r < kRows; ++r) {
        double x = kMargin;
        for (const KeyDef &d : kLayout[r]) {
            Key k{d, int(std::lround(x + kGap / 2.0)), int(std::lround(kGrip + r * row + kGap / 2.0)),
                  int(std::lround(x + d.units * unit - kGap / 2.0)),
                  int(std::lround(kGrip + (r + 1) * row - kGap / 2.0))};
            g_keys.push_back(k);
            x += d.units * unit;
        }
    }
}

// A rounded rectangle over what's there (straight alpha, like the other panels).
void FillRounded(int x0, int y0, int x1, int y1, double radius, const uint8_t rgba[4]) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double cx = std::clamp(x + 0.5, x0 + radius, x1 - radius);
            const double cy = std::clamp(y + 0.5, y0 + radius, y1 - radius);
            const double cover = std::clamp(radius - std::hypot(x + 0.5 - cx, y + 0.5 - cy) + 0.5, 0.0, 1.0);
            if (cover <= 0) continue;
            uint8_t *p = &g_px[(size_t(y) * kW + x) * 4];
            const double a = rgba[3] / 255.0 * cover, below = p[3] / 255.0;
            const double out = a + below * (1 - a);
            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t(std::lround(out > 0 ? (rgba[c] * a + p[c] * below * (1 - a)) / out : 0));
            p[3] = uint8_t(std::lround(out * 255));
        }
}

// ---------------------------------------------------------------- buffers

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
};
int g_drm = -1;
gbm_device *g_gbm = nullptr;
Buffer g_buffers[3];
int g_next = 0;
bool g_useBuffers = false;

void DropBuffers() {
    for (Buffer &b : g_buffers) {
        if (b.handle) vr::VRIPCResourceManager()->UnrefResource(b.handle);
        if (b.fd >= 0) close(b.fd);
        if (b.bo) gbm_bo_destroy(b.bo);
        b = Buffer{};
    }
    if (g_gbm) gbm_device_destroy(g_gbm);
    if (g_drm >= 0) close(g_drm);
    g_gbm = nullptr, g_drm = -1, g_useBuffers = false;
}

bool MakeBuffers() {
    g_drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_drm >= 0) g_gbm = gbm_create_device(g_drm);
    for (Buffer &b : g_buffers) {
        // ABGR8888 is R, G, B, A in memory, like g_px.
        if (g_gbm) b.bo = gbm_bo_create(g_gbm, kW, kH, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!b.bo || (b.fd = gbm_bo_get_fd(b.bo)) < 0) break;
        vr::DmabufAttributes_t a{};
        a.unWidth = kW, a.unHeight = kH;
        a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
        a.unFormat = DRM_FORMAT_ABGR8888;
        a.ulModifier = DRM_FORMAT_MOD_LINEAR;
        a.unPlaneCount = 1;
        a.plane[0].unOffset = gbm_bo_get_offset(b.bo, 0);
        a.plane[0].unStride = gbm_bo_get_stride(b.bo);
        a.plane[0].nFd = b.fd;
        if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &b.handle)) b.handle = 0;
        if (!b.handle) break;
    }
    g_useBuffers = g_buffers[2].handle != 0;
    if (!g_useBuffers) {
        std::printf("keyboard: no shared buffers; falling back to SetOverlayRaw (the panel flickers)\n");
        DropBuffers();
    }
    return g_useBuffers;
}

// g_px to the panel: into the next buffer (premultiplied, as the flag says), then shown.
void Present() {
    if (!g_useBuffers) {
        vr::VROverlay()->SetOverlayRaw(g_overlay, g_px.data(), kW, kH, 4);
        return;
    }
    Buffer &b = g_buffers[g_next];
    g_next = (g_next + 1) % 3;
    uint32_t stride = 0;
    void *mapping = nullptr;
    auto *dst = static_cast<uint8_t *>(gbm_bo_map(b.bo, 0, 0, kW, kH, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
    if (!dst) return;
    for (int y = 0; y < kH; ++y) {
        const uint8_t *src = &g_px[size_t(y) * kW * 4];
        uint8_t *row = dst + size_t(y) * stride;
        for (int x = 0; x < kW * 4; x += 4) {
            const unsigned a = src[x + 3];
            row[x] = uint8_t(src[x] * a / 255), row[x + 1] = uint8_t(src[x + 1] * a / 255);
            row[x + 2] = uint8_t(src[x + 2] * a / 255), row[x + 3] = uint8_t(a);
        }
    }
    gbm_bo_unmap(b.bo, mapping);
    vr::Texture_t tex = {&b.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(g_overlay, &tex);
}

void Draw() {
    g_px.assign(size_t(kW) * kH * 4, 0);
    const uint8_t back[4] = {20, 22, 26, 225};
    FillRounded(0, 0, kW, kH, 22, back);
    const bool grip = g_hoverGrip || g_dragDevice != vr::k_unTrackedDeviceIndexInvalid;
    const uint8_t bar[4] = {220, 222, 228, uint8_t(grip ? 235 : 120)};
    FillRounded(kW / 2 - 130, kGrip / 2 - 5, kW / 2 + 130, kGrip / 2 + 7, 6, bar);
    const bool shift = g_latched[Shift];
    const int row = (kH - kGrip - kMargin) / kRows;
    for (size_t i = 0; i < g_keys.size(); ++i) {
        const Key &k = g_keys[i];
        const int mod = ModOf(k.def.code);
        const bool latched = mod >= 0 && g_latched[mod];
        uint8_t face[4] = {58, 62, 70, 255};
        if (latched) face[0] = 26, face[1] = 115, face[2] = 232;  // the blue the other controls use when armed
        if (int(i) == g_down) face[0] = 26, face[1] = 115, face[2] = 232;
        else if (int(i) == g_hover) for (int c = 0; c < 3; ++c) face[c] = uint8_t(std::min(255, face[c] + 38));
        FillRounded(k.x0, k.y0, k.x1, k.y1, 10, face);
        const int cx = (k.x0 + k.x1) / 2, cy = (k.y0 + k.y1) / 2;
        switch (k.def.code) {
            case KEY_UP: DrawArrow(cx, cy, row / 3, 0, -1, 235); continue;
            case KEY_DOWN: DrawArrow(cx, cy, row / 3, 0, 1, 235); continue;
            case KEY_LEFT: DrawArrow(cx, cy, row / 3, -1, 0, 235); continue;
            case KEY_RIGHT: DrawArrow(cx, cy, row / 3, 1, 0, 235); continue;
        }
        const char *label = shift && k.def.shifted ? k.def.shifted : k.def.label;
        const bool word = Codepoints(label).size() > 1;
        DrawText(label, int(row * (word ? 0.3 : 0.46)), cx, cy, 235);
    }
    Present();
    g_dirty = false;
}

int KeyAt(double x, double y) {
    for (size_t i = 0; i < g_keys.size(); ++i) {
        const Key &k = g_keys[i];
        if (x >= k.x0 - kGap / 2.0 && x < k.x1 + kGap / 2.0 && y >= k.y0 - kGap / 2.0 && y < k.y1 + kGap / 2.0)
            return int(i);
    }
    return -1;
}

// ---------------------------------------------------------------- carrying it

vr::HmdMatrix34_t Mul(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b) {
    vr::HmdMatrix34_t r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    return r;
}

vr::HmdMatrix34_t Inverse(const vr::HmdMatrix34_t &a) {  // rigid: R^T, -R^T t
    vr::HmdMatrix34_t r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

bool DevicePose(vr::TrackedDeviceIndex_t dev, vr::HmdMatrix34_t *out) {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    if (dev >= vr::k_unMaxTrackedDeviceCount || !poses[dev].bPoseIsValid) return false;
    *out = poses[dev].mDeviceToAbsoluteTracking;
    return true;
}

void Place(const vr::HmdMatrix34_t &pose) {
    g_pose = pose;
    vr::VROverlay()->SetOverlayTransformAbsolute(g_overlay, vr::TrackingUniverseStanding, &g_pose);
}

void StopCarrying() {
    if (g_dragDevice == vr::k_unTrackedDeviceIndexInvalid) return;
    g_dragDevice = vr::k_unTrackedDeviceIndexInvalid;
    g_dirty = true;
}

bool Create() {
    if (g_overlay != vr::k_ulOverlayHandleInvalid) return true;
    if (vr::VROverlay()->CreateOverlay("frametop.keyboard", "Frametop keyboard", &g_overlay) != vr::VROverlayError_None)
        return false;
    LoadFont();
    Layout();
    vr::VROverlay()->SetOverlayWidthInMeters(g_overlay, float(kWidthMetres));
    vr::VROverlay()->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{{float(kW), float(kH)}};
    vr::VROverlay()->SetOverlayMouseScale(g_overlay, &scale);
    vr::VROverlay()->SetOverlaySortOrder(g_overlay, 15);  // over the screens and their controls
    if (MakeBuffers()) vr::VROverlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_IsPremultiplied, true);
    return true;
}

}  // namespace

bool Show(const vr::HmdMatrix34_t &pose) {
    if (!Create()) return false;
    Place(pose);
    if (!g_shown) {
        std::fill(std::begin(g_latched), std::end(g_latched), false);
        g_hover = g_down = -1;
        g_hoverGrip = false;
        g_dragDevice = vr::k_unTrackedDeviceIndexInvalid;
        Draw();
        vr::VROverlay()->ShowOverlay(g_overlay);
        g_shown = true;
    }
    return true;
}

const vr::HmdMatrix34_t &Pose() { return g_pose; }

void EndDragBy(uint32_t device) {
    if (device == g_dragDevice) StopCarrying();
}

void Hide() {
    StopCarrying();
    if (!g_shown) return;
    vr::VROverlay()->HideOverlay(g_overlay);
    g_shown = false;
}

bool Shown() { return g_shown; }

void SetLasers(bool on) {
    static vr::VROverlayHandle_t set = vr::k_ulOverlayHandleInvalid;  // the overlay `was` is for
    static bool was = false;
    if (!Create() || (set == g_overlay && was == on)) return;
    set = g_overlay, was = on;
    vr::VROverlay()->SetOverlayFlag(g_overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, on);
}

bool Aimed(const vr::HmdMatrix34_t &laser) {
    if (!g_shown) return false;
    vr::VROverlayIntersectionParams_t in{};
    in.eOrigin = vr::TrackingUniverseStanding;
    in.vSource = {laser.m[0][3], laser.m[1][3], laser.m[2][3]};
    in.vDirection = {-laser.m[0][2], -laser.m[1][2], -laser.m[2][2]};
    vr::VROverlayIntersectionResults_t out{};
    return vr::VROverlay()->ComputeOverlayIntersection(g_overlay, &in, &out);
}

void Poll(void (*handle)(const Event &, void *), void *data) {
    if (g_overlay == vr::k_ulOverlayHandleInvalid) return;
    auto send = [&](uint32_t code, bool pressed) { handle(Event{Event::Key, code, pressed}, data); };
    // Let go of the key held down: it, then the latched modifiers it was typed with.
    auto release = [&]() {
        if (g_down < 0) return;
        const uint32_t code = g_keys[g_down].def.code;
        g_down = -1;
        g_dirty = true;
        if (code == kHide || ModOf(code) >= 0) return;
        send(code, false);
        for (int m = kMods - 1; m >= 0; --m)
            if (g_latched[m]) send(kModCode[m], false), g_latched[m] = false;
    };
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(g_overlay, &ev, sizeof ev)) {
        const double x = ev.data.mouse.x, y = kH - ev.data.mouse.y;  // OpenVR's mouse origin is bottom left
        switch (ev.eventType) {
            case vr::VREvent_MouseMove: {
                const int k = KeyAt(x, y);
                if (k != g_hover) g_hover = k, g_dirty = true;
                if ((y < kGrip) != g_hoverGrip) g_hoverGrip = y < kGrip, g_dirty = true;
                break;
            }
            case vr::VREvent_MouseButtonDown: {
                if (ev.data.mouse.button != vr::VRMouseButton_Left) break;
                release();
                vr::HmdMatrix34_t dev;
                if (y < kGrip && DevicePose(ev.trackedDeviceIndex, &dev)) {
                    g_dragDevice = ev.trackedDeviceIndex;
                    g_dragRel = Mul(Inverse(dev), g_pose);
                    g_dirty = true;
                    break;
                }
                const int k = KeyAt(x, y);
                if (k < 0) break;
                g_down = k;
                g_dirty = true;
                const uint32_t code = g_keys[k].def.code;
                const int mod = ModOf(code);
                if (code == kHide) {
                    Hide();
                    handle(Event{Event::Closed, 0, false}, data);
                } else if (mod >= 0) {
                    g_latched[mod] = !g_latched[mod];
                } else {
                    for (int m = 0; m < kMods; ++m)
                        if (g_latched[m]) send(kModCode[m], true);
                    send(code, true);
                }
                break;
            }
            case vr::VREvent_MouseButtonUp:
                if (ev.data.mouse.button == vr::VRMouseButton_Left) release(), EndDragBy(ev.trackedDeviceIndex);
                handle(Event{Event::RawMouseButtonUp, 0, false, ev.trackedDeviceIndex, ev.data.mouse.button}, data);
                break;
            case vr::VREvent_FocusLeave:
                release();
                if (g_hover >= 0) g_hover = -1, g_dirty = true;
                if (g_hoverGrip) g_hoverGrip = false, g_dirty = true;
                break;
            default:
                break;
        }
    }
    vr::HmdMatrix34_t dev;
    if (g_dragDevice != vr::k_unTrackedDeviceIndexInvalid) {
        if (DevicePose(g_dragDevice, &dev)) Place(Mul(dev, g_dragRel));
        else StopCarrying();  // the device went away
    }
    if (g_dirty && g_shown) Draw();
}

void Destroy() {
    if (g_overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(g_overlay);
    DropBuffers();
    g_overlay = vr::k_ulOverlayHandleInvalid;
    g_shown = false;
}

}  // namespace keyboard
