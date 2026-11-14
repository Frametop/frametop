// Someone in a shared room who isn't there: ft-avatars with no headset, telling ft-together
// the pose of a demo wearer (../view/demo.h) the way room.h tells a real one's, thirty times a
// second. With an ft-together of its own that joined a room, it is a fake peer to test with:
// the others see someone standing on their spot, waving, or wherever they are said to have
// walked to since they came in. No GPU or SteamVR.
#pragma once

#include "../../conf/ftsock.h"
#include "floor.h"

#include <string>

namespace pretend {

struct Ask {
    std::string pose = "wave";                 // demo's names
    std::string together = FT_SOCK_TOGETHER;   // ft-together's socket
    roomfloor::Floor walked;                   // where they are now, from where they came in: metres to their right and
                                               // behind them, and turned to their left (radians)
    double seconds = 0;                        // end after this long (0: when told to)
    double status = 0;                         // a status line on stdout this often, seconds (0: none)
};

// Runs until a signal, or `seconds`. The program's exit status, with a line on stderr when it isn't 0.
int Run(const Ask &ask);

}  // namespace pretend
