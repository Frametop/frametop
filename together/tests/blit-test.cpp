// A shared panel's GPU pass (together/media/blit.h) on this Frame's GPU: one picture of known
// colours drawn into the encoder's pictures, RGBA and NV12, and read back. Skipped where there is
// no Frame's encoder (/dev/video-enc0): another GPU lays its buffers out otherwise.
#include "../media/blit.h"
#include "../media/irisenc.h"
#include "../media/testpattern.h"

#include <drm_fourcc.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

static int failed = 0;
static bool check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

namespace {

constexpr int kW = 640, kH = 360;
struct Colour { int r, g, b; };
// Four stripes across.
constexpr Colour kStripe[4] = {{255, 255, 255}, {255, 0, 0}, {0, 0, 255}, {240, 192, 64}};

bool Near(int a, int b) { return std::abs(a - b) <= 2; }

// The picture drawn into one of `layout`, as bytes; empty when it can't be.
std::vector<uint8_t> Drawn(const media::PictureLayout &layout, std::string &err) {
    media::Pictures pictures, source;
    media::PictureLayout rgba;
    rgba.width = kW, rgba.height = kH, rgba.stride = kW * 4, rgba.size = size_t(kW) * kH * 4;
    if (!pictures.Open(layout, err) || !source.Open(rgba, err)) return {};
    uint32_t stride = 0;
    uint8_t *px = source.Map(0, stride);
    if (!px) return err = "no map", std::vector<uint8_t>();
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const Colour &c = kStripe[x * 4 / kW];
            uint8_t *p = px + size_t(y) * stride + size_t(x) * 4;
            // the top half; below it, the same at half the light
            p[0] = uint8_t(y < kH / 2 ? c.r : c.r / 2), p[1] = uint8_t(y < kH / 2 ? c.g : c.g / 2), p[2] = uint8_t(y < kH / 2 ? c.b : c.b / 2), p[3] = 0;
        }
    source.Unmap(0);
    media::Blit blit;
    media::Dmabuf d;
    d.width = kW, d.height = kH, d.format = DRM_FORMAT_ABGR8888, d.modifier = DRM_FORMAT_MOD_LINEAR;
    d.planes = 1, d.fd[0] = source.fd(0), d.stride[0] = rgba.stride;
    if (!blit.Open(pictures, err) || !blit.Import(7, d, err)) return {};
    if (!blit.Draw(7, 2)) return err = "not drawn", std::vector<uint8_t>();
    px = pictures.Map(2, stride);
    if (!px || stride != layout.stride) return err = "no map back", std::vector<uint8_t>();
    std::vector<uint8_t> bytes(px, px + layout.size);
    pictures.Unmap(2);
    return bytes;
}

// Where stripe s's middle is, in the top half (0) or the bottom (1).
int X(int s) { return kW / 8 + s * kW / 4; }
int Y(int half) { return kH / 4 + half * kH / 2; }

}  // namespace

int main() {
    if (access("/dev/video-enc0", F_OK) || access("/dev/dri/renderD128", R_OK | W_OK)) return std::printf("skipped: no Frame encoder here\n"), 0;
    std::string err;

    media::PictureLayout rgba;
    rgba.width = kW, rgba.height = kH, rgba.stride = kW * 4, rgba.size = size_t(kW) * 4 * 384;
    std::vector<uint8_t> px = Drawn(rgba, err);
    if (!check("an RGBA picture is drawn", !px.empty())) std::printf("    (%s)\n", err.c_str());
    bool right = !px.empty();
    for (int s = 0; right && s < 4; ++s)
        for (int half = 0; half < 2; ++half) {
            const uint8_t *p = px.data() + size_t(Y(half)) * rgba.stride + size_t(X(s)) * 4;
            const int by = half + 1;
            right = right && Near(p[0], kStripe[s].r / by) && Near(p[1], kStripe[s].g / by) && Near(p[2], kStripe[s].b / by) && p[3] == 255;
        }
    check("...in its colours, the right way up, opaque", right);

    media::PictureLayout nv12;
    nv12.width = kW, nv12.height = kH, nv12.nv12 = true, nv12.stride = kW;
    nv12.chroma = irisenc::Nv12Chroma(kW, kH), nv12.size = irisenc::Nv12Size(kW, kH);
    px = Drawn(nv12, err = "");
    if (!check("an NV12 picture is drawn", !px.empty())) std::printf("    (%s)\n", err.c_str());
    bool luma = !px.empty(), chroma = luma;
    for (int s = 0; !px.empty() && s < 4; ++s)
        for (int half = 0; half < 2; ++half) {
            const int by = half + 1;
            const double r = kStripe[s].r / by, g = kStripe[s].g / by, b = kStripe[s].b / by, y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            const int gotY = px[size_t(Y(half)) * kW + size_t(X(s))];
            const uint8_t *uv = px.data() + nv12.chroma + size_t(Y(half) / 2) * kW + size_t(X(s) / 2) * 2;
            const bool l = Near(gotY, int(y + 0.5)), c = Near(uv[0], std::min(255, int((b - y) / 1.8556 + 128.5))) && Near(uv[1], std::min(255, int((r - y) / 1.5748 + 128.5)));
            if (!l || !c) std::printf("    (stripe %d half %d: Y %d of %.0f, Cb %d of %.0f, Cr %d of %.0f)\n", s, half, gotY, y, uv[0], (b - y) / 1.8556 + 128, uv[1], (r - y) / 1.5748 + 128);
            luma = luma && l, chroma = chroma && c;
        }
    check("...its Y in BT.709, full range", luma);
    check("...its Cb,Cr where the encoder looks for them", chroma);

    // the test picture says the same in NV12 as in RGBA: here in memory, with no GPU
    std::vector<uint8_t> was(nv12.size, 9);
    namespace tp = media::testpattern;
    tp::DrawNv12(was.data(), was.data() + nv12.chroma, kW, kW, kH, 5, -1);
    tp::DrawNv12(was.data(), was.data() + nv12.chroma, kW, kW, kH, 6, 5);
    std::vector<uint8_t> fresh(nv12.size, 9);
    tp::DrawNv12(fresh.data(), fresh.data() + nv12.chroma, kW, kW, kH, 6, -1);
    check("the test picture drawn over the last one is the one drawn whole", was == fresh);
    check("...its grey and its squares in Y", fresh[size_t(kH - 1) * kW + 300] == 48 && fresh[3] == 240 && fresh[size_t(tp::Square(kW)) + 3] == 16);

    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
