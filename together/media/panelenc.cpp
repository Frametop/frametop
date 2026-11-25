#include "panelenc.h"

#include <time.h>

namespace media {

namespace {

int64_t NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

}  // namespace

bool PanelEncoder::Open(int width, int height, int fps, int bitrate, std::string &err) {
    Close();
    irisenc::Config cfg;
    cfg.width = width, cfg.height = height, cfg.fps = fps, cfg.bitrate = bitrate;
    cfg.hevc = true, cfg.dmabuf = true;
    if (!enc_.Open(cfg, err)) return Close(), false;
    // The encoder reads rows past the picture's last, to its own step down (1080 rows as 1088):
    // its frameSize has them, so the buffers are made that long.
    PictureLayout layout;
    layout.width = width, layout.height = height, layout.nv12 = enc_.format() == irisenc::Format::kNv12;
    layout.stride = uint32_t(enc_.stride()), layout.size = enc_.frameSize(), layout.chroma = enc_.chroma();
    if (!pictures_.Open(layout, err)) return Close(), false;
    bitrate_ = bitrate;
    return true;
}

void PanelEncoder::Close() {
    enc_.Close();
    pictures_.Close();
    for (int &s : slot_) s = -1;
    last_ = -1, bitrate_ = 0;
}

void PanelEncoder::TakeBack() {
    enc_.Returned(back_);
    for (const int slot : back_)
        for (int &s : slot_)
            if (s == slot) s = -1;
}

int PanelEncoder::Free() {
    if (!open()) return -1;
    TakeBack();
    for (int i = 0; i < Pictures::kCount; ++i)
        if (slot_[i] < 0 && i != last_) return i;
    return -1;
}

bool PanelEncoder::Queue(int i, bool key) {
    if (!open() || i < 0 || i >= Pictures::kCount || slot_[i] >= 0) return false;
    if (key && !enc_.ForceKey()) return false;
    int slot = -1;
    if (!enc_.QueueDmabuf(pictures_.fd(i), pictures_.size(), NowUs(), slot)) return false;
    slot_[i] = slot, last_ = i;
    return true;
}

bool PanelEncoder::Again(bool key) {
    if (last_ < 0) return false;
    TakeBack();
    return Queue(last_, key);
}

bool PanelEncoder::Next(irisenc::Unit &u) { return open() && enc_.Dequeue(u); }

void PanelEncoder::Bitrate(int bitsPerSecond) {
    if (!open() || bitsPerSecond == bitrate_) return;
    if (enc_.SetBitrate(bitsPerSecond)) bitrate_ = bitsPerSecond;
}

}  // namespace media
