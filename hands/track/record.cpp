#include "record.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>

namespace {
constexpr size_t kMaxQueued = 48;   // about 130 MB of sets
}

Recorder::~Recorder() {
    if (!f_) return;
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
    std::fclose(f_);
}

bool Recorder::open(const std::string &dir, std::string &err) {
    if (mkdir(dir.c_str(), 0755) < 0 && errno != EEXIST) return err = dir + ": " + std::strerror(errno), false;
    const std::string path = dir + "/sets.bin";
    f_ = std::fopen(path.c_str(), "wbx");   // never overwrite a recording
    if (!f_) return err = path + ": " + std::strerror(errno), false;
    thread_ = std::thread(&Recorder::loop, this);
    return true;
}

void Recorder::add(const std::vector<SetFrame> &frames) {
    size_t bytes = sizeof(fh_set_hdr_t) + frames.size() * sizeof(fh_set_cam_t);
    for (const SetFrame &s : frames) bytes += size_t(s.width) * s.height;
    std::vector<uint8_t> rec(bytes);
    fh_set_hdr_t h{};
    std::memcpy(h.magic, FH_SET_MAGIC, 8);
    h.ncams = uint32_t(frames.size());
    h.bytes = uint32_t(bytes);
    std::memcpy(rec.data(), &h, sizeof h);
    uint8_t *p = rec.data() + sizeof h;
    for (const SetFrame &s : frames) {
        fh_set_cam_t c{};
        std::strncpy(c.name, s.name.c_str(), sizeof c.name - 1);
        c.width = s.width, c.height = s.height, c.capture_ns = s.capture_ns, c.dqbuf_ns = s.dqbuf_ns;
        std::memcpy(p, &c, sizeof c);
        p += sizeof c;
    }
    for (const SetFrame &s : frames) {
        std::memcpy(p, s.px, size_t(s.width) * s.height);
        p += size_t(s.width) * s.height;
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        if (queue_.size() >= kMaxQueued) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(rec));
        ++added_;
    }
    wake_.notify_one();
}

void Recorder::loop() {
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        wake_.wait(l, [&] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) return;   // stopping, and everything is written
        std::vector<uint8_t> rec = std::move(queue_.front());
        queue_.pop_front();
        l.unlock();
        const bool ok = std::fwrite(rec.data(), 1, rec.size(), f_) == rec.size();
        l.lock();
        ok ? ++written_ : ++dropped_;
    }
}

SetReader::~SetReader() {
    if (f_) std::fclose(f_);
}

bool SetReader::open(const std::string &dir, std::string &err) {
    const std::string path = dir + "/sets.bin";
    dir_ = dir, part_ = 1;
    f_ = std::fopen(path.c_str(), "rb");
    return f_ ? true : (err = path + ": " + std::strerror(errno), false);
}

bool SetReader::next(std::vector<fh_set_cam_t> &cams, std::vector<std::vector<uint8_t>> &pixels) {
    fh_set_hdr_t h;
    if (std::fread(&h, sizeof h, 1, f_) != 1) {   // the next part, if any
        if (!std::feof(f_)) return false;
        FILE *next = std::fopen((dir_ + "/sets-" + std::to_string(part_ + 1) + ".bin").c_str(), "rb");
        if (!next) return false;
        std::fclose(f_);
        f_ = next, ++part_;
        if (std::fread(&h, sizeof h, 1, f_) != 1) return false;
    }
    if (std::memcmp(h.magic, FH_SET_MAGIC, 8) || h.ncams == 0 || h.ncams > 16) return false;
    cams.resize(h.ncams);
    if (std::fread(cams.data(), sizeof(fh_set_cam_t), h.ncams, f_) != h.ncams) return false;
    pixels.resize(h.ncams);
    for (uint32_t i = 0; i < h.ncams; ++i) {
        cams[i].name[sizeof cams[i].name - 1] = 0;
        pixels[i].resize(size_t(cams[i].width) * cams[i].height);
        if (std::fread(pixels[i].data(), 1, pixels[i].size(), f_) != pixels[i].size()) return false;
    }
    return true;
}
