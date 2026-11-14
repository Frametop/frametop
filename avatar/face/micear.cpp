// The wearer's microphone, heard (micear.h).
#include "micear.h"

#include <pulse/error.h>
#include <pulse/simple.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace micear {

namespace {
constexpr size_t kPiece = kRate / 50;   // 20 ms at a time
constexpr size_t kKept = kRate / 2;     // what nobody took in half a second is dropped
}  // namespace

struct Ear::Heard {
    std::mutex mu;
    std::vector<float> samples;   // oldest first
    std::atomic<bool> stop{false}, up{false}, done{false};
};

void Ear::Open(const char *name) {
    Close();
    heard_ = std::make_shared<Heard>();
    std::thread([heard = heard_, name = std::string(name)] {
        const pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE, kRate, 1};
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1), attr.prebuf = uint32_t(-1), attr.minreq = uint32_t(-1), attr.tlength = uint32_t(-1);
        attr.fragsize = uint32_t(kPiece * sizeof(float));
        int err = 0;
        pa_simple *pa = pa_simple_new(nullptr, "Frametop", PA_STREAM_RECORD, nullptr, name.c_str(), &spec, nullptr, &attr, &err);
        if (!pa) std::fprintf(stderr, "ear: can't hear the microphone: %s\n", pa_strerror(err));
        heard->up = pa != nullptr;
        float piece[kPiece];
        while (pa && !heard->stop) {
            if (pa_simple_read(pa, piece, sizeof piece, &err) < 0) {
                std::fprintf(stderr, "ear: the microphone went: %s\n", pa_strerror(err));
                break;
            }
            std::lock_guard<std::mutex> lock(heard->mu);
            std::vector<float> &s = heard->samples;
            if (s.size() + kPiece > kKept) s.erase(s.begin(), s.begin() + std::ptrdiff_t(s.size() + kPiece - kKept));
            s.insert(s.end(), piece, piece + kPiece);
        }
        if (pa) pa_simple_free(pa);
        heard->up = false, heard->done = true;
    }).detach();
}

size_t Ear::Take(float *out, size_t most) {
    if (!heard_) return 0;
    std::lock_guard<std::mutex> lock(heard_->mu);
    std::vector<float> &s = heard_->samples;
    const size_t n = s.size() < most ? s.size() : most;
    if (n) std::memcpy(out, s.data() + (s.size() - n), n * sizeof(float));
    s.clear();
    return n;
}

bool Ear::hearing() const { return heard_ && heard_->up; }

// A recording stuck in pa_simple_read (it can't be interrupted: a source that stopped giving
// samples) is left behind rather than waited for; it ends with the program.
void Ear::Close() {
    if (!heard_) return;
    heard_->stop = true;
    for (int i = 0; i < 10 && !heard_->done; ++i) usleep(10'000);
    heard_.reset();
}

}  // namespace micear
