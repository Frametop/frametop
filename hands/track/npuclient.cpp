#include "npuclient.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMagic = 0x4450504E, kVersion = 1;   // 'NPPD'

std::string sys_err(const char *what) { return std::string(what) + ": " + std::strerror(errno); }

}  // namespace

bool NpuClient::open(const std::string &dir, size_t out_bytes, uint32_t graph_id, std::string &err) {
    close();
    out_bytes_ = out_bytes;
    const size_t need = kOuts + size_t(kMaxCrops) * out_bytes;
    const std::string shm_path = dir + "/ft-npud.shm", sock_path = dir + "/ft-npud.sock";
    const int fd = ::open(shm_path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return err = sys_err(shm_path.c_str()), false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_uid != getuid() || size_t(st.st_size) < need) {
        ::close(fd);
        return err = shm_path + ": not this user's, or too small for the heads' outputs", false;
    }
    shm_bytes_ = size_t(st.st_size);
    void *m = mmap(nullptr, shm_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return err = sys_err("mmap"), false;
    shm_ = static_cast<uint8_t *>(m);
    uint32_t h[8];
    std::memcpy(h, shm_, sizeof h);
    if (h[0] != kMagic || h[1] != kVersion) return close(), err = "ft-npud's header is not version 1", false;
    if (h[7] != graph_id) return close(), err = "ft-npud serves another graph than the heads file's", false;

    sock_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock_ < 0) return close(), err = sys_err("socket"), false;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof a.sun_path) return close(), err = "socket path too long", false;
    std::memcpy(a.sun_path, sock_path.c_str(), sock_path.size() + 1);
    if (connect(sock_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
        return err = sys_err(sock_path.c_str()), close(), false;
    return true;
}

void NpuClient::close() {
    if (sock_ >= 0) ::close(sock_), sock_ = -1;
    if (shm_) munmap(shm_, shm_bytes_), shm_ = nullptr;
}

bool NpuClient::run(int n, int timeout_ms, std::string &err) {
    if (n < 1 || n > kMaxCrops) return err = "crop count out of range", false;
    return request(uint8_t(n), timeout_ms, err);
}

bool NpuClient::run_det(int n, int timeout_ms, std::string &err) {
    if (n < 1 || n > kMaxCrops) return err = "crop count out of range", false;
    if (!has_detector()) return err = "ft-npud serves no detector", false;
    return request(uint8_t(kDet | n), timeout_ms, err);
}

bool NpuClient::has_detector() const {
    if (!shm_ || shm_bytes_ < det_outs() + size_t(kMaxCrops) * kDetBytes) return false;
    uint32_t features = 0;
    std::memcpy(&features, shm_ + 16, 4);
    return features & kFeatDet;
}

bool NpuClient::request(uint8_t req, int timeout_ms, std::string &err) {
    if (sock_ < 0) return err = "not connected", false;
    if (send(sock_, &req, 1, MSG_NOSIGNAL) != 1) return err = sys_err("ft-npud send"), close(), false;
    pollfd p{sock_, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r == 0) return err = "ft-npud timed out", close(), false;
    uint8_t status = 1;
    if (r < 0 || recv(sock_, &status, 1, 0) != 1) return err = sys_err("ft-npud recv"), close(), false;
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
