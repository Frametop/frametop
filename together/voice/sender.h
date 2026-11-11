// Your own voice on its way out (docs/together.md, "Sound"): while it is wanted, a thread of
// its own reads the microphone 20 ms at a time, makes each an Opus packet, and hands those
// that aren't silence to ft-together as voice datagrams (proto/voicewire.h), which sends them
// to everyone in the room.
//
// The microphone is open only while the voice is wanted, so outside a room, muted or with the
// headset off nothing listens. Each packet's count is one more than the last, also for those
// not sent, and starts from the clock, so it goes on from about where it was when the program
// or the microphone starts again.
#pragma once

#include "sound.h"

#include <memory>
#include <string>

namespace voice {

class Sender {
public:
    static constexpr double kRetryS = 3;   // between tries to open a microphone that wouldn't

    // `open`: how the microphone is opened; `together`: ft-together's socket's name. False
    // when that isn't one.
    bool Start(OpenSource open, const std::string &together);
    void Stop();
    // Whether the voice is wanted from now on.
    void Want(bool on);

    bool open() const;       // the microphone is
    bool failed() const;     // it wouldn't open, the last time it was tried
    unsigned sent() const;   // packets handed to ft-together
    float level() const;     // how loud the wearer is now, 0 to 1
    Sender() = default;
    Sender(const Sender &) = delete;
    Sender &operator=(const Sender &) = delete;
    ~Sender() { Stop(); }

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

}  // namespace voice
