// What ft-avatars needs to draw in the room with a headset on: SteamVR, as a program that shows
// overlays; the GPU, with a context of its own; and the program bodies are drawn with. The
// mirror (live.h) stands on one, and so do the other people in a shared room (../room/room.h).
#pragma once

#include "bodygpu.h"
#include "gpuctx.h"
#include "vroutput.h"

namespace stage {

struct Stage {
    vroutput::Maker maker;   // for the buffers SteamVR shows (sheet.h)
    EGLContext ctx = EGL_NO_CONTEXT;
    bodygpu::Painter painter;

    // False with a line on stderr starting with `who` (kept: a literal) when SteamVR isn't up
    // or the GPU can't. Close it all the same.
    bool Open(const char *who);
    // After everything drawn with it is freed.
    void Close();

private:
    bool vr_ = false;
};

}  // namespace stage
