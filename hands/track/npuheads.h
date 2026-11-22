// The NPU student's CPU part: the float heads that turn ft-npud's integer outputs for one crop into landmarks.
// The weights come from the file ft-npud writes beside its socket (npu-hands npud/heads_file.py, which documents
// the layout); the maths is int8_ref.heads in the hands training code: a softmax over each keypoint's heatmap,
// the soft-argmax in crop pixels, the heatmap-weighted keypoint features, and the presence, side and world MLPs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct NpuHeads {
    uint32_t graph_id = 0;
    int K = 0, H = 0, W = 0, kpd = 0, g = 0, hidden = 0, pool_h = 0, pool_w = 0;
    int crop = 224;   // the crop side in pixels the graph reads (header word 11; 0 in older files: 224)
    float s_heat = 0, s_kp = 0, s_pool = 0;
    std::vector<float> grid, pres_w, right_w, w0, b0, w1, b1;
    float pres_b = 0, right_b = 0;
    // Derived at parse, for speed (the maths is unchanged): exp(-d * s_heat) for d = 0..255 (an int8 heatmap's
    // only distances from its maximum), and w0 / w1 transposed to [in][out] so the layers' inner loops vectorize.
    float exp_lut[256]{};
    std::vector<float> w0t, w1t;

    // Reads a heads file; false with err on a bad magic, version, shape or size.
    bool load(const std::string &path, std::string &err);
    bool parse(const uint8_t *data, size_t size, std::string &err);
    // Bytes of one crop's outputs as ft-npud lays them out: heat s8 [K][H][W], kp_feat s8 [kpd][H][W],
    // pool u8 [g][pool_h][pool_w].
    size_t out_bytes() const { return size_t(K + kpd) * H * W + size_t(g) * pool_h * pool_w; }
    // One crop: screen [K][2] in crop pixels, world [3K], presence and side as probabilities.
    void decode(const uint8_t *out, float *screen, float *world, float &presence, float &right) const;
};
