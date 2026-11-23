#include "npulandmarks.h"

#include <algorithm>
#include <chrono>

bool NpuLandmarks::run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
                       const std::vector<int> &hands, std::vector<Landmarks> &out) {
    if (images.size() != rois.size() || images.size() != hands.size() || images.empty() || !s_.ensure()) return false;
    std::vector<Landmarks> res(images.size());
    NpuClient &cl = s_.client();
    const NpuHeads &heads = s_.heads();
    float tm[NpuClient::kMaxCrops][6];
    float screen[42], world[63];
    for (const NpuRequest &r : npu_requests(hands, heads.two_view)) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < r.n; ++i)
            nets_.hand_crop(*images[r.views[i]], rois[r.views[i]], cl.crop(i), tm[i], heads.crop);
        const auto t1 = std::chrono::steady_clock::now();
        std::string e;
        if (!cl.run(r.n, NpuSession::kTimeoutMs, e, r.pairs)) return s_.fail(e), false;
        times.add(std::chrono::duration<double, std::milli>(t1 - t0).count(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(), cl);
        for (int i = 0; i < r.n; ++i) {
            Landmarks &lm = res[r.views[i]];
            const bool paired = r.pairs & (i < 2 ? NpuClient::kPair0 : NpuClient::kPair1);
            float p = 0, rt = 0;
            heads.decode(cl.output(i), screen, world, p, rt, paired ? cl.output(i ^ 1) : nullptr);
            for (int k = 0; k < 21; ++k) {
                lm.pts[k] = Nets::crop_to_image(tm[i], screen[2 * k], screen[2 * k + 1]);
                for (int c = 0; c < 3; ++c) lm.world[k][c] = world[3 * k + c];
            }
            lm.presence = p, lm.right = rt;
        }
    }
    out = std::move(res);
    return true;
}
