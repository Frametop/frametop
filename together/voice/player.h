// The room's voices on their way to your ears: while they are wanted, a thread of its own
// takes the next 20 ms of the mix (voices.h) and writes it to the loudspeakers, which keep its
// time.
#pragma once

#include "sound.h"
#include "voices.h"

#include <memory>

namespace voice {

class Player {
public:
    static constexpr double kRetryS = 3;   // between tries to open loudspeakers that wouldn't

    // `open`: how the loudspeakers are opened; `voices`: what is played.
    void Start(OpenSink open, std::shared_ptr<Voices> voices);
    void Stop();
    void Want(bool on);

    bool open() const;
    bool failed() const;
    Player() = default;
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;
    ~Player() { Stop(); }

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

}  // namespace voice
