// One person's voice between the link and the loudspeakers (docs/together.md, "Sound"): the
// packets as they come, each 20 ms and counted (proto/voicewire.h), and which of them is played
// every 20 ms.
//
// A voice starts kDelay packets after its first one came, so those that come a little late or
// in a bunch are still in time. From then on the count says what is due:
//
//   the packet that is due      it is played
//   not it, but the one after   it is made up from that one (Opus carries a thin copy of the
//                               packet before)
//   neither                     the decoder fills in, kFilled times in a row; after that there
//                               is silence, and with nothing waiting the voice has stopped:
//                               the next packet starts it again, kDelay behind once more
//
// Silence isn't sent, so every pause ends that way and each sentence finds its own timing. A
// packet that is due before it came (the two clocks drift, or the link stalled) starts the
// voice again from that packet, and when the one due is more than kMost behind the newest, the
// oldest are skipped.
//
// The rules only: no sound, no clock. Header only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace voice {

class Jitter {
public:
    static constexpr int kDelay = 3;    // packets (60 ms) between a voice's first one and its start
    static constexpr int kMost = 12;    // how far behind the newest packet the one due may be
    static constexpr int kFilled = 5;   // missing in a row that the decoder fills in

    enum What {
        kNothing,   // silence
        kPacket,    // decode `opus`
        kFromNext,  // the due one is lost: make it up from `opus`, the one after
        kFill,      // nothing to go by: the decoder fills in
    };
    struct Due {
        What what = kNothing;
        const uint8_t *opus = nullptr;   // until the next Push or Pull
        size_t size = 0;
    };

    // A packet came, counted `count`.
    void Push(uint16_t count, const uint8_t *opus, size_t size) {
        const int64_t at = any_ ? last_ + int16_t(uint16_t(count - uint16_t(last_))) : count;
        if (!any_ || at > last_) any_ = true, last_ = at;
        if (playing_ && at < play_) {
            if (!waiting_.empty()) return void(++late_);   // its turn is over, and newer ones wait
            playing_ = false;                              // we ran ahead of the sender: start again from it
        }
        waiting_[at].assign(opus, opus + size);
        if (!playing_) playing_ = true, play_ = at, hold_ = kDelay, missed_ = 0;
        if (const int64_t newest = waiting_.rbegin()->first; newest - play_ > kMost) {   // too far behind: skip ahead
            play_ = newest - kDelay;
            while (waiting_.begin()->first < play_) ++skipped_, waiting_.erase(waiting_.begin());
        }
    }

    // What is played for the next 20 ms.
    Due Pull() {
        Due due;
        if (!playing_) return due;
        if (hold_ > 0) return --hold_, due;
        const auto it = waiting_.find(play_);
        if (it != waiting_.end()) {
            out_.swap(it->second);
            waiting_.erase(it);
            missed_ = 0, ++played_;
            due.what = kPacket;
        } else if (const auto next = waiting_.find(play_ + 1); next != waiting_.end()) {
            out_ = next->second;
            missed_ = 0, ++lost_;
            due.what = kFromNext;
        } else {
            if (++missed_ > kFilled) {
                if (waiting_.empty()) playing_ = false, --lost_;   // the voice's end, not a loss
                return ++play_, due;
            }
            if (missed_ == 1) ++lost_;   // taken back if this turns out to be where the voice ends
            due.what = kFill;
        }
        ++play_;
        if (due.what != kFill) due.opus = out_.data(), due.size = out_.size();
        return due;
    }

    bool playing() const { return playing_; }
    size_t waiting() const { return waiting_.size(); }
    unsigned played() const { return played_; }
    unsigned lost() const { return lost_; }     // not there when due, with more of the voice after them (a gap counts once)
    unsigned late() const { return late_; }     // came after their turn
    unsigned skipped() const { return skipped_; }

private:
    bool any_ = false, playing_ = false;
    int64_t last_ = 0, play_ = 0;   // the newest packet's count and the one due, both without the wrap
    int hold_ = 0, missed_ = 0;
    std::map<int64_t, std::vector<uint8_t>> waiting_;
    std::vector<uint8_t> out_;
    unsigned played_ = 0, lost_ = 0, late_ = 0, skipped_ = 0;
};

}  // namespace voice
