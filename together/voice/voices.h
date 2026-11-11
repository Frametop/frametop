// The voices of the room's other people, mixed for the two ears (docs/together.md, "Sound"):
// each person's packets go in as they come, where everyone's head is goes in as it is known,
// and every 20 ms the next 20 ms of all of them comes out.
//
// One person is one decoder, one order of packets (jitter.h) and one place (spatial.h). How
// loud they are in each ear follows their place smoothly, so a head that turns makes no click.
//
// It is asked from two threads, the one that takes what comes and the one that plays, and
// keeps its own lock.
#pragma once

#include "codec.h"
#include "jitter.h"
#include "spatial.h"

#include <mutex>

namespace voice {

constexpr int kVoices = 4;   // the places in a room (proto::kPeople)
constexpr float kHeard = 0.02f;   // a voice's level from which its owner is told of as speaking

class Voices {
public:
    // A voice datagram of the person at `place` (proto/voicewire.h). False when it isn't one.
    bool Heard(int place, const uint8_t *datagram, size_t n);
    // Where their head is in the room; `known` false while nobody knows.
    void Place(int place, bool known, const float at[3]);
    // Where the listener's head is in the room and the way it faces (spatial.h).
    void Listener(bool known, const double at[3], double yaw);
    // The person at `place` left, or someone else has it now: what was theirs is dropped.
    void Forget(int place);

    // The next 20 ms for the loudspeakers: kFrame times left, right.
    void Mix(float *stereo);

    struct Told {
        bool speaking = false, placed = false;   // speaking: heard now, not the hiss an encoder sends between words
        float level = 0;   // how loud they are now, 0 to 1
        unsigned played = 0, lost = 0, late = 0;
    };
    Told told(int place);

private:
    struct Voice {
        Decoder decoder;
        Jitter jitter;
        bool placed = false;
        double at[3] = {};
        Ears ears = kMiddle;   // as they were at the end of the last 20 ms
        float level = 0;
    };
    std::mutex mu_;
    Voice voices_[kVoices];
    bool listening_ = false;   // the listener's place is known
    double ear_[3] = {}, yaw_ = 0;
};

}  // namespace voice
