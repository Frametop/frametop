// A palm detector over whole camera images (the NPU detector through ft-npud), one camera's image a request,
// posted on one step and collected on the next (DetectorRounds schedules it). When the tracker has one, its
// cameras get its palms instead of the CPU palm search's tiles; after a failed post or collect, the tiles again.
#pragma once

#include "nets.h"

#include <string>
#include <vector>

class PalmDetector {
public:
    virtual ~PalmDetector() = default;
    virtual bool covers(const std::string &camera) const = 0;
    // Starts a request on this camera's image (copied: the image may go away). False: nothing was started.
    virtual bool post(const Image &image, const std::string &camera) = 0;
    bool pending() const { return pending_; }
    // The posted request's camera and palms (image pixels), waiting for it if need be. False: it failed.
    virtual bool collect(std::string &camera, std::vector<Palm> &out) = 0;

protected:
    bool pending_ = false;
};
