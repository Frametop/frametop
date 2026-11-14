// The room's voices on their way to your ears (player.h).
#include "player.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <unistd.h>

namespace voice {

// What the thread and its owner share. The thread holds it and the voices too, so one that is
// stuck in a write is left behind at Stop, and ends with the program.
struct Player::Shared {
    std::atomic<bool> stop{false}, want{false}, open{false}, failed{false}, done{false};
    OpenSink sink;
};

void Player::Start(OpenSink open, std::shared_ptr<Voices> voices) {
    Stop();
    auto shared = std::make_shared<Shared>();
    shared->sink = std::move(open);
    shared_ = shared;
    std::thread([s = shared, voices = std::move(voices)] {
        std::unique_ptr<Sink> out;
        float stereo[2 * kFrame];
        const auto rest = [&s](double seconds) {
            for (int i = 0; i < int(seconds * 20) && !s->stop; ++i) usleep(50'000);
        };
        while (!s->stop) {
            if (!s->want) {
                if (out) out.reset(), s->open = false;
                rest(0.05);
                continue;
            }
            if (!out) {
                std::string err;
                out = s->sink(err);
                s->failed = !out;
                if (!out) {
                    std::fprintf(stderr, "ft-roomvoice: the loudspeakers: %s\n", err.c_str());
                    rest(kRetryS);
                    continue;
                }
                s->open = true;
            }
            voices->Mix(stereo);
            if (!out->Write(stereo)) {   // they went: opened again next time round
                out.reset(), s->open = false;
                rest(0.5);
            }
        }
        s->open = false, s->done = true;
    }).detach();
}

void Player::Stop() {
    if (!shared_) return;
    shared_->stop = true;
    for (int i = 0; i < 50 && !shared_->done; ++i) usleep(10'000);
    shared_.reset();
}

void Player::Want(bool on) {
    if (shared_) shared_->want = on;
}
bool Player::open() const { return shared_ && shared_->open; }
bool Player::failed() const { return shared_ && shared_->failed; }

}  // namespace voice
