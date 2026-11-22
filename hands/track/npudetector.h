// The NPU palm detector (npu-hands train/student/det.py) as a PalmDetector: each side camera's image as
// grid x grid tiles (det_tiles) through ft-npud, decoded on the CPU (decode_det, merge_palms: det.py's decode and
// merge) into Palms whose roi() is ft-hands' usual landmark crop. When the daemon is missing, serves no detector
// or fails a request, run() returns false and the tracker uses the CPU palm search.
#pragma once

#include "npusession.h"
#include "palmdetector.h"

// One detection in crop or image pixels: the wrist and the middle finger's base, and its score (0..1).
struct DetPoint {
    double score = 0;
    V2 wrist{}, middle{};
};

// det.py tile_rois: grid x grid squares of frac * max(w, h), the outer ones on the image's edges.
std::vector<Roi> det_tiles(int width, int height, int grid, double frac);
// det.py decode for one crop's outputs (obj [28][28], reg [4][28][28], s8; value = q * scale): local maxima over
// thr, the two points from reg, crop pixels.
std::vector<DetPoint> decode_det(const int8_t *obj, const int8_t *reg, float s_obj, float s_reg, float thr);
// det.py BOX: the palm box from the two points; kp[0] the wrist, kp[2] the middle base (what Palm::roi reads).
Palm palm_from(const DetPoint &d);
// det.py merge: strongest first; a palm whose centre lies within half a kept one's size is dropped.
std::vector<Palm> merge_palms(std::vector<Palm> palms);

class NpuDetector : public PalmDetector {
public:
    static constexpr int kGrid = 2;
    static constexpr double kFrac = 0.625, kThreshold = 0.3;
    NpuDetector(const Nets &nets, NpuSession &session, int grid = kGrid, double frac = kFrac, float thr = kThreshold)
        : nets_(nets), s_(session), grid_(grid), frac_(frac), thr_(thr) {}
    bool covers(const std::string &camera) const override { return camera == "slam_left" || camera == "slam_right"; }
    bool run(const std::vector<const Image *> &images, std::vector<std::vector<Palm>> &out) override;
    NpuTimes times;

private:
    const Nets &nets_;
    NpuSession &s_;
    int grid_;
    double frac_;
    float thr_;
};
