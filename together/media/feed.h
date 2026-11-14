// Where a shared panel's pictures come from, as ft-sharepanel's loop sees it: a screen of
// ft-screens (screenfeed.h), or a test picture (patternfeed.h).
#pragma once

#include "pictures.h"

#include <string>

namespace media {

class Feed {
public:
    virtual ~Feed() = default;

    // To wait on with poll (readable: Serve has something to take), or -1.
    virtual int fd() const = 0;
    // Takes what its far end said. False when the feed is gone for good.
    virtual bool Serve() = 0;

    // Someone watches: pictures are wanted from now, drawn into `pictures`. False with `err`
    // when it can't draw into them.
    virtual bool Begin(const Pictures &pictures, std::string &err) = 0;
    // Nobody does, and `pictures` are about to go.
    virtual void End() = 0;
    // Between the two: its picture as it is now is wanted once more, drawn before or not (someone
    // began to watch after pictures were let go with Skip).
    virtual void Fresh() = 0;

    // At most that many pictures a second are wanted from now on, of the stream's own rate at
    // the most: the newest when its turn comes.
    virtual void AtMost(int aSecond) = 0;
    // Seconds from `now` until it may have a picture, at most.
    virtual double Wait(double now) const = 0;
    // Draws its newest picture into picture `into`, if it has one that wasn't drawn yet.
    virtual bool Draw(Pictures &pictures, int into, double now) = 0;
    // Its newest picture isn't wanted (nobody watches just now): it is let go undrawn.
    virtual void Skip(double now) = 0;
};

}  // namespace media
