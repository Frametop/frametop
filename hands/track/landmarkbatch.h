// A landmark model that takes several crops in one call (the NPU student through ft-npud: one DSP request a
// frame). Tracker::run_landmarks hands its views to one when set, and runs Nets::landmarks on its pool for any
// call it refuses, so a missing or failed backend only costs CPU time.
#pragma once

#include "nets.h"

#include <vector>

class LandmarkBatch {
public:
    virtual ~LandmarkBatch() = default;
    // One Landmarks per (image, roi), in order. False: nothing was written to out; the caller falls back.
    virtual bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
                     std::vector<Landmarks> &out) = 0;
};
