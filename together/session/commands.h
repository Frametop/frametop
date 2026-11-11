// A session behind lines of text, for ft-together's local socket and for tests: a command in,
// "ok ..." or "error ..." out, and the session's state as JSON.
#pragma once

#include "session.h"

#include <string>

namespace session {

//   state                    -> ok and the session as JSON (State)
//   open [X Z YAW]           -> a room, with you its owner standing there
//   invite                   -> ok and a one-time code for the room
//   join HOST PORT CODE      -> calls a room's owner; `state` tells how it went
//   accept N | decline N     -> an ask (state's "asks") answered
//   away game|off|back       -> tells the room
//   leave                    -> leaves the room; its owner leaving ends it for everyone
//   share K W H X Y Z QW QX QY QZ METRES CURVE [NAME]
//                            -> your panel K (0, 1 or 2) is in the room: its video's pixels, its
//                               middle and its turn in the room (a quaternion; it faces its own
//                               +z), its width in metres, its curve's radius (0: flat), its name.
//                               Said again, it moves the panel.
//   unshare K                -> takes it back
std::string Command(Session &session, const std::string &text, double now);

// {"mode":"idle|hosting|calling|joined","note":TEXT,"me":N,"owner":N,
//  "people":[{"id":N,"name":TEXT,"login":TEXT,"away":N,"linked":BOOL,"spot":[X,Y,Z,YAW]}],
//  "panels":[{"id":N,"owner":N,"number":N,"name":TEXT,"size":[W,H],"at":[X,Y,Z],
//             "turn":[W,X,Y,Z],"metres":N,"curve":N}],
//  "asks":[{"id":N,"name":TEXT,"login":TEXT,"address":TEXT}],"codes":N}
std::string State(const Session &session);

}  // namespace session
