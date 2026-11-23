// The NPU student's CPU part: the float heads that turn ft-npud's integer outputs for one crop into landmarks.
// The weights come from the file ft-npud writes beside its socket (npu-hands npud/heads_file.py, which documents
// the layout); the maths is int8_ref.heads in the hands training code: a softmax over each keypoint's heatmap,
// the soft-argmax in crop pixels, the heatmap-weighted keypoint features, and the presence, side and world MLPs.
// A two-view student (heads file version 2) also reads the other view of the same hand: its pre-fusion pool and
// keypoint features add to the world, presence and side layers (no biases), when the crops were sent as a pair.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct NpuHeads {
    uint32_t graph_id = 0;
    int K = 0, H = 0, W = 0, kpd = 0, g = 0, hidden = 0, pool_h = 0, pool_w = 0;
    int crop = 224;   // the crop side in pixels the graph reads (header word 11; 0 in older files: 224)
    bool two_view = false;   // version 2: crops go in pairs (NpuClient::kPair0/kPair1), outputs add pool_pre
    float s_heat = 0, s_kp = 0, s_pool = 0, s_pool_pre = 0;
    std::vector<float> grid, pres_w, right_w, w0, b0, w1, b1;
    std::vector<float> wp, pres_pw, right_pw;   // two-view: world_p [hidden][g + K kpd], pres_p [g], right_p [g]
    float pres_b = 0, right_b = 0;
    // Derived at parse, for speed (the maths is unchanged): exp(-d * s_heat) for d = 0..255 (an int8 heatmap's
    // only distances from its maximum), and w0 / w1 transposed to [in][out] so the layers' inner loops vectorize.
    float exp_lut[256]{};
    std::vector<float> w0t, w1t, wpt;

    // Reads a heads file; false with err on a bad magic, version, shape or size.
    bool load(const std::string &path, std::string &err);
    bool parse(const uint8_t *data, size_t size, std::string &err);
    // Bytes of one crop's outputs as ft-npud lays them out: heat s8 [K][H][W], kp_feat s8 [kpd][H][W],
    // pool u8 [g][pool_h][pool_w], and for a two-view graph pool_pre u8 [g][pool_h][pool_w].
    size_t out_bytes() const { return size_t(K + kpd) * H * W + size_t(g) * pool_h * pool_w * (two_view ? 2 : 1); }
    // The heads file's size for a header (12 u32): 0 for a bad magic or version, or for shapes past parse's bounds.
    static size_t file_bytes(const uint32_t *hdr);
    // One crop: screen [K][2] in crop pixels, world [3K], presence and side as probabilities. partner: the other
    // crop of its pair when it was sent as one (two-view only), else null.
    void decode(const uint8_t *out, float *screen, float *world, float &presence, float &right,
                const uint8_t *partner = nullptr) const;

private:
    // a crop's head inputs: x = [pool mean (g) | keypoint features (K kpd)] from pool (or pool_pre with pre), and
    // screen when non-null
    void features(const uint8_t *out, bool pre, float *x, float *screen) const;
};
