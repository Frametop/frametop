#include "npulandmarks.h"

#include <algorithm>
#include <cstdio>

bool NpuLandmarks::ensure() {
    if (client_.connected()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) return false;
    retry_at_ = now + std::chrono::milliseconds(int(kRetrySeconds * 1000));
    std::string e;
    if (!heads_.load(dir_ + "/ft-npud.heads", e) || !client_.open(dir_, heads_.out_bytes(), heads_.graph_id, e)) {
        if (e != err_) std::fprintf(stderr, "npu: %s; landmarks on the CPU\n", e.c_str());
        err_ = e;
        return false;
    }
    std::fprintf(stderr, "npu: ft-npud connected (graph %08x)\n", heads_.graph_id);
    err_.clear();
    return true;
}

void NpuLandmarks::fail(const std::string &why) {
    ++failures;
    err_ = why;
    std::fprintf(stderr, "npu: %s; landmarks on the CPU until ft-npud answers again\n", why.c_str());
}

bool NpuLandmarks::run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
                       std::vector<Landmarks> &out) {
    if (images.size() != rois.size() || images.empty() || !ensure()) return false;
    std::vector<Landmarks> res(images.size());
    float tm[NpuClient::kMaxCrops][6];
    float screen[42], world[63];
    for (size_t at = 0; at < images.size(); at += NpuClient::kMaxCrops) {
        const int n = int(std::min<size_t>(NpuClient::kMaxCrops, images.size() - at));
        for (int i = 0; i < n; ++i) nets_.hand_crop(*images[at + i], rois[at + i], client_.crop(i), tm[i]);
        std::string e;
        if (!client_.run(n, kTimeoutMs, e)) return fail(e), false;
        ++requests;
        dsp_ms += client_.wrapper_ms();
        for (int i = 0; i < n; ++i) {
            Landmarks &lm = res[at + i];
            float p = 0, r = 0;
            heads_.decode(client_.output(i), screen, world, p, r);
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
