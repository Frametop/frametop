// The body someone has in a shared room when no avatar of theirs is drawn (docs/together.md):
// a head with a visor and two hands, in the colour of their place in the room. It is made here,
// not read from a file, and it is a whole body by bodyfile's rules (every bone a person has, at
// rest in a T pose, 1.7 m tall), so it is posed and drawn the way an imported avatar is. Only
// the head and the hands have a mesh: the arms and legs are solved and not seen.
//
// No GPU, SteamVR or network calls here.
#pragma once

#include "../bodyfile.h"
#include "colour.h"

namespace abstract {

// The body, in the colour of `place`. It is valid (bodyfile::Valid).
bodyfile::Body Body(int place);

}  // namespace abstract
