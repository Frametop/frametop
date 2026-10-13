// The cutout composite's cost, offline: recorded hands (ft-handreplay --publish-dump) cut out of
// a 3440x1935 test buffer on a panel 0.6 m ahead, through handcut's Renderer on the GPU (no
// SteamVR, no overlay), the round, hand-shaped and ft-hands' capsule mattes.
//
//   matte_bench DUMP [--frames N] [--panel-px W H]
//
// Prints, per matte: CPU ms per Composite (as ft-screens' "cutouts state"), the GPU's time to
// finish each buffer (glFinish right after it: an upper bound, the GPU is otherwise idle here),
// and the pieces per eye. Build and run: scripts/bench-matte.sh DUMP.
#include "../handcut.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

handcut::Mat Identity() {
    handcut::Mat m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    return m;
}

bool Pattern(int drm, int w, int h, ft_dmabuf *b) {
    gbm_device *gbm = gbm_create_device(drm);
    gbm_bo *bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) return false;
    uint32_t stride = 0;
    void *data = nullptr;
    auto *px = static_cast<uint8_t *>(gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &data));
    if (!px) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = px + size_t(y) * stride + size_t(x) * 4;
            const uint8_t g = (x % 80 < 3 || y % 80 < 3) ? 90 : 210;
            p[0] = p[1] = p[2] = g, p[3] = 255;
        }
    gbm_bo_unmap(bo, data);
    *b = {};
    b->width = w, b->height = h, b->format = DRM_FORMAT_ABGR8888, b->modifier = DRM_FORMAT_MOD_LINEAR, b->n_planes = 1;
    b->fd[0] = gbm_bo_get_fd(bo), b->stride[0] = gbm_bo_get_stride(bo);
    return true;
}

double Pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * v.size()))];
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: matte_bench DUMP [--frames N] [--panel-px W H]\n"), 2;
    int frames = 1500, W = 3440, H = 1935;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--panel-px") && i + 2 < argc) W = std::atoi(argv[++i]), H = std::atoi(argv[++i]);
    }
    std::ifstream f(argv[1], std::ios::binary);
    char magic[8];
    uint32_t sizes[2];
    if (!f.read(magic, 8) || std::memcmp(magic, "FHDUMP01", 8) || !f.read(reinterpret_cast<char *>(sizes), 8))
        return std::fprintf(stderr, "%s: not a publish dump\n", argv[1]), 1;
    std::vector<std::vector<char>> hands;
    std::vector<fh_gestures_t> gestures;
    std::vector<char> gb(sizes[1]);
    while (int(hands.size()) < frames) {
        std::vector<char> h(sizes[0]);
        fh_gestures_t g{};
        if (!f.read(h.data(), sizes[0]) || !f.read(gb.data(), sizes[1])) break;
        std::memcpy(&g, gb.data(), std::min(sizeof g, size_t(sizes[1])));
        hands.push_back(std::move(h)), gestures.push_back(g);
    }
    handcut::Renderer r;
    if (!r.Init({}, nullptr)) return 1;
    const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    ft_dmabuf client;
    if (drm < 0 || !Pattern(drm, W, H, &client)) return std::fprintf(stderr, "no test buffer\n"), 1;
    const double width = 1.2;
    const handcut::Panel panel{[] {
                                   handcut::Mat m = Identity();
                                   m.m[1][3] = -0.15f, m.m[2][3] = -0.6f;
                                   return m;
                               }(),
                               width, width * H / W, 0, W, H};
    const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}}, head[3] = {0, 0, 0};
    std::printf("%zu frames of %s on a %dx%d panel %.1f m wide, 0.6 m ahead\n", hands.size(), argv[1], W, H, width);
    // The modes take turns frame by frame (each its own panel, so its own buffers), so the
    // GPU's clocks and whatever else runs weigh on them alike.
    struct Mode {
        const char *name;
        handcut::MatteShape shape;
        bool glows, still;
        handcut::Hands hands;
        handcut::Matte m;
        std::vector<double> cpu, gpu;
        double pieces = 0;
        int cut = 0, draws = 0, partial = 0;
    };
    std::vector<Mode> modes;
    for (bool still : {false, true}) {
        modes.push_back({"circle", handcut::MatteShape::Circle, false, still, {}, {}, {}, {}});
        modes.push_back({"mesh", handcut::MatteShape::Mesh, false, still, {}, {}, {}, {}});
        modes.push_back({"capsules", handcut::MatteShape::Capsules, false, still, {}, {}, {}, {}});
        modes.push_back({"circle + 2 glows", handcut::MatteShape::Circle, true, still, {}, {}, {}, {}});
    }
    uint64_t serial = 1;
    for (size_t k = 0; k < hands.size(); ++k) {
        const auto *v1 = reinterpret_cast<const fh_hands_t *>(hands[k].data());
        serial += k % 2;   // a new client frame every other hands frame (a video)
        for (size_t i = 0; i < modes.size(); ++i) {
            Mode &md = modes[i];
            handcut::MatteParams mp;
            mp.shape = md.shape;
            md.hands.Feed(hands[k].data(), hands[k].size(), &gestures[k], Identity(), int64_t(v1->publish_ns));
            handcut::BuildMatte(md.hands.shown(), md.hands.capsules(), head, mp, md.m);
            handcut::Spots s[2];
            if (!handcut::Project(panel, md.m, eyes, s)) continue;
            if (md.glows)
                for (auto &e : s) e.glows = {{W * 0.4f, H * 0.5f, 18, 0.6f, true, {1, 1, 1}}, {W * 0.6f, H * 0.6f, 30, 0.9f, false, {0.35f, 0.67f, 1}}};
            r.Finish();
            r.Composite(int(i), &client, md.still ? 1 : serial, client, s);
            md.cpu.push_back(r.lastMs());
            md.gpu.push_back(r.Finish());
            const handcut::CutStats st = r.TakeStats();
            md.draws += st.draws, md.partial += st.partial;
            md.pieces += s[0].caps.size() + s[0].hulls.size();
            ++md.cut;
        }
    }
    for (const Mode &md : modes)
        std::printf("%-16s %-12s %d composites (%d draws, %d partial): CPU median %.3f ms, 90%% %.3f; GPU finish median "
                    "%.2f ms, 90%% %.2f, max %.2f; %.1f pieces per eye\n",
                    md.name, md.still ? "still screen" : "video", md.cut, md.draws, md.partial, Pct(md.cpu, 0.5),
                    Pct(md.cpu, 0.9), Pct(md.gpu, 0.5), Pct(md.gpu, 0.9), Pct(md.gpu, 1), md.cut ? md.pieces / md.cut : 0.0);
    return 0;
}
