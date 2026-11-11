// Where a voice comes from and goes to on this Frame: 20 ms at a time, 48 kHz (codec.h). The
// real ones are the Frame's microphone and loudspeakers (pulse.cpp); the pretend ones
// (pretend.h) let the rest be run where there is neither.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace voice {

struct Source {
    virtual ~Source() = default;
    // The next kFrame samples, one channel. It waits for them. False when the source is gone.
    virtual bool Read(float *frame) = 0;
};

struct Sink {
    virtual ~Sink() = default;
    // kFrame times left, right. It waits while more is queued than should be. False when the
    // sink is gone.
    virtual bool Write(const float *stereo) = 0;
};

// How one is opened, each time it is wanted: nullptr with why in `err`.
using OpenSource = std::function<std::unique_ptr<Source>(std::string &err)>;
using OpenSink = std::function<std::unique_ptr<Sink>(std::string &err)>;

// The Frame's own, through PipeWire's PulseAudio server: the default source (the microphone
// with the loudspeakers' echo taken out) and the default sink.
std::unique_ptr<Source> Microphone(std::string &err);
std::unique_ptr<Sink> Loudspeakers(std::string &err);

}  // namespace voice
