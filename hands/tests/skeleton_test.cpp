// Unit tests for the skeleton fit (track/skeleton.h) on made-up hands: poses within the
// joint limits are posed with the forward kinematics, noise is added, and the fit has to give
// them back. No models, no recordings. make check (in the dev container) builds and runs it.
#include "../track/skeleton.h"

#include <chrono>
#include <cstdio>
#include <random>

using namespace skel;

namespace {

int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures;   \
    } while (0)

double lo_hi(std::mt19937 &rng, double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); }

// A random but plausible pose: angles well inside the limits, the palm somewhere in front.
void random_pose(std::mt19937 &rng, Rot &R, V3 &t, double q[kParams]) {
    R = exp_rot({lo_hi(rng, -1.5, 1.5), lo_hi(rng, -1.5, 1.5), lo_hi(rng, -1.5, 1.5)});
    t = {lo_hi(rng, -0.25, 0.25), lo_hi(rng, -0.4, 0.0), lo_hi(rng, -0.6, -0.25)};
    const double D = M_PI / 180;
    q[0] = lo_hi(rng, -30, 30) * D, q[1] = lo_hi(rng, -30, 30) * D, q[2] = lo_hi(rng, -15, 15) * D;
    q[3] = lo_hi(rng, -10, 10) * D, q[4] = lo_hi(rng, 0, 60) * D, q[5] = lo_hi(rng, 0, 70) * D;
    for (int f = 0; f < 4; ++f) {
        double *a = q + 6 + 5 * f;
        a[0] = lo_hi(rng, -15, 15) * D, a[1] = lo_hi(rng, -10, 80) * D, a[2] = lo_hi(rng, -8, 8) * D;
        a[3] = lo_hi(rng, 0, 90) * D, a[4] = lo_hi(rng, 0, 60) * D;
    }
}

V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

}  // namespace

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0, 1);
    const Shape s = Skeleton::canonical(1.2);

    // rotations
    for (int i = 0; i < 100; ++i) {
        const V3 w{lo_hi(rng, -2, 2), lo_hi(rng, -2, 2), lo_hi(rng, -2, 2)};
        if (norm(w) > 3.1) continue;
        const V3 back = log_rot(exp_rot(w));
        CHECK(norm(back - w) < 1e-9);
        double q[4];
        to_quat(exp_rot(w), q);
        const double a = 2 * std::atan2(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), std::fabs(q[3]));
        CHECK(std::fabs(a - norm(w)) < 1e-9);
    }

    // the frames: right-handed, -Z along the bone, +Y the back of the hand
    {
        Rot R;
        V3 t{0, 0, -0.4}, pts[21];
        Rot fr[21];
        double q[kParams] = {};
        for (int side : {1, -1}) {
            Skeleton::pose(s, side, R, t, q, pts, fr);
            for (int i = 0; i < 21; ++i) {
                CHECK(norm(cross(fr[i].col(1), fr[i].col(2)) - fr[i].col(0)) < 1e-9);
                if (i % 4 != 0 && i < 20 && i != 0) {
                    const int child = i + 1;
                    CHECK(dot(unit(pts[child] - pts[i]), fr[i].col(2) * -1) > 0.999);
                }
            }
            // palm down, fingers forward (-z): the back of the hand is +y; the index knuckle is
            // on the thumb's side: -x on a right hand, +x on a left one
            CHECK(fr[0].col(1)[1] > 0.99);
            CHECK(side * pts[5][0] < 0);
            // flexing a finger moves its tip toward the palm (-y)
            double qb[kParams] = {};
            qb[6 + 3] = 1.0;   // the index PIP
            V3 bent[21];
            Skeleton::pose(s, side, R, t, qb, bent, fr);
            CHECK(bent[8][1] < pts[8][1] - 0.01);
            // abduction moves the index toward the thumb
            double qa[kParams] = {};
            qa[6] = 0.3;
            Skeleton::pose(s, side, R, t, qa, bent, fr);
            CHECK(side * (bent[8][0] - pts[8][0]) < -0.005);
        }
    }

    // fits: noise-free poses come back exactly; noisy ones close to the truth
    double sl[21], sd[21];
    for (int i = 0; i < 21; ++i) sl[i] = 0.002, sd[i] = 0.007;
    for (const double sigma : {0.0, 0.002}) {
        double worst_rms = 0, sum_err = 0, sum_raw = 0, us = 0;
        int n = 0;
        for (int trial = 0; trial < 300; ++trial) {
            const int side = trial % 2 ? 1 : -1;
            Rot R0, R;
            V3 t0, t;
            double q0[kParams] = {}, q[kParams];
            random_pose(rng, R0, t0, q0);
            V3 truth[21], meas[21], fit[21];
            Rot fr[21];
            Skeleton::pose(s, side, R0, t0, q0, truth, fr);
            // noise as stereo has it: 3.5 times as much along the line of sight
            for (int i = 0; i < 21; ++i) {
                const V3 d = unit(truth[i]), e = V3{noise(rng), noise(rng), noise(rng)} * sigma;
                meas[i] = truth[i] + e + d * (2.5 * dot(e, d));
            }
            double rms = 0;
            const auto a = std::chrono::steady_clock::now();
            const double cost = Skeleton::solve(s, side, meas, sl, sd, {0, 0, 0}, nullptr, 4, R, t, q, &rms);
            us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
            CHECK(cost >= 0);
            Skeleton::pose(s, side, R, t, q, fit, fr);
            double err = 0, raw = 0;
            for (int i = 0; i < 21; ++i) err += norm(fit[i] - truth[i]) / 21, raw += norm(meas[i] - truth[i]) / 21;
            worst_rms = std::max(worst_rms, rms);
            sum_err += err, sum_raw += raw, ++n;
        }
        std::printf("noise %.1f mm (x3.5 in depth): fit rms worst %.2f mm; mean error to the truth: fitted %.2f mm, measured %.2f mm; "
                    "%.1f us a fit\n", sigma * 1000, worst_rms * 1000, sum_err / n * 1000, sum_raw / n * 1000, us / n);
        if (sigma == 0) CHECK(worst_rms < 0.002);
        else CHECK(sum_err < sum_raw);   // the bone lengths take noise out
    }

    // calibration: from noisy samples of one hand, the lengths settle on it
    {
        Skeleton sk;
        const Shape truth = Skeleton::canonical(1.3);
        Fit f;
        int64_t ns = 1'000'000'000;
        for (int k = 0; k < 120; ++k, ns += 66'000'000) {
            Rot R0;
            V3 t0, pts[21];
            Rot fr[21];
            double q0[kParams] = {};
            random_pose(rng, R0, t0, q0);
            for (int a = 6; a < kAngles; ++a) q0[a] *= 0.3;   // fairly open, so calibration takes it
            Skeleton::pose(truth, 1, R0, t0, q0, pts, fr);
            for (V3 &p : pts) p = p + V3{noise(rng), noise(rng), noise(rng)} * 0.0015;
            sk.update(f, pts, true, 0.95, 2, 0.0015, nullptr, {0, 0, 0}, 1.0, ns);
            CHECK(f.valid);
        }
        const Shape got = sk.shape(1, 1.0);
        double worst = 0;
        for (int b = 0; b < 20; ++b) worst = std::max(worst, std::fabs(got.len[b] - truth.len[b]));
        std::printf("calibration: settled %.2f, worst bone off %.2f mm\n", sk.calibration(1).settled(), worst * 1000);
        CHECK(sk.calibration(1).settled() >= 1);
        CHECK(worst < 0.002);
        // the left hand borrows the right's lengths until it has its own
        const Shape left = sk.shape(-1, 1.0);
        CHECK(std::fabs(left.len[5] - got.len[5]) < 1e-9);
    }

    // the elbow: behind and below a hand held in front, 2.8 palms away
    {
        const V3 wrist{0.15, -0.25, -0.35}, knuckle{0.15, -0.22, -0.43};
        const V3 e = Skeleton::elbow(wrist, knuckle, 1, 0.23, {0, 1, 0});
        std::printf("elbow for a right hand in front: %+.3f %+.3f %+.3f\n", e[0], e[1], e[2]);
        CHECK(std::fabs(norm(e - wrist) - 0.23) < 1e-6);
        CHECK(e[1] < wrist[1] && e[2] > wrist[2]);
    }

    std::printf(failures ? "%d FAILED\n" : "skeleton tests passed\n", failures);
    return failures ? 1 : 0;
}
