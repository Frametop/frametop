// An offscreen picture: a GPU context with no screen, and a buffer to draw into and read back
// (colour and depth). ft-avatars makes its pictures of a body with one (picture.h), and of
// someone in a shared room (../room/roompicture.h). Drawn kFine times the size each way and
// averaged down on the way out, so edges come out smooth.
#pragma once

#include "gpuctx.h"

#include <string>
#include <vector>

namespace canvas {

constexpr int kFine = 2;   // drawn this many times the size each way, then averaged down

struct Canvas {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    bool node = false;
    GLuint tex = 0, depth = 0, fbo = 0;
    int w = 0, h = 0;

    // `width` by `height` pixels as drawn (kFine times the picture's), bound and cleared. False
    // with a line on stderr when the GPU can't.
    bool Open(int width, int height);
    // What was drawn, averaged down by kFine: colour times alpha, and alpha, the top row first.
    // `topFirst`: it was drawn with its top in the buffer's first row (as SteamVR reads one), not GL's way up.
    std::vector<float> Read(bool topFirst = false);
    void Close();
};

// What Read gave (`w` by `h` pixels) as a PNG that keeps its alpha: the colour as it is, not
// times alpha. False when the file can't be written.
bool Save(const std::string &path, const std::vector<float> &px, int w, int h);

}  // namespace canvas
