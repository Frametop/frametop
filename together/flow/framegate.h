// A watched panel's pictures put back in order (docs/together.md, "The protocol"). Each picture
// comes on a stream of its own, so one may overtake another, and one the sender gave up never
// comes. A decoder needs them in order with none missing since the last keyframe.
//
// A keyframe passes whenever it is newer than what passed last. Any other picture passes only as
// the one right after the last; one that is early is held (a few of them) until the gap before
// it fills or a keyframe makes the gap moot. The sender follows a picture it gave up with a
// keyframe by itself; when pictures are held and nothing has passed for kStuck seconds the
// viewer asks for one too, at most every kAskEvery seconds.
//
// The rules only, one object for each panel this Frame watches. Header only.
#pragma once

#include "../proto/framewire.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace flow {

class Order {
public:
    static constexpr size_t kHeld = 6;   // early pictures kept: a fifth of a second of video
    static constexpr double kStuck = 0.3, kAskEvery = 0.5;

    struct Frame {
        uint32_t number = 0;
        bool key = false;
        std::vector<uint8_t> unit;
    };

    // A picture came at `now` (seconds). Those to decode now are added to `out`, in order.
    void Take(Frame frame, double now, std::vector<Frame> &out) {
        if (going_ && !proto::LaterFrame(frame.number, last_)) return;   // old, or here twice
        if (!frame.key && !(going_ && frame.number == last_ + 1)) {
            Hold(std::move(frame), now);
            return;
        }
        Pass(std::move(frame), out);
        for (bool more = true; more;) {
            more = false;
            for (size_t i = 0; i < held_.size(); ++i) {
                if (held_[i].number != last_ + 1) continue;
                Frame next = std::move(held_[i]);
                held_.erase(held_.begin() + long(i));
                Pass(std::move(next), out);
                more = true;
                break;
            }
        }
        held_.erase(std::remove_if(held_.begin(), held_.end(), [this](const Frame &f) { return !proto::LaterFrame(f.number, last_); }), held_.end());
        since_ = now;
    }

    // Whether to ask the panel's owner for a keyframe at `now`. True once per ask.
    bool WantsKey(double now) {
        if (held_.empty() || now - since_ < kStuck || (asked_ && now - askedAt_ < kAskEvery)) return false;
        asked_ = true, askedAt_ = now;
        return true;
    }

    // The decoder starts again: only a keyframe will do, whatever its number.
    void Reset() {
        going_ = false, asked_ = false;
        held_.clear();
    }

    size_t held() const { return held_.size(); }

private:
    void Pass(Frame frame, std::vector<Frame> &out) {
        going_ = true, last_ = frame.number;
        out.push_back(std::move(frame));
    }
    void Hold(Frame frame, double now) {
        for (const Frame &f : held_)
            if (f.number == frame.number) return;
        if (held_.empty()) since_ = now;
        held_.push_back(std::move(frame));
        if (held_.size() <= kHeld) return;
        // too many: the earliest goes, as the gap before it is the least likely to fill
        size_t first = 0;
        for (size_t i = 1; i < held_.size(); ++i)
            if (proto::LaterFrame(held_[first].number, held_[i].number)) first = i;
        held_.erase(held_.begin() + long(first));
    }

    bool going_ = false;   // a keyframe passed: last_ is the last picture that did
    uint32_t last_ = 0;
    std::vector<Frame> held_;
    double since_ = 0;     // when a picture last passed, or the first of those held came
    bool asked_ = false;
    double askedAt_ = 0;
};

}  // namespace flow
