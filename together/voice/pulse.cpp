// The Frame's microphone and loudspeakers (sound.h).
#include "codec.h"
#include "sound.h"

#include <pulse/error.h>
#include <pulse/simple.h>

namespace voice {

namespace {

struct Pulse {
    pa_simple *pa = nullptr;
    ~Pulse() {
        if (pa) pa_simple_free(pa);
    }
    bool Open(pa_stream_direction_t way, uint8_t channels, const pa_buffer_attr &attr, const char *what, std::string &err) {
        const pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE, kRate, channels};
        int code = 0;
        pa = pa_simple_new(nullptr, "Frametop", way, nullptr, what, &spec, nullptr, &attr, &code);
        if (!pa) err = pa_strerror(code);
        return pa != nullptr;
    }
};

pa_buffer_attr Attr() {
    pa_buffer_attr attr;
    attr.maxlength = attr.tlength = attr.prebuf = attr.minreq = attr.fragsize = uint32_t(-1);
    return attr;
}

struct Mic : Source {
    Pulse pulse;
    bool Read(float *frame) override {
        int code = 0;
        return pa_simple_read(pulse.pa, frame, kFrame * sizeof(float), &code) >= 0;
    }
};

struct Speakers : Sink {
    Pulse pulse;
    bool Write(const float *stereo) override {
        int code = 0;
        return pa_simple_write(pulse.pa, stereo, 2 * kFrame * sizeof(float), &code) >= 0;
    }
};

}  // namespace

std::unique_ptr<Source> Microphone(std::string &err) {
    auto mic = std::make_unique<Mic>();
    pa_buffer_attr attr = Attr();
    attr.fragsize = uint32_t(kFrame * sizeof(float));   // 20 ms at a time: no more delay than that
    if (!mic->pulse.Open(PA_STREAM_RECORD, 1, attr, "Your voice in the room", err)) return nullptr;
    return mic;
}

std::unique_ptr<Sink> Loudspeakers(std::string &err) {
    auto out = std::make_unique<Speakers>();
    pa_buffer_attr attr = Attr();
    attr.tlength = uint32_t(3 * 2 * kFrame * sizeof(float));   // about 60 ms queued
    if (!out->pulse.Open(PA_STREAM_PLAYBACK, 2, attr, "The room's voices", err)) return nullptr;
    return out;
}

}  // namespace voice
