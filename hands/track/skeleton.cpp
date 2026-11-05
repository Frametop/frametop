#include "skeleton.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace skel {

// ------------------------------------------------------------------------------ rotations

V3 Rot::operator*(V3 v) const {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Rot Rot::operator*(const Rot &o) const {
    Rot r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
    return r;
}

Rot Rot::t() const {
    Rot r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
    return r;
}

Rot exp_rot(V3 w) {
    const double a = norm(w);
    Rot r;
    if (a < 1e-12) return r;
    const V3 k = w * (1 / a);
    const double c = std::cos(a), s = std::sin(a), v = 1 - c;
    r.m[0][0] = c + k[0] * k[0] * v, r.m[0][1] = k[0] * k[1] * v - k[2] * s, r.m[0][2] = k[0] * k[2] * v + k[1] * s;
    r.m[1][0] = k[1] * k[0] * v + k[2] * s, r.m[1][1] = c + k[1] * k[1] * v, r.m[1][2] = k[1] * k[2] * v - k[0] * s;
    r.m[2][0] = k[2] * k[0] * v - k[1] * s, r.m[2][1] = k[2] * k[1] * v + k[0] * s, r.m[2][2] = c + k[2] * k[2] * v;
    return r;
}

V3 log_rot(const Rot &r) {
    const double c = std::clamp((r.m[0][0] + r.m[1][1] + r.m[2][2] - 1) / 2, -1.0, 1.0), a = std::acos(c);
    const V3 v{r.m[2][1] - r.m[1][2], r.m[0][2] - r.m[2][0], r.m[1][0] - r.m[0][1]};
    if (a < 1e-6) return v * 0.5;
    if (a > M_PI - 1e-4) {   // near a half turn: the axis from the diagonal
        int i = 0;
        for (int k = 1; k < 3; ++k)
            if (r.m[k][k] > r.m[i][i]) i = k;
        V3 axis{};
        axis[i] = std::sqrt(std::max(0.0, (r.m[i][i] + 1) / 2));
        for (int k = 0; k < 3; ++k)
            if (k != i) axis[k] = (r.m[k][i] + r.m[i][k]) / (4 * axis[i]);
        return unit(axis) * a;
    }
    return v * (a / (2 * std::sin(a)));
}

void to_quat(const Rot &r, double q[4]) {
    const auto &m = r.m;
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        q[3] = s / 4, q[0] = (m[2][1] - m[1][2]) / s, q[1] = (m[0][2] - m[2][0]) / s, q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s, q[0] = s / 4, q[1] = (m[0][1] + m[1][0]) / s, q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s, q[0] = (m[0][1] + m[1][0]) / s, q[1] = s / 4, q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s, q[0] = (m[0][2] + m[2][0]) / s, q[1] = (m[1][2] + m[2][1]) / s, q[2] = s / 4;
    }
}

namespace {

V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 mirror(V3 v, int side) { return {side * v[0], v[1], v[2]}; }

Rot from_cols(V3 x, V3 y, V3 z) {
    Rot r;
    for (int i = 0; i < 3; ++i) r.m[i][0] = x[i], r.m[i][1] = y[i], r.m[i][2] = z[i];
    return r;
}

// Rotation about one of the frame's own axes.
Rot axis_rot(int axis, double a) {
    Rot r;
    const double c = std::cos(a), s = std::sin(a);
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;
    r.m[i][i] = c, r.m[i][j] = -s, r.m[j][i] = s, r.m[j][j] = c;
    return r;
}

// ---------------------------------------------------------------- the hand model's tables

// The palm's landmarks (rigid), and each chain: the thumb from its CMC (1), the fingers from
// their knuckles.
const int kPalmIds[6] = {0, 1, 5, 9, 13, 17};
const int kChainBase[5] = {1, 5, 9, 13, 17};
const int kChainQ[5] = {0, 6, 11, 16, 21};   // first angle in Fit::q

struct Dof {
    int joint;   // 0, 1, 2 along the chain
    int axis;    // 0 x: flexion (positive toward the palm); 1 y: abduction (positive toward the
                 // thumb's side); 2 z: twist about the bone
    double lo, hi;
};
// Angles in radians. Abduction is applied before flexion at a joint, so a bent finger keeps
// its spread.
constexpr double D = M_PI / 180;
// How far a chain's base (a knuckle, the thumb's CMC) may sit off the rigid palm: a prior's
// sigma and a hard limit, m.
constexpr double kBaseSigma = 0.004, kBaseMax = 0.015;
const Dof kThumb[6] = {{0, 1, -50 * D, 50 * D}, {0, 0, -50 * D, 50 * D}, {0, 2, -35 * D, 35 * D},   // CMC
                       {1, 1, -20 * D, 20 * D}, {1, 0, -30 * D, 80 * D},                            // MCP
                       {2, 0, -30 * D, 90 * D}};                                                    // IP
// Fingers get a little twist at the knuckle too: their flexion planes don't quite follow the
// palm's normal (in the 2026-10-03 gestures take, without it the fitted index tip sat a median
// 8 mm across the finger from the measured one).
const Dof kFinger[4][5] = {
    {{0, 1, -25 * D, 25 * D}, {0, 0, -35 * D, 95 * D}, {0, 2, -25 * D, 25 * D}, {1, 0, -10 * D, 110 * D}, {2, 0, -20 * D, 90 * D}},
    {{0, 1, -25 * D, 25 * D}, {0, 0, -35 * D, 95 * D}, {0, 2, -25 * D, 25 * D}, {1, 0, -10 * D, 110 * D}, {2, 0, -20 * D, 90 * D}},
    {{0, 1, -25 * D, 25 * D}, {0, 0, -35 * D, 95 * D}, {0, 2, -25 * D, 25 * D}, {1, 0, -10 * D, 110 * D}, {2, 0, -20 * D, 90 * D}},
    {{0, 1, -30 * D, 30 * D}, {0, 0, -35 * D, 95 * D}, {0, 2, -25 * D, 25 * D}, {1, 0, -10 * D, 110 * D}, {2, 0, -20 * D, 90 * D}},
};

const Dof *chain_dofs(int c, int *n) {
    *n = c == 0 ? 6 : 5;
    return c == 0 ? kThumb : kFinger[c - 1];
}

// The rotation a dof's angle makes about its axis: flexion turns the bone from -Z toward -Y,
// and abduction and twist are mirrored on a left hand so the same angles mean the same motion.
double dof_angle(const Dof &d, double q, int side) { return d.axis == 0 ? -q : side * q; }
double dof_sign(const Dof &d, int side) { return d.axis == 0 ? -1.0 : double(side); }

// The model's average hand (MediaPipe's world landmarks, medians over the 2026-09-30 lit
// recording, at Hand::scale 1), in the right hand's palm frame (x to the little finger,
// y out of the back, z toward the wrist), metres.
const V3 kCanonPalm[6] = {{0, 0, 0},
                          {-0.0149, -0.0142, -0.0158},
                          {-0.0172, -0.0043, -0.0657},
                          {0, 0, -0.0706},
                          {0.0162, 0, -0.0646},
                          {0.0286, -0.0043, -0.0559}};
const double kCanonPhalanx[15] = {0.0266, 0.0216, 0.0201, 0.0237, 0.0175, 0.0165, 0.0235, 0.0166,
                                  0.0176, 0.0214, 0.0163, 0.0134, 0.0168, 0.0109, 0.0108};
// The thumb's rest direction from its CMC, and the way its MCP bends (same data), right hand.
const V3 kThumbRest{-0.50, -0.48, -0.70};
const V3 kThumbFlex{0.80, -0.40, -0.37};

int phalanx_bone(int k) { return kChainBase[k / 3] + k % 3; }   // phalanx k -> bone index

// A chain's rest frame in the palm frame (mirrored for side): -Z along the resting bone.
Rot rest_frame(const Shape &s, int c, int side) {
    V3 z, y;
    if (c == 0) {
        const V3 r = unit(kThumbRest), f = kThumbFlex;
        z = r * -1;
        y = unit(f - r * dot(f, r)) * -1;
    } else {
        // halfway between the metacarpal and the palm's axis, in the palm's plane
        V3 m = s.palm[kChainBase[c]];
        m[1] = 0;
        z = unit(unit(m) + V3{0, 0, -1}) * -1;
        y = {0, 1, 0};
    }
    z = mirror(z, side), y = mirror(y, side);
    return from_cols(cross(y, z), y, z);
}

struct ChainState {
    V3 base;            // the chain's first landmark (on the palm, plus its offset)
    V3 pts[3];          // the chain's landmarks
    Rot frames[3];      // each joint's frame
    V3 axis[6], pivot[6];
};

// rest: rest_frame(s, c, side), which the caller keeps for the chain's iterations
void chain_fk(const Shape &s, int c, int side, const Rot &rest, const Rot &R, V3 t, const double *q, V3 off, ChainState &st) {
    int n;
    const Dof *dofs = chain_dofs(c, &n);
    const int base = kChainBase[c];
    Rot F = R * rest;
    V3 p = R * mirror(s.palm[base], side) + t + off;
    st.base = p;
    int d = 0;
    for (int j = 0; j < 3; ++j) {
        for (; d < n && dofs[d].joint == j; ++d) {
            st.axis[d] = F.col(dofs[d].axis);
            st.pivot[d] = p;
            F = F * axis_rot(dofs[d].axis, dof_angle(dofs[d], q[d], side));
        }
        st.frames[j] = F;
        p = p - F.col(2) * s.len[base + j];
        st.pts[j] = p;
    }
}

// Point weights: W = a I + b d d^T, d the line of sight (depth is the noisy direction).
struct Weight {
    double a, b;
    V3 d;
    V3 apply(V3 r) const { return r * a + d * (b * dot(d, r)); }
};

Weight weight(V3 y, V3 eye, double sl, double sd) {
    return {1 / (sl * sl), 1 / (sd * sd) - 1 / (sl * sl), unit(y - eye)};
}

// Solves H x = -g for small symmetric positive definite H (Cholesky). False if it isn't.
constexpr int kMaxP = 9;   // a chain's parameters: up to 6 angles and the base's offset
bool solve_spd(double H[kMaxP][kMaxP], const double *g, double *x, int n) {
    double L[kMaxP][kMaxP] = {};
    for (int i = 0; i < n; ++i)
        for (int j = 0; j <= i; ++j) {
            double s = H[i][j];
            for (int k = 0; k < j; ++k) s -= L[i][k] * L[j][k];
            if (i == j) {
                if (s <= 1e-18) return false;
                L[i][i] = std::sqrt(s);
            } else {
                L[i][j] = s / L[j][j];
            }
        }
    double y[kMaxP];
    for (int i = 0; i < n; ++i) {
        double s = -g[i];
        for (int k = 0; k < i; ++k) s -= L[i][k] * y[k];
        y[i] = s / L[i][i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < n; ++k) s -= L[k][i] * x[k];
        x[i] = s / L[i][i];
    }
    return true;
}

// The palm frame straight from measured landmarks (wrist, index, middle and little knuckles).
bool measured_frame(const V3 p[21], int side, Rot &R) {
    const V3 f0 = p[9] - p[0];
    if (norm(f0) < 0.02) return false;
    const V3 f = unit(f0);
    V3 a = p[5] - p[17];
    a = a - f * dot(a, f);
    if (norm(a) < 0.008) return false;
    a = unit(a);
    const V3 y = cross(f, a) * double(side), z = f * -1;
    R = from_cols(cross(y, z), y, z);
    return true;
}

double angle(V3 a, V3 b) { return std::acos(std::clamp(dot(unit(a), unit(b)), -1.0, 1.0)); }

double median(const std::vector<float> &v) {
    if (v.empty()) return 0;
    float tmp[Calibration::kSettle];
    const size_t n = std::min(v.size(), size_t(Calibration::kSettle));
    std::copy(v.begin(), v.begin() + n, tmp);
    std::nth_element(tmp, tmp + n / 2, tmp + n);
    return tmp[n / 2];
}

void to_quantities(const Shape &s, double *q) {
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 3; ++j) q[3 * k + j] = s.palm[kPalmIds[k + 1]][j];
    for (int k = 0; k < 15; ++k) q[15 + k] = s.len[phalanx_bone(k)];
}

}  // namespace

void Shape::from_quantities(const double *q) {
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 3; ++j) palm[kPalmIds[k + 1]][j] = q[3 * k + j];
    for (int k = 0; k < 15; ++k) len[phalanx_bone(k)] = q[15 + k];
    for (int c = 0; c < 5; ++c) len[kChainBase[c] - 1] = norm(palm[kChainBase[c]]);   // bones 0, 4, 8, 12, 16
}

// ---------------------------------------------------------------------------- calibration

void Calibration::add(const V3 local[21], const bool ok[21], const bool finger_ok[5]) {
    double x[kQuantities];
    Shape s;
    for (int k : {1, 5, 9, 13, 17}) s.palm[k] = local[k];
    for (int b = 0; b < 20; ++b) s.len[b] = norm(local[b + 1] - local[b % 4 == 0 ? 0 : b]);
    to_quantities(s, x);
    // a phalanx far off the model's average hand's proportions is a misplaced joint (the dim
    // typing recording measured a middle phalanx 1.9 times as long as that), not this hand
    const double palm_len = norm(local[9] - local[0]);
    for (int k = 0; k < kQuantities; ++k) {
        if (k >= 15) {
            const double r = x[k] / (palm_len * kCanonPhalanx[k - 15] / -kCanonPalm[3][2]);
            if (!(r > 0.7 && r < 1.5)) continue;
        }
        if (k < 15 ? !ok[kPalmIds[k / 3 + 1]] || !ok[0] || !ok[5] || !ok[9] || !ok[17] : !finger_ok[(k - 15) / 3]) continue;
        if (k >= 15) {
            const int b = phalanx_bone(k - 15);
            if (!ok[b] || !ok[b + 1]) continue;
        }
        if (n_[k] < kSettle) {
            buf_[k].push_back(float(x[k]));
            est_[k] = median(buf_[k]);   // so far
            if (++n_[k] == kSettle) {
                buf_[k].clear();
                buf_[k].shrink_to_fit();
            }
        } else if (std::fabs(x[k] - est_[k]) < 0.3 * std::fabs(est_[k]) + 0.003) {
            est_[k] += std::clamp(x[k] - est_[k], -2e-5, 2e-5);   // a running median, at most 0.02 mm a sample
            ++n_[k];
        }
    }
}

Shape Calibration::shape(const Shape &prior) const {
    double p[kQuantities], q[kQuantities];
    to_quantities(prior, p);
    constexpr double kPriorWeight = 8;
    for (int k = 0; k < kQuantities; ++k) {
        if (n_[k] >= kSettle) q[k] = est_[k];
        else if (n_[k] == 0) q[k] = p[k];
        else q[k] = (n_[k] * est_[k] + kPriorWeight * p[k]) / (n_[k] + kPriorWeight);
    }
    Shape s;
    s.from_quantities(q);
    return s;
}

double Calibration::settled() const {
    int least = kSettle;
    for (int k = 0; k < kQuantities; ++k) least = std::min(least, n_[k]);
    return double(least) / kSettle;
}

// ---------------------------------------------------------------------------- skeleton

Shape Skeleton::canonical(double scale) {
    Shape s;
    double q[kQuantities];
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 3; ++j) q[3 * k + j] = kCanonPalm[k + 1][j] * scale;
    for (int k = 0; k < 15; ++k) q[15 + k] = kCanonPhalanx[k] * scale;
    s.from_quantities(q);
    return s;
}

Shape Skeleton::shape(int side, double scale) const {
    const Calibration &me = cal_[side > 0], &other = cal_[side <= 0];
    // the other hand, when it knows more: left and right hands are mirror images
    const Shape prior = other.settled() > me.settled() ? other.shape(canonical(scale)) : canonical(scale);
    return me.shape(prior);
}

void Skeleton::pose(const Shape &s, int side, const Rot &R, V3 t, const double *q, V3 out[21], Rot frames[21]) {
    out[0] = t;
    frames[0] = R;
    for (int c = 0; c < 5; ++c) {
        ChainState st;
        const double *o = q + kAngles + 3 * c;
        chain_fk(s, c, side, rest_frame(s, c, side), R, t, q + kChainQ[c], {o[0], o[1], o[2]}, st);
        const int base = kChainBase[c];
        out[base] = st.base;
        for (int j = 0; j < 3; ++j) out[base + 1 + j] = st.pts[j], frames[base + j] = st.frames[j];
        frames[base + 3] = st.frames[2];   // the tip: its bone's frame
    }
}

double Skeleton::solve(const Shape &s, int side, const V3 pts[21], const double sigma_l[21], const double sigma_d[21],
                       V3 eye, const double *prev, int iterations, Rot &R, V3 &t, double *q, double *rms, double limits,
                       double chain_rms[5], bool warm) {
    Weight w[21];
    for (int i = 0; i < 21; ++i) w[i] = weight(pts[i], eye, sigma_l[i], sigma_d[i]);

    // 1. the palm, rigid: from the measured frame, then Gauss-Newton on rotation and position
    if (!measured_frame(pts, side, R)) return -1;
    V3 T[6];
    for (int k = 0; k < 6; ++k) T[k] = mirror(s.palm[kPalmIds[k]], side);
    {
        V3 c{};
        double ws = 0;
        for (int k = 0; k < 6; ++k) {
            const double wk = w[kPalmIds[k]].a;
            c = c + (pts[kPalmIds[k]] - R * T[k]) * wk, ws += wk;
        }
        t = c * (1 / ws);
    }
    for (int it = 0; it < 3; ++it) {
        double H[kMaxP][kMaxP] = {}, g[6] = {}, x[6];
        for (int k = 0; k < 6; ++k) {
            const int i = kPalmIds[k];
            const V3 v = R * T[k], r = v + t - pts[i];
            V3 J[6];
            for (int a = 0; a < 3; ++a) {
                V3 e{};
                e[a] = 1;
                J[a] = cross(e, v);
                J[3 + a] = e;
            }
            V3 WJ[6];
            for (int a = 0; a < 6; ++a) WJ[a] = w[i].apply(J[a]);
            for (int a = 0; a < 6; ++a) {
                g[a] += dot(WJ[a], r);
                for (int b = 0; b <= a; ++b) H[a][b] += dot(J[b], WJ[a]);
            }
        }
        for (int a = 0; a < 6; ++a) {
            for (int b = 0; b < a; ++b) H[b][a] = H[a][b];
            H[a][a] *= 1 + 1e-6;
        }
        if (!solve_spd(H, g, x, 6)) break;
        R = exp_rot({x[0], x[1], x[2]}) * R;
        t = t + V3{x[3], x[4], x[5]};
        if (norm(V3{x[0], x[1], x[2]}) < 1e-4 && norm(V3{x[3], x[4], x[5]}) < 1e-5) break;
    }

    // 2. each chain from its base on the fitted palm. The base may move off the rigid palm a
    // little (kBaseSigma): the measured knuckles wander far more than a palm bends (the little
    // finger's by 3 cm), so a rigid palm alone puts whole fingers beside their measurements.
    // Angles start straight from the measured bone directions, then Gauss-Newton with the
    // weights, the joint limits and those priors.
    double cost = 0;
    {
        const V3 r = t - pts[0];
        cost += dot(r, w[0].apply(r));
    }
    for (int c = 0; c < 5; ++c) {
        int n;
        const Dof *dofs = chain_dofs(c, &n);
        const int base = kChainBase[c], np = n + 3;   // angles, then the base's offset
        const Rot rest = rest_frame(s, c, side);
        double *qc = q + kChainQ[c], *oc = q + kAngles + 3 * c;
        const double *pc = prev ? prev + kChainQ[c] : nullptr;
        // initial offset: most of the way to the measured knuckle
        {
            const V3 d = pts[base] - (R * mirror(s.palm[base], side) + t);
            const double k = kBaseSigma * kBaseSigma / (kBaseSigma * kBaseSigma + sigma_l[base] * sigma_l[base]);
            V3 o = d * k;
            if (norm(o) > kBaseMax) o = o * (kBaseMax / norm(o));
            for (int j = 0; j < 3; ++j) oc[j] = o[j];
        }
        // initial angles
        {
            Rot F = R * rest;
            V3 p = R * mirror(s.palm[base], side) + t + V3{oc[0], oc[1], oc[2]};
            int d = 0;
            for (int j = 0; j < 3; ++j) {
                const V3 l = F.t() * unit(pts[base + 1 + j] - p);
                for (; d < n && dofs[d].joint == j; ++d) {
                    double a = 0;
                    if (dofs[d].axis == 1) a = std::atan2(-l[0], -l[2]) * side;   // abduction (before flexion)
                    else if (dofs[d].axis == 0) a = dofs[d].joint == 0 || (c == 0 && j == 1) ? std::asin(std::clamp(-l[1], -1.0, 1.0))
                                                                                             : std::atan2(-l[1], -l[2]);
                    else a = pc ? pc[d] : 0;   // twist: unseen in one bone's direction
                    qc[d] = std::clamp(a, dofs[d].lo * limits, dofs[d].hi * limits);
                    F = F * axis_rot(dofs[d].axis, dof_angle(dofs[d], qc[d], side));
                }
                p = p - F.col(2) * s.len[base + j];
            }
        }
        // priors: the base's offset toward none; a weak pull toward last frame's angles (and the
        // twist toward none), for what one frame doesn't show: the twist of a straight thumb, a
        // finger seen end-on
        double lambda[kMaxP], target[kMaxP], x[kMaxP];
        for (int d = 0; d < n; ++d) {
            const double sigma = dofs[d].axis == 2 ? 0.35 : 0.6;
            lambda[d] = 1e-6 / (0.002 * 0.002) / (sigma * sigma);   // 1 rad off weighs like 1 mm off at 2 mm sigma, over sigma^2
            target[d] = dofs[d].axis == 2 ? 0 : pc ? pc[d] : qc[d];
        }
        for (int j = 0; j < 3; ++j) lambda[n + j] = 1 / (kBaseSigma * kBaseSigma), target[n + j] = 0;
        // Gauss-Newton from (qa, oa), in place; returns the chain's cost, and its plain rms in *crms
        auto run = [&](double *qa, double *oa, double *crms) {
            auto params = [&](int k) -> double & { return k < n ? qa[k] : oa[k - n]; };
            ChainState st;
            for (int it = 0; it < iterations; ++it) {
                chain_fk(s, c, side, rest, R, t, qa, {oa[0], oa[1], oa[2]}, st);
                double H[kMaxP][kMaxP] = {}, g[kMaxP] = {};
                for (int m = 0; m < 4; ++m) {   // the base, then the chain's three landmarks
                    const int lm = base + m;
                    const Weight &wm = w[lm];
                    const V3 at = m ? st.pts[m - 1] : st.base, r = at - pts[lm];
                    V3 J[kMaxP], WJ[kMaxP];
                    for (int d = 0; d < n; ++d)
                        J[d] = m && dofs[d].joint <= m - 1 ? cross(st.axis[d], at - st.pivot[d]) * dof_sign(dofs[d], side) : V3{0, 0, 0};
                    for (int j = 0; j < 3; ++j) J[n + j] = {0, 0, 0}, J[n + j][j] = 1;
                    for (int d = 0; d < np; ++d) WJ[d] = wm.apply(J[d]);
                    for (int a2 = 0; a2 < np; ++a2) {
                        g[a2] += dot(WJ[a2], r);
                        for (int b2 = 0; b2 <= a2; ++b2) H[a2][b2] += dot(J[b2], WJ[a2]);
                    }
                }
                for (int a2 = 0; a2 < np; ++a2) {
                    for (int b2 = 0; b2 < a2; ++b2) H[b2][a2] = H[a2][b2];
                    H[a2][a2] = H[a2][a2] * (1 + 1e-3) + lambda[a2];
                    g[a2] += lambda[a2] * (params(a2) - target[a2]);
                }
                if (!solve_spd(H, g, x, np)) break;
                double step = 0;
                for (int d = 0; d < n; ++d) {
                    const double nq = std::clamp(qa[d] + x[d], dofs[d].lo * limits, dofs[d].hi * limits);
                    step = std::max(step, std::fabs(nq - qa[d]));
                    qa[d] = nq;
                }
                V3 o{oa[0] + x[n], oa[1] + x[n + 1], oa[2] + x[n + 2]};
                if (norm(o) > kBaseMax) o = o * (kBaseMax / norm(o));
                for (int j = 0; j < 3; ++j) step = std::max(step, std::fabs(o[j] - oa[j]) * 10), oa[j] = o[j];   // 0.1 mm ~ 1e-3 rad
                if (step < 1e-3) break;
            }
            chain_fk(s, c, side, rest, R, t, qa, {oa[0], oa[1], oa[2]}, st);
            double cc = 0, sq = 0;
            for (int m = 0; m < 4; ++m) {
                const V3 r = (m ? st.pts[m - 1] : st.base) - pts[base + m];
                cc += dot(r, w[base + m].apply(r));
                sq += dot(r, r);
            }
            for (int d = 0; d < np; ++d) cc += lambda[d] * (params(d) - target[d]) * (params(d) - target[d]);
            *crms = std::sqrt(sq / 4);
            return cc;
        };
        double crms = 0, cc = run(qc, oc, &crms);
        // and from last frame's pose: the one that fits better wins, so the fit doesn't hop
        // between two poses that fit about as well from one frame's analytic start to the next
        if (pc && warm) {
            double q2[kMaxP], o2[3], crms2 = 0;
            const double *po = prev + kAngles + 3 * c;
            for (int d = 0; d < n; ++d) q2[d] = std::clamp(pc[d], dofs[d].lo * limits, dofs[d].hi * limits);
            for (int j = 0; j < 3; ++j) o2[j] = po[j];
            const double cc2 = run(q2, o2, &crms2);
            if (cc2 < cc) {
                cc = cc2, crms = crms2;
                std::copy(q2, q2 + n, qc);
                std::copy(o2, o2 + 3, oc);
            }
        }
        cost += cc;
        if (chain_rms) chain_rms[c] = crms;
    }
    if (rms) {
        V3 out[21];
        Rot frames[21];
        pose(s, side, R, t, q, out, frames);
        double sum = 0;
        for (int i = 0; i < 21; ++i) sum += dot(out[i] - pts[i], out[i] - pts[i]);
        *rms = std::sqrt(sum / 21);
    }
    return cost;
}

V3 Skeleton::elbow(V3 wrist, V3 mid_knuckle, int side, double forearm, V3 up) {
    // frame-hands' train/forearm/armmodel.py: a neck pivot fixed in the head frame, the
    // shoulders off it in a torso that faces where the head does (level), upper arm 1.5 x the
    // forearm, the elbow on the circle the two bones allow, turned -3 degrees from straight down
    const V3 neck{0, -0.12, 0.08};
    constexpr double kShoulderW = 0.18, kShoulderD = 0.06, kUpper = 1.5, kSwivel = -3 * D, kMaxBend = 75 * D;
    V3 fwd{0, 0, -1};
    fwd = fwd - up * dot(fwd, up);
    const V3 torso = norm(fwd) > 1e-3 ? unit(fwd) : V3{0, 0, -1};
    const V3 right = unit(cross(torso, up));
    const V3 S = neck + right * (side * kShoulderW) - up * kShoulderD;
    const double F = forearm, U = kUpper * forearm;
    const V3 v = wrist - S;
    const double d = norm(v);
    const V3 stub = unit(wrist - mid_knuckle);
    V3 dir = stub;
    if (d > 1e-3) {
        const V3 n = v * (1 / d);
        const double dd = std::clamp(d, std::fabs(U - F) + 1e-4, U + F - 1e-4);
        const double a = (U * U - F * F + dd * dd) / (2 * dd), rho = std::sqrt(std::max(U * U - a * a, 0.0));
        const V3 C = S + n * a;
        V3 down = up * -1;
        down = down - n * dot(n, down);
        if (norm(down) > 1e-6) {
            const V3 v0 = unit(down);
            V3 v1 = cross(n, v0) * double(side);
            if (dot(v1, right) * side < 0) v1 = v1 * -1;
            const V3 E = C + (v0 * std::cos(kSwivel) + v1 * std::sin(kSwivel)) * rho;
            if (norm(E - wrist) > 1e-6) dir = unit(E - wrist);
        }
    }
    // a wrist bends at most about this far from the forearm's line
    const double bend = angle(dir, stub);
    if (bend > kMaxBend) {
        const V3 perp = unit(dir - stub * dot(dir, stub));
        dir = stub * std::cos(kMaxBend) + perp * std::sin(kMaxBend);
    }
    return wrist + dir * F;
}

void Skeleton::update(Fit &f, const V3 pts[21], bool right, double right_score, int nviews, double residual,
                      const double *res_pt, V3 eye, double scale, int64_t t_ns) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool continuing = f.valid && t_ns > f.ns && t_ns - f.ns < 300'000'000;
    f.valid = false;
    if (nviews <= 0) return;
    const int model_side = right ? 1 : -1;
    double sl[21], sd[21];
    for (int i = 0; i < 21; ++i) {
        // stereo: a landmark whose rays miss each other is less sure
        const double r = res_pt && res_pt[i] >= 0 ? res_pt[i] : residual;
        sl[i] = nviews >= 2 ? std::hypot(0.002, std::max(0.0, r)) : 0.003;
        sd[i] = nviews >= 2 ? p_.depth_ratio * sl[i] : 0.015;
    }
    // the side: the model's, unless the mirror image fits far better (a finger can't bend
    // backward, so a hand fitted as the wrong side fits badly once its fingers bend) for
    // kFlipFrames in a row. A side chosen that way sticks while the hand is tracked, as the
    // model's call does; switching resets the filter, so it mustn't flicker.
    constexpr int kFlipFrames = 3;
    // The model's call is a running average (Hand::right_score); while it hovers near 0.5 a
    // tracked hand keeps the side it has, so the skeleton doesn't turn inside out.
    int side = model_side;
    if (continuing && f.side != model_side && (f.flipped || std::fabs(right_score - 0.5) < 0.25)) side = f.side;
    if (!continuing) f.votes = 0;
    Shape s = shape(side, scale);
    Rot R;
    V3 t;
    double q[kParams] = {}, rms = 0;
    const double *prev = continuing && f.side == side ? f.q : nullptr;
    double crms[5] = {};
    double cost = solve(s, side, pts, sl, sd, eye, prev, p_.iterations, R, t, q, &rms, p_.limits, crms, p_.warm > 0);
    if (cost < 0) return;
    // A mirror image follows fingers that bend toward the palm only by bending its own backward
    // (the knuckles allow -35 degrees): a fit as the other side than the model's call that does,
    // on average over the four fingers, is the wrong side however well it fits. Such a fit goes
    // back to the model's side at once, and a mirror that needs it never wins.
    auto backward = [](const double *qq) {
        double m = 0;
        for (int c = 0; c < 4; ++c) m += qq[6 + 5 * c + 1];
        return m / 4 < -10 * D;
    };
    if (side != model_side && backward(q)) {
        Rot R2;
        V3 t2;
        double q2[kParams] = {}, rms2 = 0, crms2[5] = {};
        const Shape s2 = shape(model_side, scale);
        const double cost2 = solve(s2, model_side, pts, sl, sd, eye, nullptr, p_.iterations, R2, t2, q2, &rms2, p_.limits,
                                   crms2, p_.warm > 0);
        if (cost2 >= 0) {
            side = model_side, s = s2, R = R2, t = t2, rms = rms2, cost = cost2;
            std::copy(q2, q2 + kParams, q);
            std::copy(crms2, crms2 + 5, crms);
            f.votes = 0;
        }
    }
    bool better = false;
    if (rms > 0.015) {
        Rot R2;
        V3 t2;
        double q2[kParams] = {}, rms2 = 0;
        const Shape s2 = shape(-side, scale);
        double crms2[5] = {};
        const double cost2 = solve(s2, -side, pts, sl, sd, eye, continuing && f.side == -side ? f.q : nullptr,
                                   p_.iterations, R2, t2, q2, &rms2, p_.limits, crms2, p_.warm > 0);
        better = cost2 >= 0 && cost2 < 0.5 * cost && !(-side != model_side && backward(q2));
        if (better && ++f.votes >= kFlipFrames) {
            side = -side, s = s2, R = R2, t = t2, rms = rms2, cost = cost2;
            std::copy(q2, q2 + kParams, q);
            std::copy(crms2, crms2 + 5, crms);
            f.votes = 0;
            ++flips;
        }
    }
    if (!better) f.votes = 0;
    f.flipped = side != model_side;

    // learn the bone lengths from clean stereo frames, in the hand's own palm frame
    if (nviews >= 2 && residual >= 0 && residual <= 0.004 && side == model_side && std::fabs(right_score - 0.5) > 0.3) {
        bool pt_ok[21];
        for (int i = 0; i < 21; ++i) pt_ok[i] = !res_pt || (res_pt[i] >= 0 && res_pt[i] <= 0.004);
        Rot M;
        if (measured_frame(pts, side, M)) {
            V3 local[21];
            const Rot Mt = M.t();
            for (int i = 0; i < 21; ++i) local[i] = mirror(Mt * (pts[i] - pts[0]), side);
            bool ok[5];
            for (int c = 0; c < 5; ++c) {
                const int b = kChainBase[c];
                // bent fingers hide their own joints from the cameras: only fairly straight ones
                const double bend = angle(local[b + 1] - local[b], local[b + 2] - local[b + 1]) +
                                    angle(local[b + 2] - local[b + 1], local[b + 3] - local[b + 2]);
                ok[c] = bend < 90 * D;
            }
            cal_[side > 0].add(local, pt_ok, ok);
        }
    }

    // filter the pose: the palm centre, the palm's rotation, each chain's angles
    V3 Tc{};
    for (int k : {0, 5, 9, 13, 17}) Tc = Tc + mirror(s.palm[k], side) * 0.2;
    const V3 c = R * Tc + t;
    const double dt = continuing ? (t_ns - f.ns) / 1e9 : 0;
    if (!continuing || f.side != side || dt <= 0) {
        f.Rf = R, f.cf = c, f.vel = f.angvel = {0, 0, 0};
        std::copy(q, q + kParams, f.qf);
        std::fill(f.dq, f.dq + kParams, 0.0);
    } else {
        auto alpha = [dt](double cutoff) { return 1 / (1 + 1 / (2 * M_PI * cutoff * dt)); };
        const double ad = alpha(p_.d_cutoff);
        f.vel = f.vel + ((c - f.cf) * (1 / dt) - f.vel) * ad;
        f.cf = f.cf + (c - f.cf) * alpha(p_.pos_min + p_.pos_beta * norm(f.vel));
        const V3 delta = log_rot(f.Rf.t() * R);   // in the filtered palm's frame
        f.angvel = f.angvel + (f.Rf * delta * (1 / dt) - f.angvel) * ad;
        f.Rf = f.Rf * exp_rot(delta * alpha(p_.rot_min + p_.rot_beta * norm(f.angvel)));
        for (int ch = 0; ch < 5; ++ch) {
            const int a = kChainQ[ch], b = ch + 1 < 5 ? kChainQ[ch + 1] : kAngles;
            // a chain that fits its landmarks badly this frame is mostly a misplaced landmark (a
            // finger hidden behind another): it moves the filter, and its speed, less
            const double trust = std::min(1.0, std::pow(p_.glitch / std::max(crms[ch], 1e-6), 2));
            double speed = 0;
            for (int d = a; d < b; ++d) {
                f.dq[d] += trust * ad * ((q[d] - f.qf[d]) / dt - f.dq[d]);
                speed = std::max(speed, std::fabs(f.dq[d]));
            }
            const double ac = trust * alpha(p_.ang_min + p_.ang_beta * speed);
            for (int d = a; d < b; ++d) f.qf[d] += ac * (q[d] - f.qf[d]);
            for (int d = kAngles + 3 * ch; d < kAngles + 3 * ch + 3; ++d) f.qf[d] += ac * (q[d] - f.qf[d]);   // its base
        }
    }
    std::copy(crms, crms + 5, f.chain_rms);
    f.R = R, f.t = t, f.rms = rms, f.side = side, f.ns = t_ns, f.valid = true;
    std::copy(q, q + kParams, f.q);
    {
        Rot fr[21];
        pose(s, side, R, t, q, f.raw, fr);
    }
    const V3 tf = f.cf - f.Rf * Tc;
    pose(s, side, f.Rf, tf, f.qf, f.pts, f.frames);
    std::copy(s.len, s.len + 20, f.len);
    // rough radii, from the capsules' (io.cpp) at this hand's size
    const double k = norm(s.palm[9]) / 0.0815;
    for (int i = 0; i < 21; ++i) f.radius[i] = k * (i == 0 ? 0.020 : i == 1 || i % 4 == 1 ? 0.012 : i <= 4 ? 0.0095 : 0.0085);
    f.elbow = elbow(tf, f.pts[9], side, p_.forearm_ratio * norm(s.palm[9]), up_);
    f.settled = cal_[side > 0].settled();
    f.us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    ++fits, us_total += f.us;
}

}  // namespace skel
