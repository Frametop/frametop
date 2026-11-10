// ft-avatars with a headset on: the mirror. A sheet of glass stands on the floor in front of
// you (mirror.h) and shows your avatar's body, posed from your head and your tracked hands
// (wearer.h, the pose solver), to each eye (mirrorview.h), as an overlay SteamVR shows in the
// room. Its face is yours too: the mouth moves with your voice and the eyes blink
// (../face/ownface.h). It runs while the mirror is wanted and no longer: Settings starts it and
// tells it to quit.
//
// Commands, one datagram each on @ft_avatars, answered to the asker:
//   state         -> "ok " and the last status line
//   model PATH    -> another model file's body from now on: "ok", or "error" and why (the body
//                    worn stays)
//   place         -> the glass stands in front of you again, where you look now: "ok"
//   below on|off  -> your own body is drawn below you when you look down (below.h), or not:
//                    "ok", or "error" and why. An experiment (docs/together.md, T5)
//   quit          -> "ok", and it ends
#pragma once

#include "below.h"

#include <string>

namespace live {

struct Ask {
    std::string model;     // a model file with a body
    int height = 960;      // each eye's picture of the glass, pixels tall
    double seconds = 0;    // end after this long (0: when told to)
    double status = 0;     // a status line on stdout this often, seconds (0: none)
    bool voice = true;     // the mouth moves with what the microphone hears
    bool below = false;    // your own body is drawn below you from the start
    int belowSide = 1024;  // each eye's picture of it, pixels a side
    double belowFrom = below::kLooksDown;   // ...drawn once the head looks down this far, radians under level
};

// Runs until `quit`, SteamVR quitting, a signal, or `seconds`. The program's exit status, with a
// line on stderr when it isn't 0.
int Mirror(const Ask &ask);

}  // namespace live
