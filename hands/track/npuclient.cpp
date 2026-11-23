#include "npuclient.h"
#include "npuserver.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMagic = 0x4450504E, kVersion = 2;   // 'NPPD'; 2: the memory comes over the socket
constexpr uint8_t kHello = 0, kBusy = 2;

std::string sys_err(const char *what) { return std::string(what) + ": " + std::strerror(errno); }

// Waits for fd to be readable: 1 ready, 0 timed out, -1 failed (errno). A signal doesn't cut the wait short.
int wait_readable(int fd, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now()).count();
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, int(std::max<long long>(left, 0)));
        if (r >= 0 || errno != EINTR) return r;
    }
}

// The daemon's first message: its status byte and, with HELLO, the memory's fd (-1 without one). Closes any
// extra fds a confused sender passed.
bool read_hello(int sock, uint8_t &status, int &fd) {
    fd = -1;
    iovec iov{&status, 1};
    alignas(cmsghdr) char ctl[CMSG_SPACE(4 * sizeof(int))];
    msghdr mh{};
    mh.msg_iov = &iov, mh.msg_iovlen = 1, mh.msg_control = ctl, mh.msg_controllen = sizeof ctl;
    const ssize_t got = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    for (cmsghdr *c = got >= 0 ? CMSG_FIRSTHDR(&mh) : nullptr; c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n; ++i) {
            int f;
            std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
            if (fd < 0) fd = f;
            else ::close(f);
        }
    }
    return got == 1 && !(mh.msg_flags & MSG_CTRUNC);
}

}  // namespace

bool NpuClient::open(const std::string &dir, size_t out_bytes, uint32_t graph_id, std::string &err) {
    close();
    if (out_bytes > kOutSlot) return err = "the heads' outputs are larger than ft-npud's output slots", false;
    out_bytes_ = out_bytes;
    // Connect through the resolved directory, the one the peer check below walks: a symlink on the way can't
    // be swapped between the check and the connect.
    char real[PATH_MAX];
    if (!::realpath(dir.c_str(), real)) return err = sys_err(dir.c_str()), false;
    const std::string real_dir = real, sock_path = real_dir + "/ft-npud.sock";
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof a.sun_path) return err = "socket path too long", false;
    std::memcpy(a.sun_path, sock_path.c_str(), sock_path.size() + 1);
    // Non-blocking: a full backlog (the daemon stuck) fails at once instead of stalling the tracker.
    sock_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock_ < 0) return err = sys_err("socket"), false;
    if (connect(sock_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
        return err = errno == EAGAIN ? sock_path + ": ft-npud is not accepting connections"
                                     : sys_err(sock_path.c_str()),
               close(), false;
    // The daemon runs as root (or, in tests, as this user); in the container root shows as the overflow uid,
    // taken only at a path no one else can write (npuserver.h). Anyone else at that path is not ft-npud.
    ucred peer{};
    socklen_t len = sizeof peer;
    std::string why = "no peer credentials";
    if (getsockopt(sock_, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0 ||
        !npu_server_trusted(peer.uid, getuid(), npu_overflow_uid(), real_dir, why))
        return close(), err = sock_path + ": " + why, false;
    const int r = wait_readable(sock_, kHelloMs);
    if (r <= 0) return err = r == 0 ? "ft-npud did not answer the connection" : sys_err("ft-npud poll"), close(), false;
    uint8_t status = 0xff;
    int fd = -1;
    const bool whole = read_hello(sock_, status, fd);
    if (!whole || status != kHello || fd < 0) {
        if (fd >= 0) ::close(fd);
        close();
        return err = !whole && status == 0xff ? "ft-npud closed the connection (another user?)"
                   : status == kBusy        ? "ft-npud serves another client"
                                            : "ft-npud's hello carried no memory",
               false;
    }
    struct stat st{};
    const int seals = fcntl(fd, F_GET_SEALS);
    if (fstat(fd, &st) != 0 || size_t(st.st_size) < kShmBytes || seals < 0 || !(seals & F_SEAL_SHRINK)) {
        ::close(fd), close();
        return err = "ft-npud's memory is too small or not sealed", false;
    }
    shm_bytes_ = size_t(st.st_size);
    void *m = mmap(nullptr, shm_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return err = sys_err("mmap"), close(), false;
    shm_ = static_cast<uint8_t *>(m);
    uint32_t h[8];
    std::memcpy(h, shm_, sizeof h);
    if (h[0] != kMagic || h[1] != kVersion) return close(), err = "ft-npud speaks another protocol version", false;
    if (h[7] != graph_id) return close(), err = "ft-npud serves another graph than the heads file's", false;
    return true;
}

void NpuClient::close() {
    if (sock_ >= 0) ::close(sock_), sock_ = -1;
    in_flight_ = false;
    if (shm_) munmap(shm_, shm_bytes_), shm_ = nullptr;
}

bool NpuClient::run(int n, int timeout_ms, std::string &err) {
    if (n < 1 || n > kMaxCrops) return err = "crop count out of range", false;
    return request(uint8_t(n), timeout_ms, err);
}

bool NpuClient::has_detector() const {
    if (!shm_) return false;
    uint32_t features = 0;
    std::memcpy(&features, shm_ + 16, 4);
    return features & kFeatDet;
}

bool NpuClient::post_det(int n, std::string &err) {
    if (n < 1 || n > kMaxCrops) return err = "crop count out of range", false;
    if (!has_detector()) return err = "ft-npud serves no detector", false;
    return post(uint8_t(kDet | n), err);
}

bool NpuClient::request(uint8_t req, int timeout_ms, std::string &err) {
    return settle(timeout_ms, err) && post(req, err) && settle(timeout_ms, err);
}

bool NpuClient::post(uint8_t req, std::string &err) {
    if (sock_ < 0) return err = "not connected", false;
    if (in_flight_) return err = "a request is still in flight", close(), false;
    if (send(sock_, &req, 1, MSG_NOSIGNAL) != 1) return err = sys_err("ft-npud send"), close(), false;
    in_flight_ = true;
    return true;
}

bool NpuClient::settle(int timeout_ms, std::string &err) {
    if (!in_flight_) return true;
    in_flight_ = false;
    const int r = wait_readable(sock_, timeout_ms);
    if (r == 0) return err = "ft-npud timed out", close(), false;
    if (r < 0) return err = sys_err("ft-npud poll"), close(), false;
    uint8_t status = 1;
    const ssize_t got = recv(sock_, &status, 1, 0);
    if (got == 0) return err = "ft-npud closed the connection", close(), false;
    if (got != 1) return err = sys_err("ft-npud recv"), close(), false;
    if (status != 0) return err = "ft-npud refused the request", close(), false;
    return true;
}

float NpuClient::header_float(int word) const {
    float f = 0;
    if (shm_) std::memcpy(&f, shm_ + 4 * word, 4);
    return f;
}

float NpuClient::wrapper_ms() const { return header_float(8); }

float NpuClient::daemon_ms() const { return header_float(9); }

float NpuClient::det_scale(int which) const { return header_float(10 + which); }
