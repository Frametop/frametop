// The NPU student as a LandmarkBatch: Nets' own crops go to ft-npud (up to 4 a request), and NpuHeads decodes the
// outputs on the CPU into the same Landmarks the stock and fine-tuned ncnn models give. When the daemon is
// missing, serves another graph or fails a request, run() returns false (the tracker uses the CPU model) and the
// session tries the daemon again later (NpuSession).
#pragma once

#include "landmarkbatch.h"
#include "npusession.h"

class NpuLandmarks : public LandmarkBatch {
public:
    NpuLandmarks(const Nets &nets, NpuSession &session) : nets_(nets), s_(session) {}
    bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
             std::vector<Landmarks> &out) override;
    NpuTimes times;

private:
    const Nets &nets_;
    NpuSession &s_;
};
