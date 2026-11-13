// fh_shm.hpp, the hands files' reader, on files in a scratch folder (never the live
// /run/user/UID/frametop-hands): no file, the wrong magic, a write in progress, a short file
// zero-filled, a new file at the path, a symlink, and a file too short to use.
#include "../include/fh_shm.hpp"

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) std::printf("FAILED: %s\n", what), ++failures;
}

// A made-up file like fh_hands.h's head: magic, version, size, seq, then data.
struct File {
    char magic[8];
    uint32_t version, size;
    uint64_t seq;
    uint32_t data[4];
};

void Write(const std::string &path, const void *p, size_t n) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, p, n) != ssize_t(n)) std::perror(path.c_str()), std::exit(2);
    close(fd);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/fh-shm-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) return std::perror("mkdtemp"), 2;
    const std::string path = std::string(dir) + "/hands", other = std::string(dir) + "/other";
    constexpr size_t kSeq = offsetof(File, seq);
    fh::ShmFile f("hands", "TESTMAGC", offsetof(File, data), dir, std::chrono::seconds(0));
    File out{};

    Check(!f.Read(&out, sizeof out, kSeq) && f.opens == 0, "no file: nothing");

    File w{{'T', 'E', 'S', 'T', 'M', 'A', 'G', 'C'}, 1, sizeof(File), 4, {1, 2, 3, 4}};
    Write(path, &w, sizeof w);
    Check(f.Read(&out, sizeof out, kSeq) && out.seq == 4 && out.data[3] == 4 && f.opens == 1, "a file: read");
    Check(f.Size() == sizeof w, "its size");
    Check(f.Read(&out, sizeof out, kSeq) && f.opens == 1, "the same file: no new mapping");

    w.seq = 5;   // odd: being written
    Write(other, &w, sizeof w);
    rename(other.c_str(), path.c_str());
    Check(!f.Read(&out, sizeof out, kSeq) && f.opens == 2, "a new file mid-write: mapped again, not read");

    // Shorter than the reader's struct (an older version): zero past its end.
    w.seq = 6;
    Write(other, &w, offsetof(File, data) + 8);
    rename(other.c_str(), path.c_str());
    out.data[2] = out.data[3] = 99;
    Check(f.Read(&out, sizeof out, kSeq) && f.opens == 3 && out.data[1] == 2 && out.data[2] == 0 && out.data[3] == 0,
          "a shorter file: zero-filled past its end");

    File bad = w;
    bad.magic[0] = 'X';
    Write(other, &bad, sizeof bad);
    rename(other.c_str(), path.c_str());
    Check(!f.Read(&out, sizeof out, kSeq), "the wrong magic: nothing");

    Write(other, &w, offsetof(File, data) - 1);
    rename(other.c_str(), path.c_str());
    Check(!f.Read(&out, sizeof out, kSeq), "too short to use: nothing");

    // A symlink to a good file isn't followed.
    const std::string real = std::string(dir) + "/real";
    Write(real, &w, sizeof w);
    unlink(path.c_str());
    if (symlink(real.c_str(), path.c_str()) != 0) return std::perror("symlink"), 2;
    Check(!f.Read(&out, sizeof out, kSeq), "a symlink: not followed");

    unlink(path.c_str());
    Check(!f.Read(&out, sizeof out, kSeq), "the file gone: nothing");

    // its own scratch files only
    unlink(real.c_str());
    rmdir(dir);
    if (failures) return EXIT_FAILURE;
    std::printf("fh_shm: ok\n");
    return 0;
}
