// A feed of test pictures (testpattern.h) at a steady rate, for ft-sharepanel --pattern: what a
// shared panel sends when there is no screen behind it, so the whole path can be tested with no
// desktop and no headset. Header only.
#pragma once

#include "feed.h"
#include "testpattern.h"

#include <algorithm>

namespace media {

class PatternFeed : public Feed {
public:
    explicit PatternFeed(int fps) : fps_(fps), every_(1.0 / fps) {}

    int fd() const override { return -1; }
    bool Serve() override { return true; }
    bool Begin(const Pictures &, std::string &) override {
        for (int &w : was_) w = -1;
        return true;
    }
    void End() override {}
    void Fresh() override {}
    void AtMost(int aSecond) override { every_ = 1.0 / std::clamp(aSecond, 1, fps_); }
    double Wait(double now) const override { return next_ > now ? next_ - now : 0; }
    bool Draw(Pictures &pictures, int into, double now) override {
        if (now < next_) return false;
        uint32_t stride = 0;
        uint8_t *px = pictures.Map(into, stride);
        if (!px) return false;
        const PictureLayout &l = pictures.layout();
        if (l.nv12) testpattern::DrawNv12(px, px + l.chroma / l.stride * stride, int(stride), l.width, l.height, frame_, was_[into]);
        else testpattern::Draw(px, int(stride), l.width, l.height, frame_, was_[into]);
        pictures.Unmap(into);
        was_[into] = frame_;
        frame_ = (frame_ + 1) & ((1 << testpattern::kBits) - 1);
        Skip(now);
        return true;
    }
    // The next picture is one period on; after falling behind, one period from now.
    void Skip(double now) override {
        if (now < next_) return;
        next_ = next_ + every_ > now ? next_ + every_ : now + every_;
    }

private:
    int fps_;
    double every_, next_ = 0;
    int frame_ = 0;
    int was_[Pictures::kCount] = {-1, -1, -1, -1};
};

}  // namespace media
