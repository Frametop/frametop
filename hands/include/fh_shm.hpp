// Reading a file fh-hands rewrites in place under a sequence lock (fh_hands.h, fh_gestures.h): the
// used by FrameHands' overlay renderer and available to external consumers.
// C++, header-only, no OpenVR.
//
// The file is mapped read-only. At most once a `recheck` (a second) it's looked up again: a file
// that's gone, another one at that path (fh-hands made a new one) or one of another size (a later
// version) is mapped again, and `opens` counts the mappings, so a reader can start over. It must
// be the user's own and not a symlink. A read copies the file (read seq, copy, read seq again; the
// copy counts only if both are the same even number, with up to 3 tries) and zero-fills `out`
// past the file's end.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fh {

// The folder fh-hands publishes in: /run/user/UID/framehands.
inline std::string RuntimeDir() { return "/run/user/" + std::to_string(getuid()) + "/framehands"; }

class ShmFile {
public:
    // `name` in `dir`; `magic`: its first 8 bytes; `minSize`: shorter files aren't used.
    ShmFile(const char *name, const char *magic, size_t minSize, std::string dir = RuntimeDir(),
            std::chrono::steady_clock::duration recheck = std::chrono::seconds(1))
        : path_(dir + "/" + name), magic_(magic), min_(minSize), recheck_(recheck) {}
    ~ShmFile() { Close(); }
    ShmFile(const ShmFile &) = delete;
    ShmFile &operator=(const ShmFile &) = delete;

    // Copies up to `size` bytes into out. seqOffset: where the file's 64-bit sequence number is.
    // False with no file, the wrong magic, or a write in progress all 3 times.
    bool Read(void *out, size_t size, size_t seqOffset) {
        const auto now = std::chrono::steady_clock::now();
        if (!checked_ || now - lastCheck_ >= recheck_) {
            checked_ = true, lastCheck_ = now;
            Check();
        }
        if (!map_ || len_ < 8 || std::memcmp(map_, magic_, 8) != 0 || seqOffset + 8 > len_) return false;
        const auto *seqp = reinterpret_cast<const volatile uint64_t *>(static_cast<const char *>(map_) + seqOffset);
        const size_t n = std::min(len_, size);
        for (int tries = 0; tries < 3; ++tries) {
            const uint64_t seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
            if (seq & 1) continue;
            std::memset(out, 0, size);
            std::memcpy(out, map_, n);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(seqp, __ATOMIC_RELAXED) != seq) continue;
            return true;
        }
        return false;
    }
    // The mapped file's length (0 with none).
    size_t Size() const { return len_; }
    int opens = 0;   // mappings so far: a new one means a new file, so counters start over

private:
    void Check() {
        struct stat st;
        if (map_ && stat(path_.c_str(), &st) == 0 && size_t(st.st_size) == len_ && st.st_ino == ino_) return;
        Close();
        const int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return;
        if (fstat(fd, &st) == 0 && st.st_uid == getuid() && size_t(st.st_size) >= min_ && st.st_size > 0) {
            void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) map_ = m, len_ = size_t(st.st_size), ino_ = st.st_ino, ++opens;
        }
        close(fd);
    }
    void Close() {
        if (map_) munmap(map_, len_);
        map_ = nullptr, len_ = 0, ino_ = 0;
    }
    std::string path_;
    const char *magic_;
    size_t min_;
    std::chrono::steady_clock::duration recheck_;
    void *map_ = nullptr;
    size_t len_ = 0;
    ino_t ino_ = 0;
    bool checked_ = false;
    std::chrono::steady_clock::time_point lastCheck_{};
};

}  // namespace fh
