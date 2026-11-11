// A body drawn as seen through a window in the room (mirror::Window): both eyes' views of it
// through one rectangle, side by side, the left eye's in the left half, clear where no body is.
// SteamVR shows the picture on that rectangle, one half to each eye, so the body has its own
// depth in front of the rectangle and behind it. Your own body below you is drawn this way
// (belowview.h), and so is everyone else in a shared room (../room).
#pragma once

#include "bodygpu.h"
#include "viewmath.h"

namespace windowview {

// Into the framebuffer bound (2 x `w` by `h` pixels, with depth). `clip`: the room -> clip space
// for each eye, with the picture's top in the first row (mirror::Window gives that); `sees`: the
// eye has a view (a half without one is left clear). `light`: toward the light, in the room.
void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[bodyfile::kRoles][12], const viewmath::Mat4 clip[2],
          const bool sees[2], const avatar::Vec &light, int w, int h);

}  // namespace windowview
