// Your own voice on its way out (sender.h).
#include "sender.h"

#include "../../conf/ftsock.h"
#include "../proto/voicewire.h"
#include "codec.h"
#include "mouthsay.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <vector>

namespace voice {

// What the thread and its owner share. The thread holds it too, so one that is stuck in a read
// (a microphone that stopped giving samples can't be interrupted) is left behind at Stop, and
// ends with the program.
struct Sender::Shared {
    std::atomic<bool> stop{false}, want{false}, open{false}, failed{false}, done{false};
    std::atomic<unsigned> sent{0};
    std::atomic<float> level{0};
    OpenSource source;
    int sock = -1;
    sockaddr_un to{};
    socklen_t toLen = 0;
    ~Shared() {
        if (sock >= 0) close(sock);
    }
};

namespace {

// Seconds slept in pieces, so a stop or a change of mind is seen soon. False when stopped.
bool Rest(const std::atomic<bool> &stop, double seconds) {
    for (int i = 0; i < int(seconds * 20) && !stop; ++i) usleep(50'000);
    return !stop;
}

uint16_t CountNow() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint16_t((int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1'000'000) / 20);
}

}  // namespace

bool Sender::Start(OpenSource open, const std::string &together) {
    Stop();
    auto shared = std::make_shared<Shared>();
    shared->source = std::move(open);
    shared->toLen = ft_sock_addr(together.c_str(), &shared->to);
    shared->sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (!shared->toLen || shared->sock < 0) return false;
    shared_ = shared;
    std::thread([s = shared] {
        Encoder encoder;
        std::unique_ptr<Source> mic;
        uint16_t count = 0;
        float frame[kFrame];
        uint8_t opus[proto::kMostVoice];
        MouthSay mouth;
        uint8_t said[local::kMouthBytes];
        const auto hand = [&s](const uint8_t *data, size_t n) {
            return sendto(s->sock, data, n, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&s->to), s->toLen) == ssize_t(n);
        };
        const auto shut = [&] {   // the microphone, and with it the wearer's mouth
            mic.reset(), s->open = false, s->level = 0;
            if (mouth.Shut(said)) hand(said, sizeof said);
        };
        while (!s->stop) {
            if (!s->want) {
                if (mic) shut();
                Rest(s->stop, 0.05);
                continue;
            }
            if (!mic) {
                std::string err;
                mic = s->source(err);
                s->failed = !mic || !encoder.Open();
                if (s->failed) {
                    std::fprintf(stderr, "ft-roomvoice: the microphone: %s\n", err.empty() ? "no encoder" : err.c_str());
                    mic.reset();
                    Rest(s->stop, kRetryS);
                    continue;
                }
                s->open = true, count = CountNow();
            }
            if (!mic->Read(frame)) {   // it went: opened again next time round
                shut();
                Rest(s->stop, 0.5);
                continue;
            }
            ++count;
            double squares = 0;
            for (float v : frame) squares += double(v) * v;
            s->level = 0.5f * s->level + 0.5f * float(std::fmin(1.0, std::sqrt(squares / kFrame) * 4));
            const size_t n = encoder.Encode(frame, opus, sizeof opus);
            const std::vector<uint8_t> out = proto::PackVoice(count, opus, n);
            if (!out.empty() && hand(out.data(), out.size())) ++s->sent;
            if (mouth.Hear(frame, kFrame, kRate, said)) hand(said, sizeof said);
        }
        if (mouth.Shut(said)) hand(said, sizeof said);
        s->open = false, s->done = true;
    }).detach();
    return true;
}

void Sender::Stop() {
    if (!shared_) return;
    shared_->stop = true;
    for (int i = 0; i < 50 && !shared_->done; ++i) usleep(10'000);
    shared_.reset();
}

void Sender::Want(bool on) {
    if (shared_) shared_->want = on;
}
bool Sender::open() const { return shared_ && shared_->open; }
bool Sender::failed() const { return shared_ && shared_->failed; }
unsigned Sender::sent() const { return shared_ ? shared_->sent.load() : 0; }
float Sender::level() const { return shared_ ? shared_->level.load() : 0; }

}  // namespace voice
