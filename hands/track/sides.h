// Are the side cameras' images under the right names? fh-camd tells slam_left's buffers from
// slam_right's only by the order XRService allocated them, and some XRService starts reverse
// that order: then each side camera's images carry the other's name, every hand is seen by one
// camera only, at the wrong depth, and the cutouts land beside the hands.
//
// SideCheck tells from the hands the tracker already finds. Whenever a hand's landmarks are
// found in two cameras in the same frame set (one of them a side camera), the rays through its
// 21 landmarks are intersected twice: with the calibrations as the images are named, and with
// the two side cameras' calibrations exchanged. The same hand seen right meets within a few mm,
// in front of both cameras and as far away as its apparent size says; under the wrong naming
// the rays miss by centimetres or meet behind a camera. Each such pair is a vote. It decides once
// one way has min_clean votes and the other none, or min_votes with at most max_other of all
// votes the other way, over at least min_span_s of hands.
//
// The tracker's own views rarely give a pair when the names are wrong: it hands a hand over
// to the other cameras where the wrong calibration puts it, finds nothing there, and keeps it in
// one camera (the replays of swapped recordings: one hand, every handoff a miss, no pair). So
// probe() looks for itself, 5 times a second while it's checking: it takes a hand the tracker
// sees, places it in 3D from that one view (as far as its size says) with the side cameras
// exchanged, and runs the landmark model where that puts it in the other side camera, at 0.8,
// 1 and 1.25 times the one-view distance. That's the tracker's handoff under the other naming
// (the tracker does it under the current one every step); the palm detector misses many of
// these hands, which the landmark model finds from a good crop. What it finds is a view
// like the tracker's, and the test above votes on it: finding a hand there proves nothing by
// itself. It doesn't depend on how the tracker paired the views up.
//
// FH_SIDES_DEBUG=1 in the environment prints each probe's crop and what it found (stderr).
//
// Upper-camera pairs with a side camera count too (the upper pair's own naming was checked
// stable: tools/check_sides.py --pair upper). Two upper views alone say nothing.
#pragma once

#include "tracker.h"

#include <map>
#include <string>
#include <vector>

class SideCheck {
public:
    enum Verdict { Undecided, AsNamed, Swapped };

    // cams: the mono cameras' calibrations; slam_left and slam_right are needed.
    explicit SideCheck(const std::map<std::string, Camera> &cams);
    bool usable() const { return left_ && right_; }

    // One step's fresh views (Tracker::views_now, plus probe()'s), cameras named as the
    // images are now. Returns how many pairs voted.
    int add(const std::vector<Seen> &views, int64_t t_ns);
    // At most every probe_interval_s: for each naming, one view the tracker has, looked for in
    // the other side camera where that naming puts it (see the top). Returns the views found
    // (presence 0.5 or more), for add() with the tracker's. Runs the models on pool.
    std::vector<Seen> probe(const Nets &nets, Pool &pool, const std::map<std::string, Image> &images,
                            const std::vector<Seen> &views, int64_t t_ns);
    double probe_interval_s = 0.2;
    int probes = 0, probe_hits = 0;   // landmark model runs, and the views they found

    // One pair: the median landmark ray miss (m) as named [0] and with the side cameras
    // exchanged [1]; -1 where they can't be one hand that way (rays meet behind a camera, or
    // at a distance the hand's apparent size rules out). For tests.
    struct Miss {
        double m[2];
    };
    Miss test(const std::string &cam_a, const Landmarks &a, const std::string &cam_b, const Landmarks &b) const;
    // The vote for a Miss: 0 as named, 1 swapped, -1 neither.
    static int vote(const Miss &m);

    Verdict verdict() const;
    void reset();

    // Evidence so far
    int votes[2] = {0, 0};
    double span_s() const { return first_ns_ >= 0 ? (last_ns_ - first_ns_) / 1e9 : 0; }
    double median_miss_mm(int way) const;   // over the voted pairs; -1 if none met that way
    std::string summary() const;            // for the log
    std::string json() const;               // for the sides file and recordings

    // The rule (see the top)
    int min_clean = 10, min_votes = 20;
    double min_span_s = 1.0, max_other = 0.2;
    static constexpr double kGood = 0.015;   // m: a pair's median miss to count as meeting
    static constexpr double kClear = 3.0;    // both meet: one must miss this many times less

private:
    const Camera *cam(const std::string &name, bool swapped) const;
    double miss(const Camera &ca, const Landmarks &a, const Camera &cb, const Landmarks &b) const;

    std::map<std::string, const Camera *> cams_;
    const Camera *left_ = nullptr, *right_ = nullptr;
    std::vector<double> misses_[2];   // per voted pair, each way (-1: implausible)
    int64_t first_ns_ = -1, last_ns_ = -1;   // the first and last vote
    int64_t probe_ns_ = -1;
    unsigned probe_turn_ = 0;
};
