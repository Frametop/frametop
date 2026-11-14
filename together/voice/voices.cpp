// The room's voices, mixed (voices.h).
#include "voices.h"

#include "../proto/voicewire.h"

#include <algorithm>
#include <cmath>

namespace voice {

namespace {
bool Is(int place) { return place >= 0 && place < kVoices; }
}  // namespace

bool Voices::Heard(int place, const uint8_t *datagram, size_t n) {
    proto::Voice v;
    if (!Is(place) || !proto::ReadVoice(datagram, n, v)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    voices_[place].jitter.Push(v.count, v.opus, v.size);
    return true;
}

void Voices::Place(int place, bool known, const float at[3]) {
    if (!Is(place)) return;
    std::lock_guard<std::mutex> lock(mu_);
    Voice &v = voices_[place];
    v.placed = known;
    for (int i = 0; known && i < 3; ++i) v.at[i] = at[i];
}

void Voices::Listener(bool known, const double at[3], double yaw) {
    std::lock_guard<std::mutex> lock(mu_);
    listening_ = known;
    for (int i = 0; known && i < 3; ++i) ear_[i] = at[i];
    if (known) yaw_ = yaw;
}

void Voices::Forget(int place) {
    if (!Is(place)) return;
    std::lock_guard<std::mutex> lock(mu_);
    Voice &v = voices_[place];
    v.decoder.Close();
    v.jitter = Jitter(), v.placed = false, v.ears = kMiddle, v.level = 0;
}

void Voices::Mix(float *stereo) {
    std::fill(stereo, stereo + 2 * kFrame, 0.0f);
    std::lock_guard<std::mutex> lock(mu_);
    float pcm[kFrame];
    for (Voice &v : voices_) {
        const Jitter::Due due = v.jitter.Pull();
        if (due.what == Jitter::kNothing) {
            v.level *= 0.5f;
            continue;
        }
        if (!v.decoder.open() && !v.decoder.Open()) continue;
        if (due.what == Jitter::kPacket) v.decoder.Decode(due.opus, due.size, pcm);
        else if (due.what == Jitter::kFromNext) v.decoder.FromNext(due.opus, due.size, pcm);
        else v.decoder.Fill(pcm);
        const Ears to = v.placed && listening_ ? Hear(ear_, yaw_, v.at) : kMiddle;
        double squares = 0;
        for (int i = 0; i < kFrame; ++i) {
            const float t = float(i + 1) / kFrame;   // from where the ears were to where they are, over the 20 ms
            stereo[2 * i] += pcm[i] * (v.ears.left + (to.left - v.ears.left) * t);
            stereo[2 * i + 1] += pcm[i] * (v.ears.right + (to.right - v.ears.right) * t);
            squares += double(pcm[i]) * pcm[i];
        }
        v.ears = to;
        v.level = 0.5f * v.level + 0.5f * float(std::fmin(1.0, std::sqrt(squares / kFrame) * 4));
    }
    for (int i = 0; i < 2 * kFrame; ++i) stereo[i] = std::clamp(stereo[i], -1.0f, 1.0f);
}

Voices::Told Voices::told(int place) {
    Told t;
    if (!Is(place)) return t;
    std::lock_guard<std::mutex> lock(mu_);
    const Voice &v = voices_[place];
    t.speaking = v.jitter.playing() && v.level >= kHeard, t.placed = v.placed, t.level = v.level;
    t.played = v.jitter.played(), t.lost = v.jitter.lost(), t.late = v.jitter.late();
    return t;
}

}  // namespace voice
