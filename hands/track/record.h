// Recordings of frame sets, for replaying live sessions through the tracker offline
// (ft-handreplay). A recording is DIR/sets.bin: one record per frame set, each
//   fh_set_hdr_t, then per camera fh_set_cam_t, then each camera's pixels (w x h, packed)
// in the same camera order.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define FH_SET_MAGIC "FHSET01"

struct fh_set_hdr_t {
    char magic[8];
    uint32_t ncams;
    uint32_t bytes;   // the whole record, this header included
};

struct fh_set_cam_t {
    char name[16];          // calibration name, e.g. "slam_left"
    uint32_t width, height;
    uint64_t capture_ns;    // CLOCK_MONOTONIC_RAW, as the ring has it
    uint64_t dqbuf_ns;      // CLOCK_MONOTONIC
};

struct SetFrame {
    std::string name;
    const uint8_t *px;
    uint32_t width, height;
    uint64_t capture_ns, dqbuf_ns;
};

// Writes sets on its own thread, so a slow disk never holds up tracking; drops sets
// when too many are waiting.
class Recorder {
public:
    ~Recorder();
    bool open(const std::string &dir, std::string &err);
    void add(const std::vector<SetFrame> &frames);
    // Sets taken (not dropped for a full queue): the next one's place in the file, unless a
    // write fails.
    size_t added() const { return added_; }
    size_t written() const { return written_; }
    size_t dropped() const { return dropped_; }

private:
    void loop();
    FILE *f_ = nullptr;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable wake_;
    std::deque<std::vector<uint8_t>> queue_;
    bool stop_ = false;
    size_t written_ = 0, dropped_ = 0, added_ = 0;
};

// Reads a recording back one set at a time: DIR/sets.bin, then the hand recorder's later
// parts (sets-2.bin, sets-3.bin, ...) if there are any.
class SetReader {
public:
    bool open(const std::string &dir, std::string &err);
    // False at the end (or on a truncated last set).
    bool next(std::vector<fh_set_cam_t> &cams, std::vector<std::vector<uint8_t>> &pixels);
    ~SetReader();

private:
    FILE *f_ = nullptr;
    std::string dir_;
    int part_ = 1;
};
