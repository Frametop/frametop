// A shared panel's bitrate (docs/together.md, "What a guest sees", Quality): it follows the
// slowest link that watches the panel, up to the cap its owner picked.
//
// What says a link is too slow is a picture given up on it: one that wasn't all acknowledged in
// time (net/). Two of those within a second bring the bitrate down by a third or so; five
// seconds of sending without one bring it up a little, and so on up to the cap. It starts in
// the middle.
//
// The rules only, one object for each panel this Frame shares. Header only.
#pragma once

#include <algorithm>

namespace flow {

class Pace {
public:
    static constexpr int kStart = 2'000'000, kLeast = 300'000;   // bits a second
    static constexpr int kDown = 70, kUp = 115;                  // percent of what it was
    static constexpr double kWindow = 1, kClean = 5;             // seconds
    static constexpr int kGiveUps = 2;                           // within kWindow: slower

    explicit Pace(int cap) { SetCap(cap); }

    // The owner's cap changed. The bitrate comes down to it at once, and climbs to it in time.
    void SetCap(int cap) {
        cap_ = std::max(cap, kLeast);
        rate_ = std::min(rate_, cap_);
    }

    // A picture of this panel was given up on some link at `now` (seconds). True when the
    // bitrate fell.
    bool GaveUp(double now) {
        if (count_ == 0 || now - first_ > kWindow) first_ = now, count_ = 0;
        last_ = now, any_ = true;
        if (++count_ < kGiveUps) return false;
        count_ = 0;
        const int was = rate_;
        rate_ = std::max(kLeast, int(rate_ * 1LL * kDown / 100));
        return rate_ != was;
    }

    // Nothing is being sent at `now`: a stretch with no picture given up starts when one is.
    void Idle(double now) { last_ = now, any_ = true; }

    // Time passes with pictures being sent. True when the bitrate rose.
    bool Tick(double now) {
        if (!any_) last_ = now, any_ = true;   // the first moment counts as the last change
        if (now - last_ < kClean || rate_ >= cap_) return false;
        rate_ = std::min(cap_, int(rate_ * 1LL * kUp / 100));
        last_ = now;
        return true;
    }

    int bitrate() const { return rate_; }

private:
    int cap_ = kStart, rate_ = kStart;
    int count_ = 0;        // pictures given up since first_
    double first_ = 0;
    double last_ = 0;      // the last give-up or rise
    bool any_ = false;
};

}  // namespace flow
