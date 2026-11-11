// Control behind lines of text: the questions on ft-together's socket that begin with
// `control` (together/README.md, "Control").
//
//   control                      -> ok and flow::Control::State as JSON
//   control ask PANEL            -> asks the owner of the room's panel PANEL (its id) for it
//   control give NUMBER PERSON   -> gives your panel NUMBER to the person in place PERSON
//   control take NUMBER [PERSON] -> takes it back from them or says no to their ask; from
//                                   everyone without PERSON
//   control act PANEL move X Y | button B 0|1 | scroll RIGHT DOWN | key CODE 0|1
//                                -> your input on a panel you were given (proto/control.h)
//   control own                  -> your own hand is on your desktop: nobody else's input is
//                                   taken for the next moment
//   control here                 -> you are there, says the program that knows (ft-roomaway,
//                                   while the headset is on your head): what you gave stands
//                                   for a few seconds more
#pragma once

#include "../flow/control.h"

#include <string>

namespace local {

// Whether `asked` is one of them, and then its answer in `reply`.
bool ControlAsk(flow::Control &control, const std::string &asked, double now, std::string &reply);

}  // namespace local
