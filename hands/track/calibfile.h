// The calibration file (include/fh_calib.h): the cameras ft-hands tracks with, each with the ring
// camera its images come from. No model runtime, so its test runs anywhere (tests/calibfile_test.cpp).
#pragma once

#include "calib.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "../include/fh_calib.h"
}

struct CalibCam {
    std::string name;   // the calibration's name (slam_left, color_video2, ...)
    int ring_cam = -1;  // index in the ring's cams[]
    int node = -1;      // that ring camera's node
    Camera cam;
};

// Fills everything but magic, version, size and seq: the first FH_CALIB_MAX_CAMS cameras.
void fill_calib(const std::vector<CalibCam> &cams, int64_t ring_writer_pid, uint64_t publish_ns, fh_calib_t &out);

// Writes DIR/calib under its sequence lock.
class CalibPublisher {
public:
    bool open(const std::string &dir, std::string &err);
    void use(fh_calib_t *mem);   // writes into mem instead (tests)
    void write(const std::vector<CalibCam> &cams, int64_t ring_writer_pid, uint64_t publish_ns);

private:
    fh_calib_t *out_ = nullptr;
    uint64_t seq_ = 0;
};
