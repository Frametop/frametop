// A PalmDetector's search rounds, pipelined one camera a step: a round queues the cameras the detector covers,
// end_step() posts the next one's image after the step's other NPU requests, and begin_step() collects its palms
// on the next step before them. The DSP time then falls between steps instead of inside one, and each request is
// one camera's tiles (one ft-npud request). The palms are a step old when the tracker reads them: its landmark
// crops from a palm box are generous enough for that.
#pragma once

#include "palmdetector.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class DetectorRounds {
public:
    explicit DetectorRounds(PalmDetector &d) : d_(d) {}
    // Before the step's other NPU requests: the palms of the camera posted last step, for ready().
    void begin_step();
    // The palms collected this step, per camera (none on most steps).
    const std::vector<std::pair<std::string, std::vector<Palm>>> &ready() const { return ready_; }
    // Queues a round over these cameras unless one is still running.
    void start(const std::vector<std::string> &cameras);
    bool running() const { return !queue_.empty() || d_.pending(); }
    // After the step's other NPU requests: posts the next queued camera that has an image in this set.
    void end_step(const std::map<std::string, Image> &images);
    // False after a post or a collect failed, until one succeeds again: the tracker then searches the
    // detector's cameras with its CPU tiles.
    bool healthy() const { return healthy_; }
    bool covers(const std::string &camera) const { return d_.covers(camera); }
    int collected = 0, failed = 0;
    double blocked_ms = 0;   // time the steps spent posting and collecting

private:
    PalmDetector &d_;
    std::vector<std::string> queue_;
    std::vector<std::pair<std::string, std::vector<Palm>>> ready_;
    bool healthy_ = true;
};
