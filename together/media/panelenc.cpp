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
    if (!pictures_.Open(width, height, err)) return false;
    irisenc::Config cfg;
    cfg.width = width, cfg.height = height, cfg.fps = fps, cfg.bitrate = bitrate;
    cfg.hevc = true, cfg.dmabuf = true;
    if (!enc_.Open(cfg, err)) return Close(), false;
    if (uint32_t(enc_.stride()) != pictures_.stride() || pictures_.size() < enc_.frameSize()) {
        err = "the encoder's layout isn't a GBM buffer's at " + std::to_string(width) + "x" + std::to_string(height) + " (stride " +
              std::to_string(enc_.stride()) + " and " + std::to_string(pictures_.stride()) + ", size " + std::to_string(enc_.frameSize()) + " and " +
              std::to_string(pictures_.size()) + ")";
        return Close(), false;
    }
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
