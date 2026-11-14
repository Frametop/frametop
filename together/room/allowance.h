// How often someone may do a thing: so many times a second, and so many at once after a quiet
// moment. What a peer may send is bounded in size by its reader and in how often by one of
// these, so a flood costs a check each and no more.
//
// The rule only. Header only.
#pragma once

#include <cmath>

namespace room {

class Allowance {
public:
    Allowance(double aSecond, double atOnce) : aSecond_(aSecond), atOnce_(atOnce), left_(atOnce) {}

    // Whether one more is within the allowance at `now` (seconds).
    bool Spend(double now) {
        if (known_) left_ = std::fmin(atOnce_, left_ + (now - at_) * aSecond_);
        known_ = true, at_ = now;
        if (left_ < 1) return false;
        left_ -= 1;
        return true;
    }

private:
    double aSecond_, atOnce_, left_, at_ = 0;
    bool known_ = false;
};

}  // namespace room
