// The pictures a shared panel's encoder reads (irisenc.h, DMA-BUF mode): a few linear GBM buffers
// at the stream's size, RGBA or NV12. Whoever fills them draws on the GPU (blit.h) or maps one and
// writes it (a test picture). Which of them the encoder still reads is its user's to know.
//
// No encoder and no GL here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct gbm_device;
struct gbm_bo;

namespace media {

// How the encoder has its pictures laid out.
struct PictureLayout {
    int width = 0, height = 0;
    bool nv12 = false;     // rows of Y, then rows of Cb,Cr at half the size each way; or else RGBA
    uint32_t stride = 0;   // bytes a row
    size_t size = 0;       // bytes in all, the rows past the picture's last that the encoder reads included
    size_t chroma = 0;     // NV12: where the Cb,Cr rows begin
};

class Pictures {
public:
    static constexpr int kCount = 4;

    Pictures() = default;
    ~Pictures() { Close(); }
    Pictures(const Pictures &) = delete;
    Pictures &operator=(const Pictures &) = delete;

    // Pictures in buffers as long as the layout says. Each buffer is put out of the CPU's caches,
    // so that the encoder reads what is drawn from the first picture on. False with `err` saying
    // what failed, or that a GBM buffer isn't laid out like that.
    bool Open(const PictureLayout &layout, std::string &err);
    void Close();

    const PictureLayout &layout() const { return layout_; }
    int width() const { return layout_.width; }
    int height() const { return layout_.height; }
    // A buffer's bytes in all.
    size_t size() const { return size_; }
    gbm_device *device() const { return gbm_; }
    int fd(int i) const { return fd_[i]; }

    // Picture i's bytes to write (rows of `stride` bytes: R, G, B, A; or Y, and from row
    // layout().chroma / layout().stride on, Cb, Cr), or nullptr. Then Unmap.
    uint8_t *Map(int i, uint32_t &stride);
    void Unmap(int i);

private:
    gbm_device *gbm_ = nullptr;
    int drm_ = -1;
    gbm_bo *bo_[kCount] = {};
    int fd_[kCount] = {-1, -1, -1, -1};
    void *mapped_[kCount] = {};
    PictureLayout layout_;
    uint32_t across_ = 0, rows_ = 0;   // a buffer's, in its own pixels
    size_t size_ = 0;
};

}  // namespace media
