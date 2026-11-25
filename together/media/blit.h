// The GPU pass of a shared panel (docs/together.md): a screen's buffer, as KWin drew it and in
// whatever layout the GPU keeps it, drawn into one of the encoder's pictures (pictures.h) at the
// stream's size. An NV12 picture is drawn in two passes, its Y and its Cb,Cr, as BT.709 at full
// range: what the RGBA encoder makes of RGBA, so a viewer converts both alike (ft-peerpanel).
// ft-screens hands the buffers over as DMA-BUFs; each is imported once and drawn from every time
// the screen changes.
//
// One GL context, made current on the thread that calls Open; every call after it is from that
// thread. No encoder and no sockets here.
#pragma once

#include "pictures.h"

#include <cstdint>
#include <map>
#include <string>

namespace media {

// A buffer as its owner describes it (DRM format and modifier, a plane's fd, offset and stride).
struct Dmabuf {
    uint32_t width = 0, height = 0, format = 0;
    uint64_t modifier = 0;
    int planes = 0;
    int fd[4] = {-1, -1, -1, -1};
    uint32_t offset[4] = {}, stride[4] = {};
};

class Blit {
public:
    static constexpr size_t kMostBuffers = 16;   // imported at once: a screen's swapchain has three or four

    Blit() = default;
    ~Blit() { Close(); }
    Blit(const Blit &) = delete;
    Blit &operator=(const Blit &) = delete;

    // False with `err` saying what EGL or GL refused. `pictures` outlives this.
    bool Open(const Pictures &pictures, std::string &err);
    void Close();

    // A buffer to draw from, under the number its owner gave it; one imported under that number
    // before goes. The fds stay the caller's, to close when it likes.
    bool Import(uint32_t id, const Dmabuf &from, std::string &err);
    void Forget(uint32_t id);
    bool Has(uint32_t id) const { return from_.count(id) != 0; }

    // Draws buffer `id` over the whole of picture `into`, scaled to it, and returns once the GPU
    // is done, so the encoder reads a finished picture.
    bool Draw(uint32_t id, int into);

private:
    struct From {
        void *image = nullptr;   // EGLImageKHR
        unsigned texture = 0;
    };
    // One of a picture's planes, as the GPU draws into it: all of an RGBA picture, or an NV12
    // picture's Y or Cb,Cr.
    struct Plane {
        unsigned program = 0;
        int width = 0, height = 0;
        unsigned fbo[Pictures::kCount] = {}, rb[Pictures::kCount] = {};
        void *image[Pictures::kCount] = {};   // EGLImageKHR
    };
    bool OpenPlane(Plane &plane, const Pictures &pictures, const char *fragment, uint32_t format, int shrink, size_t offset, std::string &err);
    void Drop(From &f);

    void *dpy_ = nullptr, *ctx_ = nullptr;   // EGLDisplay, EGLContext
    unsigned quad_ = 0;
    Plane planes_[2];
    int planeCount_ = 0;
    std::map<uint32_t, From> from_;
};

}  // namespace media
