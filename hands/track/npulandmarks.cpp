#include "npulandmarks.h"

#include <algorithm>
#include <chrono>

bool NpuLandmarks::run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
                       std::vector<Landmarks> &out) {
    if (images.size() != rois.size() || images.empty() || !s_.ensure()) return false;
    std::vector<Landmarks> res(images.size());
    NpuClient &cl = s_.client();
    float tm[NpuClient::kMaxCrops][6];
    float screen[42], world[63];
    for (size_t at = 0; at < images.size(); at += NpuClient::kMaxCrops) {
        const int n = int(std::min<size_t>(NpuClient::kMaxCrops, images.size() - at));
        const auto t0 = std::chrono::steady_clock::now();
        const int side = s_.heads().crop;
        for (int i = 0; i < n; ++i) nets_.hand_crop(*images[at + i], rois[at + i], cl.crop(i), tm[i], side);
        const auto t1 = std::chrono::steady_clock::now();
        std::string e;
        if (!cl.run(n, NpuSession::kTimeoutMs, e)) return s_.fail(e), false;
        times.add(std::chrono::duration<double, std::milli>(t1 - t0).count(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(), cl);
        for (int i = 0; i < n; ++i) {
            Landmarks &lm = res[at + i];
            float p = 0, r = 0;
            s_.heads().decode(cl.output(i), screen, world, p, r);
            for (int k = 0; k < 21; ++k) {
                lm.pts[k] = Nets::crop_to_image(tm[i], screen[2 * k], screen[2 * k + 1]);
                for (int c = 0; c < 3; ++c) lm.world[k][c] = world[3 * k + c];
            }
            lm.presence = p, lm.right = r;
        }
    }
    out = std::move(res);
    return true;
}
