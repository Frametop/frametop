// A landmark model that takes several crops in one call (the NPU student through ft-npud: one DSP request a
// frame). Tracker::run_landmarks hands its views to one when set, and runs Nets::landmarks on its pool for any
// call it refuses, so a missing or failed backend only costs CPU time.
#pragma once

#include "nets.h"

#include <vector>

class LandmarkBatch {
public:
    virtual ~LandmarkBatch() = default;
    // One Landmarks per (image, roi), in order. hands: the tracked hand each view is of (0: not known yet), so
    // a two-view model can read one hand's views together. False: nothing was written to out; the caller falls
    // back.
    virtual bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
                     const std::vector<int> &hands, std::vector<Landmarks> &out) = 0;
};
