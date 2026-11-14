// Moving shared panels behind lines of text: two questions on ft-together's socket
// (together/README.md, "Moving a panel"; flow/moves.h has the rules).
//
//   move PANEL X Y Z W X Y Z  -> the room's panel PANEL (its id, someone else's) was put there
//                                here: its middle in the room, and its turn as a quaternion.
//                                Its owner is told, when you may move it
//   moves                     -> "ok COUNT NUMBER:PERSON:X:Y:Z:W:X:Y:Z ...": where the others
//                                put your panels since this was last asked. One program asks:
//                                the one that places this Frame's screens (ft-roompanels)
#pragma once

#include "../flow/moves.h"

#include <string>

namespace local {

// Whether `asked` is one of them, and then its answer in `reply`.
bool MoveAsk(flow::Moves &moves, const std::string &asked, double now, std::string &reply);

}  // namespace local
