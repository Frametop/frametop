// Someone to wear a body with no headset on: where their head and hands are at a moment of a
// named pose (ft-avatars --snapshot, and the Settings pictures). They stand at the room's
// middle on its floor, facing -z, at the body's own height, and their hands are handskin's demo
// hands, so a body is posed from them the way it is from a tracked wearer.
#pragma once

#include "../bodyfile.h"
#include "../pose/bodypose.h"
#include "handfit.h"
#include "wearer.h"

namespace demo {

// The names, with spaces between: stand, wave, reach, type, sit, lean, look.
extern const char *const kNames;

// The wearer `t` seconds into the pose (a pose that moves gets there over its first second),
// with no body in it: someone whose eyes stand `tall` metres up and `ahead` metres in front of
// the middle of them (a body's eyes[2]). False for a name that isn't one.
bool Wearer(const char *name, double t, double tall, double ahead, wearer::Seen &out);

// The wearer of `body`, at its own height, for the pose solver (`dt` is left 0). False for a
// name that isn't one.
bool Pose(const char *name, double t, const bodyfile::Body &body, const handfit::Fit &fit, bodypose::Input &out);

}  // namespace demo
