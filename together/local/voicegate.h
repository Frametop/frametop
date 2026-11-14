// The voice ft-together passes between its links and this Frame's own program (ft-roomvoice;
// proto/voicewire.h has the bytes).
//
// In, from a link: a datagram is passed on only when it is a voice datagram, newer than the
// last one that link sent, and within what a link may send in a second (linkgate.h). One that
// comes after a newer one is dropped: it would be played too late anyway.
//
// Out, to the links: what this Frame's own program hands over goes as it is, if it is a voice
// datagram. Its count is the program's own (it follows the clock, so it goes on when the
// program starts again).
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/voicewire.h"
#include "linkgate.h"

namespace local {

class VoiceGate {
public:
    static constexpr double kASecond = 150;   // taken from one link a second, three times what a Frame sends
    static constexpr double kAtOnce = 50;     // ...and this many may come in one go after a quiet moment

    // Whether the datagram that came on `link` at `now` (seconds) is passed on.
    bool Take(int link, const uint8_t *data, size_t n, double now) {
        proto::Voice voice;
        return gate_.Count(link, now) && proto::ReadVoice(data, n, voice) && gate_.Newer(link, voice.count);
    }
    void Forget(int link) { gate_.Forget(link); }

private:
    LinkGate gate_{kASecond, kAtOnce};
};

}  // namespace local
