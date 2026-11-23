// The NPU student as a LandmarkBatch: Nets' own crops go to ft-npud (up to 4 a request), and NpuHeads decodes the
// outputs on the CPU into the same Landmarks the stock and fine-tuned ncnn models give. When the daemon is
// missing, serves another graph or fails a request, run() returns false (the tracker uses the CPU model) and the
// session tries the daemon again later (NpuSession). A two-view student reads one hand's first two views as a
// pair (npu_requests).
#pragma once

#include "landmarkbatch.h"
#include "npurequests.h"
#include "npusession.h"

class NpuLandmarks : public LandmarkBatch {
public:
    NpuLandmarks(const Nets &nets, NpuSession &session) : nets_(nets), s_(session) {}
    bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois, const std::vector<int> &hands,
             std::vector<Landmarks> &out) override;
    NpuTimes times;

private:
    const Nets &nets_;
    NpuSession &s_;
};
