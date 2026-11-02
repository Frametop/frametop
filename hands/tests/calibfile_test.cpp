// The calibration file (include/fh_calib.h, track/calibfile.cpp) in a scratch folder (never the
// live /run/user/UID/frametop-hands): what each field holds, read back the way another program
// would (fh_shm.hpp, and the projection include/fh_calib.md describes), the sequence number
// carrying on across a restart, and the cap at FH_CALIB_MAX_CAMS cameras.
#include "../include/fh_shm.hpp"
#include "../track/calibfile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) std::printf("FAILED: %s\n", what), ++failures;
}
bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// A camera turned 90 degrees about the head's +y (its +z is the head's -x, its +x the head's
// +z) and 5 cm to the left, so a transposed R or a sign slip shows in the projection.
Camera Turned() {
    Camera c;
    c.name = "slam_left";
    c.width = 640, c.height = 480;
    c.fx = 200, c.fy = 210, c.cx = 320.5, c.cy = 240.25;
    c.k[0] = 0.1, c.k[1] = -0.02, c.k[2] = 0.003, c.k[3] = -0.0004;
    const double cols[3][3] = {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};   // the camera's x, y, z in the head frame
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) c.R[i][j] = cols[j][i];
    c.origin = {-0.05, 0, 0};
    return c;
}

// A head-frame point to pixels, from the file's fields alone, as include/fh_calib.md says:
// c = R^T (p - t), theta from the axis, theta_d by KB4, then fx, fy, cx, cy.
void Project(const fh_calib_cam_t &o, const double p[3], double uv[2]) {
    double d[3], c[3];
    for (int i = 0; i < 3; ++i) d[i] = p[i] - o.t[i];
    for (int j = 0; j < 3; ++j) c[j] = o.R[0][j] * d[0] + o.R[1][j] * d[1] + o.R[2][j] * d[2];
    const double r = std::hypot(c[0], c[1]), th = std::atan2(r, c[2]), t2 = th * th;
    const double td = th * (1 + o.k[0] * t2 + o.k[1] * t2 * t2 + o.k[2] * t2 * t2 * t2 + o.k[3] * t2 * t2 * t2 * t2);
    const double s = r > 1e-12 ? td / r : 0;
    uv[0] = o.fx * c[0] * s + o.cx, uv[1] = o.fy * c[1] * s + o.cy;
}

}  // namespace

int main() {
    // what each field holds
    std::vector<CalibCam> cams = {{"slam_left", 3, 11, Turned()}, {"color_video2", 5, 2, Turned()}};
    cams[1].cam.width = 493, cams[1].cam.height = 370;
    static fh_calib_t f;
    CalibPublisher mem;
    mem.use(&f);
    mem.write(cams, 4242, 1'000);
    Check(!std::memcmp(f.magic, FH_CALIB_MAGIC, 8) && f.version == FH_CALIB_VERSION && f.size == sizeof f, "the header");
    Check(f.seq == 2, "seq even after a write");
    Check(!std::strcmp(f.producer, "ft-hands") && f.ring_writer_pid == 4242 && f.publish_ns == 1'000, "producer, pid, time");
    Check(f.ncams == 2, "two cameras");
    const fh_calib_cam_t &a = f.cams[0];
    Check(!std::strcmp(a.name, "slam_left") && a.ring_cam == 3 && a.node == 11 && a.model == FH_CALIB_KB4, "camera 0's names");
    Check(a.width == 640 && a.height == 480 && a.fx == 200 && a.fy == 210 && a.cx == 320.5 && a.cy == 240.25, "intrinsics");
    Check(a.k[0] == 0.1 && a.k[3] == -0.0004 && a.k[4] == 0 && a.k[7] == 0, "distortion: k1..k4, the rest zero");
    Check(f.cams[1].width == 493 && !std::strcmp(f.cams[1].name, "color_video2") && f.cams[1].ring_cam == 5, "camera 1");
    Check(f.cams[2].name[0] == 0 && f.cams[2].ring_cam == 0, "unused slots zero");

    // the projection a reader does with them: a point straight ahead of the camera (head -x)
    // lands on the principal point; one 0.3 rad toward the camera's +x (head +z) lands on +u by
    // fx * theta_d, worked out here by hand from KB4
    const double ahead[3] = {-1.05, 0, 0};
    double uv[2];
    Project(a, ahead, uv);
    Check(Near(uv[0], 320.5) && Near(uv[1], 240.25), "on the axis: the principal point");
    const double th = 0.3, t2 = th * th;
    const double td = th * (1 + 0.1 * t2 - 0.02 * t2 * t2 + 0.003 * t2 * t2 * t2 - 0.0004 * t2 * t2 * t2 * t2);
    const double side[3] = {-0.05 - std::cos(th), 0, std::sin(th)};
    Project(a, side, uv);
    Check(Near(uv[0], 320.5 + 200 * td) && Near(uv[1], 240.25), "0.3 rad off the axis toward the camera's +x");

    // through a file, read as another program would, and a restart carrying the seq on
    char tmpl[] = "/tmp/fh-calib-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) return std::perror("mkdtemp"), 2;
    std::string err;
    uint64_t first_seq = 0;
    {
        CalibPublisher pub;
        Check(pub.open(dir, err), "open");
        pub.write(cams, 77, 5);
        pub.write(cams, 77, 6);
        fh::ShmFile r("calib", FH_CALIB_MAGIC, sizeof(fh_calib_t), dir);
        static fh_calib_t got;
        Check(r.Read(&got, sizeof got, offsetof(fh_calib_t, seq)), "read back");
        Check(got.ncams == 2 && got.publish_ns == 6 && got.ring_writer_pid == 77 && got.cams[0].R[2][0] == 1 && got.cams[0].R[0][2] == -1, "read back's fields");
        first_seq = got.seq;
    }
    {
        CalibPublisher pub;
        Check(pub.open(dir, err), "open again");
        pub.write(cams, 78, 7);
        fh::ShmFile r("calib", FH_CALIB_MAGIC, sizeof(fh_calib_t), dir);
        static fh_calib_t got;
        Check(r.Read(&got, sizeof got, offsetof(fh_calib_t, seq)), "read back after a restart");
        Check(got.seq > first_seq && got.seq % 2 == 0, "seq carries on across a restart");
        Check(got.ring_writer_pid == 78, "the new ring's pid");
    }
    unlink((std::string(dir) + "/calib").c_str());
    rmdir(dir);

    // more cameras than fit
    std::vector<CalibCam> many(FH_CALIB_MAX_CAMS + 3, {"x", 0, 0, Turned()});
    mem.write(many, 1, 1);
    Check(f.ncams == FH_CALIB_MAX_CAMS && f.seq == 4, "capped at FH_CALIB_MAX_CAMS");

    if (failures) return std::printf("%d failed\n", failures), 1;
    std::printf("calibfile: all passed\n");
    return 0;
}
