// See irisenc.h.
#include "irisenc.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace irisenc {
namespace {

constexpr int kPictures = 4;   // picture slots: one being filled, the rest with the encoder
constexpr int kUnits = 4;      // encoded buffers

int Xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

bool SetCtrl(int fd, uint32_t id, int32_t value) {
    v4l2_control c = {id, value};
    return Xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

}  // namespace

bool Encoder::Fail(const char *what) {
    err_ = std::string(what) + ": " + strerror(errno);
    return false;
}

bool Encoder::Open(const Config &cfg, std::string &err) {
    Close();
    if (!Takes(cfg.width, cfg.height)) {
        err = err_ = "the encoder isn't given a picture of " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + ": " +
                     std::to_string(kWidthStep) + " to 8192 across in steps of " + std::to_string(kWidthStep) + ", 128 to 8192 down and even";
        return false;
    }
    cfg_ = cfg;
    auto fail = [&](const char *what) {
        Fail(what);
        err = err_;
        Close();
        return false;
    };
    fd_ = open(cfg.device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return fail(cfg.device);

    // The encoded side first: the driver sizes the picture side from it.
    v4l2_format cf = {};
    cf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cf.fmt.pix_mp.width = cfg.width;
    cf.fmt.pix_mp.height = cfg.height;
    cf.fmt.pix_mp.pixelformat = cfg.hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    cf.fmt.pix_mp.num_planes = 1;
    cf.fmt.pix_mp.plane_fmt[0].sizeimage = 4 << 20;
    if (Xioctl(fd_, VIDIOC_S_FMT, &cf) < 0) return fail("S_FMT encoded");

    v4l2_format of = {};
    of.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    of.fmt.pix_mp.width = cfg.width;
    of.fmt.pix_mp.height = cfg.height;
    of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_RGBA32;
    of.fmt.pix_mp.num_planes = 1;
    if (Xioctl(fd_, VIDIOC_S_FMT, &of) < 0) return fail("S_FMT picture");
    if (of.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_RGBA32) {
        errno = EINVAL;
        return fail("the encoder doesn't take RGBA");
    }
    stride_ = of.fmt.pix_mp.plane_fmt[0].bytesperline;
    frameSize_ = of.fmt.pix_mp.plane_fmt[0].sizeimage;

    v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = cfg.fps;
    Xioctl(fd_, VIDIOC_S_PARM, &parm);   // a hint: the rate control follows the bitrate anyway

    // Controls the driver lacks are skipped: each only tunes the stream.
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE, cfg.bitrate);
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, cfg.bitrate + cfg.bitrate / 2);
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_GOP_SIZE, cfg.keyframeEvery > 0 ? cfg.keyframeEvery : 0x7fffffff);
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);

    const uint32_t inMemory = cfg.dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    v4l2_requestbuffers orb = {};
    orb.count = kPictures;
    orb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    orb.memory = inMemory;
    if (Xioctl(fd_, VIDIOC_REQBUFS, &orb) < 0) return fail("REQBUFS picture");
    v4l2_requestbuffers crb = {};
    crb.count = kUnits;
    crb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    crb.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_, VIDIOC_REQBUFS, &crb) < 0) return fail("REQBUFS encoded");

    in_.assign(orb.count, Buf());
    out_.assign(crb.count, Buf());
    for (unsigned i = 0; i < orb.count && !cfg.dmabuf; ++i) {
        v4l2_plane pl = {};
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &pl;
        if (Xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return fail("QUERYBUF picture");
        void *p = mmap(nullptr, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, pl.m.mem_offset);
        if (p == MAP_FAILED) return fail("mmap picture");
        in_[i].map = static_cast<uint8_t *>(p);
        in_[i].len = pl.length;
    }
    for (unsigned i = 0; i < crb.count; ++i) {
        v4l2_plane pl = {};
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &pl;
        if (Xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return fail("QUERYBUF encoded");
        void *p = mmap(nullptr, pl.length, PROT_READ, MAP_SHARED, fd_, pl.m.mem_offset);
        if (p == MAP_FAILED) return fail("mmap encoded");
        out_[i].map = static_cast<uint8_t *>(p);
        out_[i].len = pl.length;
        if (Xioctl(fd_, VIDIOC_QBUF, &b) < 0) return fail("QBUF encoded");
        out_[i].queued = true;
    }
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (Xioctl(fd_, VIDIOC_STREAMON, &t) < 0) return fail("STREAMON picture");
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Xioctl(fd_, VIDIOC_STREAMON, &t) < 0) return fail("STREAMON encoded");
    return true;
}

void Encoder::Close() {
    if (fd_ < 0) return;
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &t);
    for (Buf &b : in_)
        if (b.map) munmap(b.map, b.len);
    for (Buf &b : out_)
        if (b.map) munmap(b.map, b.len);
    in_.clear();
    out_.clear();
    returned_.clear();
    held_ = -1;
    close(fd_);
    fd_ = -1;
}

void Encoder::Reap() {
    for (;;) {
        v4l2_plane pl = {};
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = cfg_.dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        b.length = 1;
        b.m.planes = &pl;
        if (Xioctl(fd_, VIDIOC_DQBUF, &b) < 0) return;
        if (b.index < in_.size()) {
            in_[b.index].queued = false;
            if (cfg_.dmabuf) returned_.push_back(static_cast<int>(b.index));
        }
    }
}

uint8_t *Encoder::Map(int &slot) {
    if (fd_ < 0 || cfg_.dmabuf) return nullptr;
    Reap();
    for (size_t i = 0; i < in_.size(); ++i)
        if (!in_[i].queued) {
            slot = static_cast<int>(i);
            return in_[i].map;
        }
    return nullptr;
}

bool Encoder::Queue(int slot, int64_t ptsUs) {
    if (fd_ < 0 || cfg_.dmabuf || slot < 0 || slot >= static_cast<int>(in_.size()) || in_[slot].queued) return false;
    v4l2_plane pl = {};
    pl.bytesused = static_cast<uint32_t>(frameSize_);
    pl.length = static_cast<uint32_t>(in_[slot].len);
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = slot;
    b.length = 1;
    b.m.planes = &pl;
    b.timestamp.tv_sec = ptsUs / 1000000;
    b.timestamp.tv_usec = ptsUs % 1000000;
    if (Xioctl(fd_, VIDIOC_QBUF, &b) < 0) return Fail("QBUF picture");
    in_[slot].queued = true;
    return true;
}

bool Encoder::QueueDmabuf(int fd, size_t size, int64_t ptsUs, int &slot) {
    if (fd_ < 0 || !cfg_.dmabuf) return false;
    Reap();
    slot = -1;
    for (size_t i = 0; i < in_.size(); ++i)
        if (!in_[i].queued) {
            slot = static_cast<int>(i);
            break;
        }
    if (slot < 0) {
        errno = EAGAIN;
        return Fail("no picture slot free");
    }
    v4l2_plane pl = {};
    pl.bytesused = static_cast<uint32_t>(size);
    pl.length = static_cast<uint32_t>(size);
    pl.m.fd = fd;
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = slot;
    b.length = 1;
    b.m.planes = &pl;
    b.timestamp.tv_sec = ptsUs / 1000000;
    b.timestamp.tv_usec = ptsUs % 1000000;
    if (Xioctl(fd_, VIDIOC_QBUF, &b) < 0) return Fail("QBUF picture (dmabuf)");
    in_[slot].queued = true;
    return true;
}

void Encoder::Returned(std::vector<int> &slots) {
    if (fd_ >= 0) Reap();
    slots.swap(returned_);
    returned_.clear();
}

bool Encoder::Dequeue(Unit &out) {
    if (fd_ < 0) return false;
    if (held_ >= 0) {   // the one handed out last time goes back to the encoder
        v4l2_plane pl = {};
        v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = held_;
        b.length = 1;
        b.m.planes = &pl;
        if (Xioctl(fd_, VIDIOC_QBUF, &b) < 0) return Fail("QBUF encoded");
        held_ = -1;
    }
    v4l2_plane pl = {};
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 1;
    b.m.planes = &pl;
    if (Xioctl(fd_, VIDIOC_DQBUF, &b) < 0) return false;
    if (b.index >= out_.size()) return false;
    held_ = static_cast<int>(b.index);
    out.data = out_[b.index].map + pl.data_offset;
    out.size = pl.bytesused;
    out.key = (b.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    out.ptsUs = static_cast<int64_t>(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
    return true;
}

bool Encoder::SetBitrate(int bitsPerSecond) {
    if (fd_ < 0) return false;
    cfg_.bitrate = bitsPerSecond;
    SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, bitsPerSecond + bitsPerSecond / 2);
    return SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE, bitsPerSecond) || Fail("bitrate");
}

bool Encoder::ForceKey() {
    if (fd_ < 0) return false;
    return SetCtrl(fd_, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1) || Fail("force keyframe");
}

}  // namespace irisenc
