// A shared panel's video by how much its viewers see of it (docs/together.md, "Panels"): it is
// encoded at its full rate while anyone has it focused and at a third of that while it is only
// in view, and someone who doesn't see it at all gets none of it. That last takes kHiddenAfter
// seconds to hold: a head that turns away and back, or a panel that has just come, goes on
// getting pictures at the lesser rate meanwhile, and after that keeps the last one it had.
//
// The rules only. Header only.
#pragma once

#include "../proto/messages.h"

#include <algorithm>

namespace flow {

constexpr int kLeastPictures = 5;   // a second, of a panel someone sees

// Pictures a second for a panel whose most attentive viewer has `level` of it (proto::kHidden,
// kInView or kFocused), where `full` is what its owner lets it have.
constexpr int PicturesFor(int level, int full) {
    return level >= proto::kFocused ? full : level == proto::kInView ? std::min(full, std::max(kLeastPictures, full / 3)) : 0;
}

// What one viewer's panel has of them, and what its owner is told of that.
class Seen {
public:
    static constexpr double kHiddenAfter = 3;

    // What the panel has of its viewer at `now` (seconds).
    void Set(int level, double now) {
        if (level == proto::kHidden && level_ != proto::kHidden) hiddenAt_ = now;
        level_ = level;
    }
    // What its owner is to hear at `now`.
    int Told(double now) const {
        if (level_ != proto::kHidden) return level_;
        return now - hiddenAt_ < kHiddenAfter ? int(proto::kInView) : int(proto::kHidden);
    }

private:
    int level_ = proto::kInView;   // a panel that has just come gets its first pictures
    double hiddenAt_ = 0;   // when it last went out of view
};

}  // namespace flow
