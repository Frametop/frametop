// Frames in from ft-camd's ring (camd/fhring.h), hands out to the hands file
// (include/fh_hands.h, read by Frametop's ft-screens).
#pragma once

#include "tracker.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "../camd/fhring.h"
#include "../include/fh_hands.h"
}

class Ring {
public:
    bool open(const char *path, std::string &err);
    bool alive() const;                      // the writer's heartbeat is fresh
    int cameras() const { return int(hdr_->ncams); }
    const fh_ring_cam_t &camera(int i) const { return hdr_->cams[i]; }
    uint64_t latest(int i) const;
    // Copy frame n of camera i into out (width x height, tightly packed). False if it's
    // gone or was being written.
    bool read(int i, uint64_t n, std::vector<uint8_t> &out, fh_ring_slot_t *meta) const;
    // Just frame n's slot header (capture time etc.), without copying the image.
    bool meta(int i, uint64_t n, fh_ring_slot_t *meta) const;

private:
    const uint8_t *map_ = nullptr;
    const fh_ring_hdr_t *hdr_ = nullptr;
    size_t len_ = 0;
};

class Publisher {
public:
    bool open(std::string &err);
    void write(const std::vector<const Hand *> &hands, uint64_t capture_ns);
    // Fills one hand's version 2 record (also for ft-handreplay's checks).
    static void fill_fit(const Hand &h, uint64_t capture_ns, fh_hand_fit_t &o);

private:
    fh_hands_v2_t *file_ = nullptr;
    fh_hands_t *out_ = nullptr;
    uint64_t seq_ = 0;
};

uint64_t mono_ns();
int64_t raw_minus_mono_ns();   // camera timestamps are CLOCK_MONOTONIC_RAW

// /run/user/UID/frametop-hands, created private to the user if it's missing: where ft-camd's ring
// (FH_RING_NAME) and the hands and gestures files live. Not $XDG_RUNTIME_DIR: a terminal in
// the Frametop desktop has a private one of its own. And not /run/user/UID/frametop: that is
// the desktop session's private runtime folder, which it deletes whenever it starts.
std::string run_dir();
