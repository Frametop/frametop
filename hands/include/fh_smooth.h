// fh_smooth - smoothing for what a hand moves: One Euro filters (Casiez, Roussel and Vogel, CHI
// 2012) run in continuous time, so a hand tracked at 15-30 Hz moves things smoothly at the
// display's rate instead of in steps. Shared by the pointer helper (the hand ray, through
// hands/include/fh_ray.h) and ft-screens (near grabs, screens/handinput.cpp).
// Design: frame-hands/notes/hand-interaction.md, section 13.
//
// The One Euro filter is a first-order low-pass whose cutoff rises with speed: minCutoff (Hz)
// still, plus beta per unit of speed (the speed itself low-passed at dCutoff). Slow and still:
// heavy smoothing, so jitter goes; fast: little, so it keeps up. Here it runs in continuous
// time: each tracker sample sets the cutoff and where the input goes, and At(now) is the
// low-pass's output at now, however often the caller asks. The input slides from the sample
// before to the newest over one frame interval (the capture times' gap, at most 0.1 s), so a
// steady move comes out as a steady move: held still, the input made the output catch up and
// stall each frame, steps again at a high cutoff. The slide costs half a frame of delay
// (17 ms at 30 Hz) over holding the sample. The output never passes the newest sample (no
// overshoot); the classic discrete filter, read at the display's rate, holds still and then
// jumps a whole step each sample. No prediction past the newest sample: the ray and a carried
// panel stay behind the hand by the tracking latency plus the filter's lag, never ahead of it.
//
// Point<N>: N values (1 for a distance, 3 for a point); the speed is the length of the
// change, so it smooths the same along every axis. Direction: a unit vector, filtered on the
// sphere (it turns along the great circle toward the newest sample; speed in degrees per
// second), with no angle to wrap and no trouble pointing straight up.
//
// Times are seconds on one steady clock. Each sample has its capture time (for the speed) and
// the time it arrived (now: where the output's timeline stands). Header-only C++17, no
// dependencies.
#pragma once

#include <algorithm>
#include <cmath>

namespace fhsmooth {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a) {
    const double l = Len(a);
    return l > 1e-12 ? a * (1 / l) : V3{0, 0, -1};
}
// The angle between two unit vectors, radians (accurate near 0, unlike acos).
inline double Angle(V3 a, V3 b) { return std::atan2(Len(Cross(a, b)), Dot(a, b)); }
// a turned toward b (unit vectors) by the fraction k of the angle between them.
inline V3 Slerp(V3 a, V3 b, double k) {
    const double w = Angle(a, b);
    if (w < 1e-9) return b;
    if (w > M_PI - 1e-6) return k < 0.5 ? a : b;  // opposite: no one way round
    const double s = std::sin(w);
    return Norm(a * (std::sin((1 - k) * w) / s) + b * (std::sin(k * w) / s));
}

struct Params {
    double minCutoff = 1;  // Hz, still
    double beta = 0;       // Hz more per unit of speed (per m/s for points, per deg/s for directions)
    double dCutoff = 1;    // Hz: the speed's own low-pass
};

namespace detail {
// The discrete low-pass's weight for a step dt at this cutoff (the derivative's, as One Euro's).
inline double Alpha(double cutoff, double dt) { return 1 / (1 + 1 / (2 * M_PI * cutoff * dt)); }
// How far a continuous first-order low-pass at this cutoff gets toward its input in dt.
inline double Toward(double cutoff, double dt) { return 1 - std::exp(-2 * M_PI * cutoff * dt); }
}  // namespace detail

// The input's slide from the sample before to the newest (see the top), and the low-pass's steps
// along it (at most kStep: the input moves within each).
constexpr double kStep = 0.004, kMaxSlide = 0.1;

template <int N>
class Point {
public:
    explicit Point(const Params &p = {}) : p_(p) {}
    // New settings (a held drag's precision, say): the cutoff changes from now, the output doesn't jump.
    void Set(const Params &p) {
        p_ = p;
        cutoff_ = p_.minCutoff + p_.beta * speed_;
    }
    const Params &Get() const { return p_; }
    void Reset() { init_ = false; }
    bool Ready() const { return init_; }
    // A tracker sample, captured at tc, arrived at now (pass tc for both when there's no clock to
    // tell them apart). The first one, or one after Reset, is taken as it is.
    void Sample(const double v[N], double tc, double now) {
        if (!init_) {
            std::copy(v, v + N, x_), std::copy(v, v + N, r_), std::copy(v, v + N, from_);
            std::fill(d_, d_ + N, 0.0);
            speed_ = 0, tc_ = tc, t_ = start_ = now, slide_ = 0, init_ = true;
            cutoff_ = p_.minCutoff;
            return;
        }
        Advance(now);
        const double dt = tc - tc_;
        if (dt > 1e-4) {
            // the speed from the raw samples (as the reference implementation), low-passed
            const double a = detail::Alpha(p_.dCutoff, std::min(dt, 0.5));
            double s2 = 0;
            for (int i = 0; i < N; ++i) {
                d_[i] += a * ((v[i] - r_[i]) / dt - d_[i]);
                s2 += d_[i] * d_[i];
            }
            speed_ = std::sqrt(s2);
            tc_ = tc;
        }
        Input(now, from_);  // the slide starts where the input is now
        std::copy(v, v + N, r_);
        start_ = now, slide_ = std::clamp(dt, 0.0, kMaxSlide);
        cutoff_ = p_.minCutoff + p_.beta * speed_;
    }
    // The output at now (moves the filter's timeline there; a time before it reads the output as it is).
    const double *At(double now) {
        Advance(now);
        return x_;
    }
    const double *Last() const { return x_; }   // the output where the timeline stands
    const double *Raw() const { return r_; }    // the newest sample
    double Speed() const { return speed_; }     // low-passed, units per second
    double Cutoff() const { return cutoff_; }

private:
    void Input(double t, double out[N]) const {
        const double u = slide_ > 0 ? std::clamp((t - start_) / slide_, 0.0, 1.0) : 1.0;
        for (int i = 0; i < N; ++i) out[i] = from_[i] + u * (r_[i] - from_[i]);
    }
    void Advance(double now) {
        if (now <= t_) return;
        if (now - t_ > 1) t_ = now - 1;
        while (t_ < now) {
            const double h = std::min(kStep, now - t_);
            t_ += h;
            double in[N];
            Input(t_, in);
            const double k = detail::Toward(cutoff_, h);
            for (int i = 0; i < N; ++i) x_[i] += k * (in[i] - x_[i]);
        }
    }
    Params p_;
    double x_[N] = {}, r_[N] = {}, from_[N] = {}, d_[N] = {};
    double speed_ = 0, cutoff_ = 1, tc_ = 0, t_ = 0, start_ = 0, slide_ = 0;
    bool init_ = false;
};

// A point in 3D (V3 in and out).
class Point3 {
public:
    explicit Point3(const Params &p = {}) : f_(p) {}
    void Set(const Params &p) { f_.Set(p); }
    const Params &Get() const { return f_.Get(); }
    void Reset() { f_.Reset(); }
    bool Ready() const { return f_.Ready(); }
    void Sample(V3 v, double tc, double now) {
        const double a[3] = {v.x, v.y, v.z};
        f_.Sample(a, tc, now);
    }
    V3 At(double now) {
        const double *x = f_.At(now);
        return {x[0], x[1], x[2]};
    }
    V3 Last() const { return {f_.Last()[0], f_.Last()[1], f_.Last()[2]}; }
    double Speed() const { return f_.Speed(); }

private:
    Point<3> f_;
};

// One value (a distance).
class Scalar {
public:
    explicit Scalar(const Params &p = {}) : f_(p) {}
    void Set(const Params &p) { f_.Set(p); }
    void Reset() { f_.Reset(); }
    bool Ready() const { return f_.Ready(); }
    void Sample(double v, double tc, double now) { f_.Sample(&v, tc, now); }
    double At(double now) { return *f_.At(now); }
    double Last() const { return *f_.Last(); }

private:
    Point<1> f_;
};

// A direction (unit vector), on the sphere; beta per deg/s.
class Direction {
public:
    explicit Direction(const Params &p = {}) : p_(p) {}
    void Set(const Params &p) {
        p_ = p;
        cutoff_ = p_.minCutoff + p_.beta * speed_;
    }
    const Params &Get() const { return p_; }
    void Reset() { init_ = false; }
    bool Ready() const { return init_; }
    void Sample(V3 v, double tc, double now) {
        v = Norm(v);
        if (!init_) {
            x_ = r_ = from_ = v, w_ = {}, speed_ = 0, tc_ = tc, t_ = start_ = now, slide_ = 0, init_ = true;
            cutoff_ = p_.minCutoff;
            return;
        }
        Advance(now);
        const double dt = tc - tc_;
        if (dt > 1e-4) {
            // angular velocity (axis times rate, deg/s) from the raw samples, low-passed as a vector
            const V3 axis = Cross(r_, v);
            const double s = Len(axis), w = Angle(r_, v) * 180 / M_PI / dt;
            const V3 now_w = s > 1e-12 ? axis * (w / s) : V3{};
            w_ = w_ + (now_w - w_) * detail::Alpha(p_.dCutoff, std::min(dt, 0.5));
            speed_ = Len(w_);
            tc_ = tc;
        }
        from_ = Input(now);
        r_ = v;
        start_ = now, slide_ = std::clamp(dt, 0.0, kMaxSlide);
        cutoff_ = p_.minCutoff + p_.beta * speed_;
    }
    V3 At(double now) {
        Advance(now);
        return x_;
    }
    V3 Last() const { return x_; }
    V3 Raw() const { return r_; }
    double Speed() const { return speed_; }  // deg/s, low-passed
    double Cutoff() const { return cutoff_; }

private:
    V3 Input(double t) const {
        return Slerp(from_, r_, slide_ > 0 ? std::clamp((t - start_) / slide_, 0.0, 1.0) : 1.0);
    }
    void Advance(double now) {
        if (now <= t_) return;
        if (now - t_ > 1) t_ = now - 1;
        while (t_ < now) {
            const double h = std::min(kStep, now - t_);
            t_ += h;
            x_ = Slerp(x_, Input(t_), detail::Toward(cutoff_, h));
        }
    }
    Params p_;
    V3 x_, r_, from_, w_;
    double speed_ = 0, cutoff_ = 1, tc_ = 0, t_ = 0, start_ = 0, slide_ = 0;
    bool init_ = false;
};

}  // namespace fhsmooth
