// The GPU pass of a shared panel (docs/together.md): a screen's buffer, as KWin drew it and in
// whatever layout the GPU keeps it, drawn into one of the encoder's pictures (pictures.h) at the
// stream's size. ft-screens hands the buffers over as DMA-BUFs; each is imported once and drawn
// from every time the screen changes.
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
    void Drop(From &f);

    void *dpy_ = nullptr, *ctx_ = nullptr;   // EGLDisplay, EGLContext
    unsigned program_ = 0, quad_ = 0;
    unsigned fbo_[Pictures::kCount] = {}, rb_[Pictures::kCount] = {};
    void *target_[Pictures::kCount] = {};
    int width_ = 0, height_ = 0;
    std::map<uint32_t, From> from_;
};

}  // namespace media
