// Where the wearer looks, behind lines of text: the questions on ft-together's socket that
// begin with `gaze` (together/README.md, "Where you look"; gaze.h has the rules).
//
//   gaze                    -> ok and JSON: {"shared":true|false,"fresh":true|false,"quiet":true|false}.
//                              `shared`: the wearer shares where they look with this room;
//                              `fresh`: a direction was told lately, and goes out in their poses;
//                              `quiet`: their pointer isn't told while it only moves (hush.h)
//   gaze on | gaze off      -> the wearer shares where they look, in a room and for that room,
//                              or stops
//   gaze say YAW PITCH      -> where they look now: degrees to their left and up from the way
//                              the head faces, smoothed, from the program that knows
//                              (ft-roomgaze). Taken only while it is shared
//   gaze pointer eyes|hand  -> whether the wearer's eyes move their pointer (gaze mode), from
//                              the same program, every second
#pragma once

#include "gaze.h"

#include <string>

namespace local {

// Whether `asked` is one of them, and then its answer in `reply`. `inRoom`: the wearer is in a
// room.
bool GazeAsk(Gaze &gaze, const std::string &asked, bool inRoom, double now, std::string &reply);

}  // namespace local
