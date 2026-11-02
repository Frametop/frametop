#include "calibfile.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

void fill_calib(const std::vector<CalibCam> &cams, int64_t ring_writer_pid, uint64_t publish_ns, fh_calib_t &out) {
    std::memset(out.producer, 0, sizeof out.producer);
    std::strncpy(out.producer, "ft-hands", sizeof out.producer - 1);
    out.publish_ns = publish_ns;
    out.ring_writer_pid = ring_writer_pid;
    out.ncams = 0;
    std::memset(out.cams, 0, sizeof out.cams);
    for (const CalibCam &c : cams) {
        if (out.ncams == FH_CALIB_MAX_CAMS) break;
        fh_calib_cam_t &o = out.cams[out.ncams++];
        std::strncpy(o.name, c.name.c_str(), sizeof o.name - 1);
        o.ring_cam = c.ring_cam, o.node = c.node;
        o.model = FH_CALIB_KB4;
        o.width = uint32_t(c.cam.width), o.height = uint32_t(c.cam.height);
        o.fx = c.cam.fx, o.fy = c.cam.fy, o.cx = c.cam.cx, o.cy = c.cam.cy;
        for (int i = 0; i < 4; ++i) o.k[i] = c.cam.k[i];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) o.R[i][j] = c.cam.R[i][j];
            o.t[i] = c.cam.origin[i];
        }
    }
}

bool CalibPublisher::open(const std::string &dir, std::string &err) {
    const std::string path = dir + "/calib";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(fh_calib_t)) < 0) {
        err = path + ": " + std::strerror(errno);
        if (fd >= 0) close(fd);
        return false;
    }
    void *m = mmap(nullptr, sizeof(fh_calib_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return err = path + ": can't map it", false;
    use(static_cast<fh_calib_t *>(m));
    return true;
}

void CalibPublisher::use(fh_calib_t *mem) {
    out_ = mem;
    // carry on from the sequence number a previous run left, so a reader that kept its last
    // seq sees this one as new
    seq_ = std::memcmp(out_->magic, FH_CALIB_MAGIC, 8) ? 0 : out_->seq / 2 + 1;
}

void CalibPublisher::write(const std::vector<CalibCam> &cams, int64_t ring_writer_pid, uint64_t publish_ns) {
    __atomic_store_n(&out_->seq, 2 * ++seq_ - 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    std::memcpy(out_->magic, FH_CALIB_MAGIC, 8);
    out_->version = FH_CALIB_VERSION;
    out_->size = sizeof(fh_calib_t);
    fill_calib(cams, ring_writer_pid, publish_ns, *out_);
    __atomic_store_n(&out_->seq, 2 * seq_, __ATOMIC_RELEASE);
}
