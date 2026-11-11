// A microphone and loudspeakers that aren't (sound.h), for running ft-roomvoice where there is
// no sound, and for telling what went through it: a source that hums, and a sink that measures
// what it is given. Both keep time as the real ones do, 20 ms a turn.
//
// Header only.
#pragma once

#include "codec.h"
#include "sound.h"

#include <cmath>
#include <ctime>
#include <mutex>

namespace voice {

// Waits for the next 20 ms to be over, counted from when it was made.
class Pace {
public:
    Pace() { clock_gettime(CLOCK_MONOTONIC, &next_); }
    void Wait() {
        next_.tv_nsec += 20'000'000;
        if (next_.tv_nsec >= 1'000'000'000) next_.tv_nsec -= 1'000'000'000, ++next_.tv_sec;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, nullptr);
    }

private:
    timespec next_;
};

// A hum at `hz` that swells five times a second, as syllables do: a steady note counts as
// noise with a voice codec, and isn't sent for long.
class Hum : public Source {
public:
    explicit Hum(double hz) : hz_(hz) {}
    bool Read(float *frame) override {
        pace_.Wait();
        for (int i = 0; i < kFrame; ++i, ++at_) {
            const double t = double(at_) / kRate;
            frame[i] = float(0.3 * std::sin(2 * M_PI * hz_ * t) * (0.6 + 0.4 * std::sin(2 * M_PI * 5 * t)));
        }
        return true;
    }

private:
    const double hz_;
    long at_ = 0;
    Pace pace_;
};

// What a Probe was given over the last half second.
struct Probed {
    std::mutex mu;
    float left = 0, right = 0;   // how loud each ear was (root mean square)
    float hz = 0;                // the note, by how often the two ears' sum crossed nought
    unsigned frames = 0;         // 20 ms turns measured since it was made
};

class Probe : public Sink {
public:
    explicit Probe(std::shared_ptr<Probed> told) : told_(std::move(told)) {}
    bool Write(const float *stereo) override {
        pace_.Wait();
        for (int i = 0; i < kFrame; ++i) {
            const float l = stereo[2 * i], r = stereo[2 * i + 1], sum = l + r;
            left_ += double(l) * l, right_ += double(r) * r;
            if (sum != 0) {
                crossings_ += last_ != 0 && (sum > 0) != (last_ > 0);
                last_ = sum;
            }
        }
        if (++turns_ < kTurns) return true;
        const double samples = double(kTurns) * kFrame;
        std::lock_guard<std::mutex> lock(told_->mu);
        told_->left = float(std::sqrt(left_ / samples)), told_->right = float(std::sqrt(right_ / samples));
        told_->hz = float(crossings_ / 2.0 / (samples / kRate));
        told_->frames += kTurns;
        left_ = right_ = 0, crossings_ = turns_ = 0;
        return true;
    }

private:
    static constexpr int kTurns = 25;
    std::shared_ptr<Probed> told_;
    Pace pace_;
    double left_ = 0, right_ = 0;
    float last_ = 0;
    int crossings_ = 0, turns_ = 0;
};

}  // namespace voice
