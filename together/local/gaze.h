// Where the wearer looks, on its way into their poses (docs/together.md, "Gaze"): not shared
// unless they turn it on, and then only a direction, smoothed by the program that tells it
// (ft-roomgaze), never a picture of an eye.
//
// Three things are kept:
//  - Whether the wearer shares where they look. They turn it on in a room, for that room: out
//    of it, it is off again.
//  - The direction last told, while it is shared. One that isn't told again within kHeldS
//    isn't the wearer's any more: their poses go out without one.
//  - Whether the wearer's eyes move their pointer (gaze mode), as the program that knows says
//    every second. A pointer that follows the eyes says where they look, so while that isn't
//    shared the pointer is `quiet` (hush.h). Not said within kPointerS counts as said that they
//    do: what isn't known isn't sent.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/posewire.h"

#include <algorithm>
#include <cmath>

namespace local {

class Gaze {
public:
    static constexpr double kHeldS = 1.0;      // a direction is told about five times a second
    static constexpr double kPointerS = 5.0;   // whether the eyes move the pointer is said each second

    // The wearer shares where they look from now on, or no longer does.
    void Share(bool on) {
        shared_ = on;
        if (!on) told_ = kNever;
    }
    // The room was left: the next one is told nothing until its wearer says so.
    void Lift() { Share(false); }

    // Where they look at `now` (seconds): to their left and up, in degrees from the way the
    // head faces. False, and nothing is kept, while it isn't shared or isn't a direction.
    bool Say(double yaw, double pitch, double now) {
        if (!shared_ || !std::isfinite(yaw) || !std::isfinite(pitch)) return false;
        yaw_ = float(std::clamp(yaw, -double(proto::kMostGaze), double(proto::kMostGaze)));
        pitch_ = float(std::clamp(pitch, -double(proto::kMostGaze), double(proto::kMostGaze)));
        told_ = now;
        return true;
    }
    // Whether the wearer's eyes move their pointer, as of `now`.
    void Pointer(bool eyes, double now) { eyes_ = eyes, pointerSaid_ = now; }

    bool shared() const { return shared_; }
    // A direction was told lately enough to be where they look now. None is kept from before
    // it was shared.
    bool fresh(double now) const { return now >= told_ && now - told_ <= kHeldS; }
    // The pointer would say where they look, and they haven't said it may.
    bool quiet(double now) const { return !shared_ && (eyes_ || !(now >= pointerSaid_ && now - pointerSaid_ <= kPointerS)); }

    // Into a pose on its way out. Whatever the pose said of a gaze itself is gone: the program
    // that handed it over isn't the one to say.
    void Into(proto::Pose &pose, double now) const {
        pose.looks = fresh(now);
        pose.gaze[0] = pose.looks ? yaw_ : 0, pose.gaze[1] = pose.looks ? pitch_ : 0;
    }

private:
    static constexpr double kNever = -1e18;
    bool shared_ = false, eyes_ = true;
    float yaw_ = 0, pitch_ = 0;
    double told_ = kNever, pointerSaid_ = kNever;
};

}  // namespace local
