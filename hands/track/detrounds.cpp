#include "detrounds.h"

#include <chrono>

namespace {

double ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

void DetectorRounds::begin_step() {
    ready_.clear();
    if (!d_.pending()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::string cam;
    std::vector<Palm> palms;
    if (d_.collect(cam, palms)) {
        ready_.push_back({cam, std::move(palms)});
        ++collected;
        healthy_ = true;
    } else {
        ++failed;
        healthy_ = false;
        queue_.clear();
    }
    blocked_ms += ms_between(t0, std::chrono::steady_clock::now());
}

void DetectorRounds::start(const std::vector<std::string> &cameras) {
    if (running()) return;
    queue_ = cameras;
}

void DetectorRounds::end_step(const std::map<std::string, Image> &images) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!queue_.empty() && !d_.pending()) {
        const std::string cam = queue_.front();
        queue_.erase(queue_.begin());
        const auto it = images.find(cam);
        if (it == images.end()) continue;
        if (d_.post(it->second, cam)) {
            healthy_ = true;
        } else {
            ++failed;
            healthy_ = false;
            queue_.clear();
        }
    }
    blocked_ms += ms_between(t0, std::chrono::steady_clock::now());
}
