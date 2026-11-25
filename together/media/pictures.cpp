#include "pictures.h"

#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>

namespace media {

namespace {

// Puts a new buffer's bytes out of the CPU's caches. The kernel noughts a buffer's pages through
// a cached view of its own, and those lines of noughts stay in the caches after our pictures
// have gone to the memory under them (we write uncached). The Frame's encoder reads through the
// caches: it found most of each buffer's first picture black, and the rest of it only where a
// line had happened to fall out. Done once, before anything is drawn, nothing stale is left.
bool Uncache(int fd, size_t size) {
#if defined(__aarch64__)
    void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) return false;
    unsigned long ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = size_t(4) << ((ctr >> 16) & 0xf);   // the smallest data cache line, in bytes
    const char *at = static_cast<const char *>(map);
    for (size_t o = 0; o < size; o += line) asm volatile("dc civac, %0" : : "r"(at + o) : "memory");
    asm volatile("dsb ish" : : : "memory");
    munmap(map, size);
#else
    (void)fd, (void)size;
#endif
    return true;
}

}  // namespace

bool Pictures::Open(const PictureLayout &layout, std::string &err) {
    Close();
    const size_t rows = layout.stride ? (layout.size + layout.stride - 1) / layout.stride : 0;
    if (layout.width < 1 || layout.height < 1 || rows < size_t(layout.height) || rows > 3 * (8192 + 64) / 2)
        return err = "the encoder's layout makes no sense", false;
    layout_ = layout;
    // NV12 is bytes to GBM: rows of Y and rows of Cb,Cr alike are `stride` of them.
    across_ = layout.nv12 ? layout.stride : uint32_t(layout.width), rows_ = uint32_t(rows);
    const uint32_t format = layout.nv12 ? GBM_FORMAT_R8 : GBM_FORMAT_ABGR8888;
    drm_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    gbm_ = drm_ >= 0 ? gbm_create_device(drm_) : nullptr;
    if (!gbm_) return err = "no GBM device (/dev/dri/renderD128)", Close(), false;
    for (int i = 0; i < kCount; ++i) {
        bo_[i] = gbm_bo_create(gbm_, across_, rows_, format, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        fd_[i] = bo_[i] ? gbm_bo_get_fd(bo_[i]) : -1;
        if (fd_[i] < 0) return err = "GBM made no " + std::to_string(across_) + "x" + std::to_string(rows_) + " buffer", Close(), false;
    }
    const uint32_t stride = gbm_bo_get_stride(bo_[0]);
    const off_t end = lseek(fd_[0], 0, SEEK_END);
    size_ = end > 0 ? size_t(end) : 0;
    if (stride != layout.stride || size_ < layout.size) {
        err = "the encoder's layout isn't a GBM buffer's at " + std::to_string(layout.width) + "x" + std::to_string(layout.height) + " (stride " +
              std::to_string(layout.stride) + " and " + std::to_string(stride) + ", size " + std::to_string(layout.size) + " and " +
              std::to_string(size_) + ")";
        return Close(), false;
    }
    for (int i = 0; i < kCount; ++i)
        if (!size_ || !Uncache(fd_[i], size_)) return err = "a picture buffer couldn't be mapped", Close(), false;
    return true;
}

void Pictures::Close() {
    for (int i = 0; i < kCount; ++i) {
        Unmap(i);
        if (fd_[i] >= 0) close(fd_[i]);
        if (bo_[i]) gbm_bo_destroy(bo_[i]);
        fd_[i] = -1, bo_[i] = nullptr;
    }
    if (gbm_) gbm_device_destroy(gbm_);
    if (drm_ >= 0) close(drm_);
    gbm_ = nullptr, drm_ = -1, layout_ = PictureLayout(), across_ = rows_ = 0, size_ = 0;
}

uint8_t *Pictures::Map(int i, uint32_t &stride) {
    if (i < 0 || i >= kCount || !bo_[i] || mapped_[i]) return nullptr;
    return static_cast<uint8_t *>(gbm_bo_map(bo_[i], 0, 0, across_, rows_, GBM_BO_TRANSFER_WRITE, &stride, &mapped_[i]));
}

void Pictures::Unmap(int i) {
    if (i < 0 || i >= kCount || !mapped_[i]) return;
    gbm_bo_unmap(bo_[i], mapped_[i]);
    mapped_[i] = nullptr;
}

}  // namespace media
