// The pictures a shared panel's encoder reads (irisenc.h, DMA-BUF mode): a few linear ABGR8888
// GBM buffers at the stream's size. Whoever fills them draws on the GPU (blit.h) or maps one and
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

class Pictures {
public:
    static constexpr int kCount = 4;

    Pictures() = default;
    ~Pictures() { Close(); }
    Pictures(const Pictures &) = delete;
    Pictures &operator=(const Pictures &) = delete;

    // Pictures of width x height in buffers `rows` rows long (height, when that is more): the
    // encoder reads whole rows past a picture's last, up to its own step (irisenc.h, frameSize),
    // and they are the buffer's to have. False with `err` saying what GBM refused.
    bool Open(int width, int height, int rows, std::string &err);
    void Close();

    int width() const { return width_; }
    int height() const { return height_; }
    // A picture's layout: bytes a row, and bytes in all. The encoder takes a buffer with no copy
    // when these are its own.
    uint32_t stride() const { return stride_; }
    size_t size() const { return size_; }
    gbm_device *device() const { return gbm_; }
    int fd(int i) const { return fd_[i]; }

    // Picture i's pixels to write (rows of `stride` bytes, R, G, B, A), or nullptr. Then Unmap.
    uint8_t *Map(int i, uint32_t &stride);
    void Unmap(int i);

private:
    gbm_device *gbm_ = nullptr;
    int drm_ = -1;
    gbm_bo *bo_[kCount] = {};
    int fd_[kCount] = {-1, -1, -1, -1};
    void *mapped_[kCount] = {};
    int width_ = 0, height_ = 0;
    uint32_t stride_ = 0;
    size_t size_ = 0;
};

}  // namespace media
