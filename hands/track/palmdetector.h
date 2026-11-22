// A palm detector over whole camera images (the NPU detector through ft-npud). When the tracker has one, a search
// round gives it every camera it covers and takes its palms there instead of running the CPU palm search's
// tiles; when run() returns false, the tiles as before.
#pragma once

#include "nets.h"

#include <string>
#include <vector>

class PalmDetector {
public:
    virtual ~PalmDetector() = default;
    virtual bool covers(const std::string &camera) const = 0;
    // Palms (image pixels) per image, in order. False: nothing was written to out; the caller falls back.
    virtual bool run(const std::vector<const Image *> &images, std::vector<std::vector<Palm>> &out) = 0;
};
