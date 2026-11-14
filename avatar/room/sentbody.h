// The body of an avatar someone in the room sent (docs/together.md), from its file as
// ft-together kept it (together/README.md, "The avatars people have on").
//
// The file is a stranger's. ft-together checked that it is the file the room named, and nothing
// of what is in it. So it is read here by the model file's strict readers (bodyfile.h) and by
// nothing else, and it is drawn only if it is a whole avatar that says its owner lets peers
// have it: a file without that mark wasn't theirs to send. One that fails is no avatar, and its
// wearer is drawn in the abstract body.
//
// No GPU, SteamVR or network calls here.
#pragma once

#include "../bodyfile.h"

#include <string>

namespace sentbody {

// False, with why, for a file that isn't drawn.
bool Read(const std::string &file, bodyfile::Body &body, std::string &why);

}  // namespace sentbody
