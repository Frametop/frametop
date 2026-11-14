// ft-avatars with a headset on, in a shared room (docs/together.md): the other people, drawn
// where they are, and you, told to them. It asks ft-together who is in the room and watches
// their poses (client.h), lays your floor in the room (floor.h), and sends your own pose, from
// your head and your tracked hands, thirty times a second, and which avatar you have on
// (AVATAR_MODEL in frametop.conf), every other second. Each of the others is posed in their body
// (figure.h): the avatar they have on, once its file came and if it may be drawn (sentbody.h),
// and until then the abstract one in the colour of their place. They are drawn for each eye on a
// sheet that stands where they are and faces you (billboard.h), as an overlay SteamVR shows in
// the room.
//
// Outside a room it draws nothing and waits. Someone whose poses stop isn't drawn: nobody
// stands frozen in the room.
//
// Commands, one datagram each on @ft_avatars_room, answered to the asker:
//   state  -> "ok " and the last status line
//   quit   -> "ok", and it ends
#pragma once

#include "../../conf/ftsock.h"

#include <string>

namespace room {

struct Ask {
    std::string together = FT_SOCK_TOGETHER;   // ft-together's socket
    std::string own = FT_SOCK_AVATARS_ROOM;    // ...and this program's
    int side = 768;       // each eye's picture of a person, pixels a side
    double seconds = 0;   // end after this long (0: when told to)
    double status = 0;    // a status line on stdout this often, seconds (0: none)
};

// Runs until `quit`, SteamVR quitting, a signal, or `seconds`. The program's exit status, with a
// line on stderr when it isn't 0.
int Run(const Ask &ask);

}  // namespace room
