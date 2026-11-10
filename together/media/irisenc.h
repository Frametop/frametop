// The Frame's hardware encoder (Qualcomm iris, /dev/video-enc0) through the V4L2 stateful API, for
// a shared panel's video (docs/together.md). RGBA pictures go in, HEVC access units come out, each
// decodable as soon as it arrives: no B-frames, parameter sets with every keyframe. The bitrate
// can change while it runs, and a keyframe can be asked for.
//
// Pictures go in one of two ways: copied into the encoder's own buffers (Map, Queue), or as
// DMA-BUFs the caller owns (QueueDmabuf), which costs no copy when the buffer's layout is the
// encoder's (stride(), frameSize()).
//
// No GPU, SteamVR or network calls here. One thread per Encoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace irisenc {

struct Config {
    int width = 0, height = 0;   // even, 128 to 8192
    int fps = 30;
    int bitrate = 3000000;       // bits a second
    int keyframeEvery = 0;       // frames between keyframes; 0: only when asked (ForceKey)
    bool hevc = true;            // false: H.264
    bool dmabuf = false;         // pictures come as DMA-BUFs (QueueDmabuf), not copies
    const char *device = "/dev/video-enc0";
};

// One encoded picture. `data` stays valid until the next Dequeue or Close.
struct Unit {
    const uint8_t *data = nullptr;
    size_t size = 0;
    bool key = false;
    int64_t ptsUs = 0;   // the picture's, as queued
};

class Encoder {
  public:
    Encoder() = default;
    ~Encoder() { Close(); }
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    // False with `err` saying what the driver refused.
    bool Open(const Config &cfg, std::string &err);
    void Close();
    bool isOpen() const { return fd_ >= 0; }

    // Readable (POLLIN) when an encoded picture waits, writable (POLLOUT) when a picture slot is
    // free again.
    int fd() const { return fd_; }
    // A picture's layout in the encoder's buffers: bytes a row, and bytes in all.
    int stride() const { return stride_; }
    size_t frameSize() const { return frameSize_; }

    // Copy mode: a free picture buffer to fill (RGBA, stride() bytes a row), or nullptr when
    // every one is with the encoder. Then Queue it.
    uint8_t *Map(int &slot);
    bool Queue(int slot, int64_t ptsUs);
    // DMA-BUF mode: the picture in `fd` (it must stay unchanged until the encoder gives the
    // slot back: Returned). False when no slot is free or the driver refuses the buffer.
    bool QueueDmabuf(int fd, size_t size, int64_t ptsUs, int &slot);
    // DMA-BUF mode: the slots whose pictures the encoder has finished reading since the last
    // call, so their buffers may change again.
    void Returned(std::vector<int> &slots);

    // The next encoded picture, if one waits.
    bool Dequeue(Unit &out);

    bool SetBitrate(int bitsPerSecond);
    bool ForceKey();

    const std::string &lastError() const { return err_; }

  private:
    struct Buf {
        uint8_t *map = nullptr;
        size_t len = 0;
        bool queued = false;
    };
    bool Fail(const char *what);
    void Reap();   // takes back picture slots the encoder is done with

    int fd_ = -1;
    Config cfg_;
    int stride_ = 0;
    size_t frameSize_ = 0;
    std::vector<Buf> in_, out_;
    std::vector<int> returned_;
    int held_ = -1;   // the encoded buffer handed out by Dequeue, requeued on the next call
    std::string err_;
};

}  // namespace irisenc
