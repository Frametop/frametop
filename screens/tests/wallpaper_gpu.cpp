// The stereo wallpaper on the GPU (DEE-317), offline: handcut's Renderer (every shader compiles,
// kKeyed included) draws a test client buffer with the key colour, the key in a black shadow's
// darkening, and grey, and a picture per eye; the output's pixels are read back and checked. No
// SteamVR, no overlay. Needs a render node; exits 77 (skipped) without one.
//
// From screens/, in the dev container (the matte bench's build, scripts/bench-matte.sh):
//   . ../scripts/openvr.sh; g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include
//     $(pkg-config --cflags egl glesv2 gbm libdrm) tests/wallpaper_gpu.cpp handcut.cpp handrender.cpp
//     rendernode.cpp -o build/wallpaper-gpu $(pkg-config --libs egl glesv2 gbm) $OPENVR_LIBS
//   build/wallpaper-gpu
#include "../handcut.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

int main() {
    const int W = 64, H = 32;
    const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm < 0) return std::printf("skipped: no render node\n"), 77;
    handcut::Renderer r;
    check("the renderer starts (every program compiles)", r.Init({}, nullptr));
    if (failed) return 1;
    gbm_device *gbm = gbm_create_device(drm);
    gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) return std::printf("no test buffer\n"), 1;
    uint32_t stride = 0;
    void *map = nullptr;
    auto *px = static_cast<uint8_t *>(gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &stride, &map));
    // columns: 0..15 the key, 16..31 the key at 50% (a black shadow over it), 32..47 grey, 48..63 the key
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t *p = px + size_t(y) * stride + size_t(x) * 4;
            const int band = x / 16;
            const uint8_t v = band == 1 ? 128 : 255;
            if (band == 2) p[0] = p[1] = p[2] = 200;
            else p[0] = v, p[1] = 0, p[2] = v;
            p[3] = 255;
        }
    gbm_bo_unmap(bo, map);
    ft_dmabuf client{};
    client.width = W, client.height = H, client.format = DRM_FORMAT_ABGR8888, client.modifier = DRM_FORMAT_MOD_LINEAR;
    client.n_planes = 1, client.fd[0] = gbm_bo_get_fd(bo), client.stride[0] = gbm_bo_get_stride(bo);

    // the pictures: 4x2, left eye all red, right eye all green
    std::vector<uint8_t> left(4 * 2 * 3), right(4 * 2 * 3);
    for (size_t i = 0; i < left.size(); i += 3) left[i] = 255, right[i + 1] = 255;
    handcut::Spots none[2];
    auto draw = [&](uint64_t serial) {
        const handcut::Output *o = nullptr;
        for (int i = 0; i < 20 && !(o && o->serial == serial && o->wall); ++i) {
            o = r.Composite(0, &client, serial, client, none);
            r.Finish();
        }
        return o;
    };
    auto pixel = [](const handcut::Output *o, int x, int y, uint8_t rgb[3]) {
        uint32_t st = 0;
        void *m = nullptr;
        auto *p = static_cast<uint8_t *>(gbm_bo_map(static_cast<gbm_bo *>(o->bo), 0, 0, o->buf.width, o->buf.height,
                                                    GBM_BO_TRANSFER_READ, &st, &m));
        if (!p) return false;
        std::memcpy(rgb, p + size_t(y) * st + size_t(x) * 4, 3);
        gbm_bo_unmap(static_cast<gbm_bo *>(o->bo), m);
        return true;
    };
    auto is = [](const uint8_t c[3], int r, int g, int b) {
        return std::abs(c[0] - r) <= 3 && std::abs(c[1] - g) <= 3 && std::abs(c[2] - b) <= 3;
    };

    r.SetWallpaper(4, 2, left, right);
    const handcut::Output *o = draw(1);
    check("a buffer with the wallpaper comes back", o && o->buf.width == 2 * W && o->buf.height == H);
    if (!o) return 1;
    uint8_t c[3];
    check("left eye, the key: the left picture (red)", pixel(o, 8, 16, c) && is(c, 255, 0, 0));
    std::printf("      got %d %d %d\n", c[0], c[1], c[2]);
    check("right eye, the key: the right picture (green)", pixel(o, W + 8, 16, c) && is(c, 0, 255, 0));
    check("the key in a shadow: the picture in that shadow", pixel(o, 24, 16, c) && is(c, 128, 0, 0));
    std::printf("      got %d %d %d\n", c[0], c[1], c[2]);
    check("grey stays grey, both eyes", pixel(o, 40, 16, c) && is(c, 200, 200, 200) && pixel(o, W + 40, 16, c) &&
                                            is(c, 200, 200, 200));

    // a new picture with the same client frame: drawn again, not skipped as "the same"
    r.SetWallpaper(4, 2, right, left);
    const uint64_t was = o->wall;
    const handcut::Output *o2 = nullptr;
    for (int i = 0; i < 20 && !(o2 && o2->wall != was); ++i) o2 = r.Composite(0, &client, 1, client, none), r.Finish();
    check("a new picture redraws the same frame", o2 && o2->wall != was && pixel(o2, 8, 16, c) && is(c, 0, 255, 0));

    // none: the client's pixels as they are
    r.SetWallpaper(0, 0, {}, {});
    const handcut::Output *o3 = nullptr;
    for (int i = 0; i < 20 && !(o3 && o3->wall != o2->wall); ++i) o3 = r.Composite(0, &client, 1, client, none), r.Finish();
    check("off: the key colour as it is", o3 && pixel(o3, 8, 16, c) && is(c, 255, 0, 255));

    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
