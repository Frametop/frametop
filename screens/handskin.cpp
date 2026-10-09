// Rendered hands from a 3D model, the CPU side (see handskin.h).
#include "handskin.h"

#include "handasset.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace handskin {
namespace {

static_assert(handasset::kJoints == kJoints, "the model's joints");

struct V {
    double x, y, z;
    V(double a = 0, double b = 0, double c = 0) : x(a), y(b), z(c) {}
    explicit V(const float p[3]) : x(p[0]), y(p[1]), z(p[2]) {}
    V operator+(const V &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(const V &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const V &o) const { return x * o.x + y * o.y + z * o.z; }
    V cross(const V &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(dot(*this)); }
    V unit() const {
        const double n = norm();
        return n > 1e-12 ? *this * (1 / n) : V{};
    }
};

// A frame: columns x (sideways: the axis a finger bends about), y, d (along the bone).
struct Frame {
    V x, y, d;
};

// The frame along `d` with its sideways axis from `hint` (else `alt`, when hint is nearly along d).
Frame Along(const V &d, const V &hint, const V &alt) {
    Frame f;
    f.d = d.unit();
    const V h = hint.unit(), a = alt.unit();
    V x = h - f.d * h.dot(f.d);
    if (x.norm() < 0.25) x = a - f.d * a.dot(f.d);
    f.x = x.unit();
    f.y = f.d.cross(f.x);
    return f;
}

constexpr int kBase[5] = {1, 5, 10, 15, 20};   // each chain's first joint: thumb, index .. pinky
constexpr int kEnd[5] = {4, 9, 14, 19, 24};    // ...and its tip

bool IsMetacarpal(int j) { return j == 5 || j == 10 || j == 15 || j == 20; }

// The palm: d from the wrist to the middle knuckle, x across from the index knuckle to the
// little finger's; and its size (the wrist to the four knuckles).
bool Palm(const V J[kJoints], Frame &f, double &size) {
    const V fwd = J[11] - J[0], across = J[21] - J[6];
    if (fwd.norm() < 0.02 || across.norm() < 0.01) return false;
    f = Along(fwd, across, V(0, 1, 0));
    if (f.x.norm() < 0.5) return false;
    size = 0;
    for (int j : {6, 11, 16, 21}) size += (J[j] - J[0]).norm();
    return true;
}

// Every joint's frame from the 25 positions (the metacarpals placed already).
void Frames(const V J[kJoints], const Frame &palm, Frame out[kJoints]) {
    out[0] = palm;
    for (int c = 0; c < 5; ++c) {
        for (int j = kBase[c]; j <= kEnd[c]; ++j) {
            const V d = j < kEnd[c] ? J[j + 1] - J[j] : J[j] - J[j - 1];
            // the first bone rolls with the palm: across it for a finger's metacarpal, its normal
            // for the thumb's; each after takes its parent's sideways axis
            const bool first = j == kBase[c];
            const V hint = !first ? out[j - 1].x : c == 0 ? palm.y : palm.x;
            const V alt = c == 0 ? palm.x : palm.y;
            out[j] = Along(d, hint, alt);
        }
    }
}

V Bind(bool right, int j) { return V(handasset::kHands[right].bind[j]); }

}  // namespace

bool Pose(const handcut::Hand &h, Bones &out) {
    V B[kJoints], J[kJoints];
    for (int j = 0; j < kJoints; ++j) B[j] = Bind(h.right, j);
    for (int k = 0; k < 21; ++k) {
        if (!std::isfinite(h.p[k][0]) || !std::isfinite(h.p[k][1]) || !std::isfinite(h.p[k][2])) return false;
        J[kFromMediaPipe[k]] = V(h.p[k]);
    }
    Frame bp, tp;
    double bs, ts;
    if (!Palm(B, bp, bs) || !Palm(J, tp, ts)) return false;
    const double s = std::clamp(ts / bs, 0.6, 1.6);
    // the metacarpals: where the model has them, moved with the palm
    for (int j : {5, 10, 15, 20}) {
        const V l = B[j] - B[0];
        const V local(l.dot(bp.x), l.dot(bp.y), l.dot(bp.d));
        J[j] = J[0] + (tp.x * local.x + tp.y * local.y + tp.d * local.z) * s;
    }
    Frame bf[kJoints], tf[kJoints];
    Frames(B, bp, bf);
    Frames(J, tp, tf);
    for (int j = 0; j < kJoints; ++j) {
        // along the bone: the tracked bone's length (a metacarpal's, the palm's scale)
        double k = s;
        const bool last = j == 0 || j == 4 || j == 9 || j == 14 || j == 19 || j == 24;
        if (!last && !IsMetacarpal(j)) {
            const double bl = (B[j + 1] - B[j]).norm(), tl = (J[j + 1] - J[j]).norm();
            if (bl > 1e-4) k = std::clamp(tl / bl, 0.6 * s, 1.6 * s);
        }
        // M = Rt S Rb^T: a bind point's frame coordinates, scaled, into the tracked frame
        const Frame &b = bf[j], &t = tf[j];
        double M[3][3];
        for (int r = 0; r < 3; ++r) {
            const double tx[3] = {t.x.x, t.x.y, t.x.z}, ty[3] = {t.y.x, t.y.y, t.y.z}, td[3] = {t.d.x, t.d.y, t.d.z};
            for (int c = 0; c < 3; ++c) {
                const double bx[3] = {b.x.x, b.x.y, b.x.z}, by[3] = {b.y.x, b.y.y, b.y.z}, bd[3] = {b.d.x, b.d.y, b.d.z};
                M[r][c] = s * tx[r] * bx[c] + s * ty[r] * by[c] + k * td[r] * bd[c];
            }
        }
        const double bj[3] = {B[j].x, B[j].y, B[j].z}, tj[3] = {J[j].x, J[j].y, J[j].z};
        for (int r = 0; r < 3; ++r) {
            double t3 = tj[r];
            for (int c = 0; c < 3; ++c) out.m[j][r * 4 + c] = float(M[r][c]), t3 -= M[r][c] * bj[c];
            out.m[j][r * 4 + 3] = float(t3);
        }
    }
    for (int i = 0; i < 3; ++i) out.wrist[i] = h.p[0][i];
    out.fwd[0] = float(tp.d.x), out.fwd[1] = float(tp.d.y), out.fwd[2] = float(tp.d.z);
    out.scale = float(s);
    return true;
}

int Vertices(bool right) { return handasset::kHands[right].verts; }

void BindVertex(bool right, int i, float out[3]) {
    const int16_t *p = handasset::kHands[right].pos + 3 * i;
    for (int k = 0; k < 3; ++k) out[k] = p[k] * handasset::kPosStep;
}

void Skin(bool right, const Bones &b, int i, float out[3]) {
    const handasset::Hand &a = handasset::kHands[right];
    float v[3];
    BindVertex(right, i, v);
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 4; ++k) {
        const float w = a.weights[4 * i + k] / 255.0f;
        if (w <= 0) continue;
        const float *m = b.m[a.joints[4 * i + k]];
        for (int r = 0; r < 3; ++r) out[r] += w * (m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3]);
    }
}

void Points(bool right, const Bones &b, int step, handmesh::Mesh &out) {
    out.clear();
    const int n = Vertices(right);
    for (int i = 0; i < n; i += std::max(1, step)) {
        handmesh::Vertex v{};
        Skin(right, b, i, v.p);
        out.v.push_back(v);
    }
}

handcut::Hand BindHand(bool right) {
    handcut::Hand h;
    h.right = right, h.fit = true, h.stereo = true, h.confidence = 1;
    for (int k = 0; k < 21; ++k)
        for (int i = 0; i < 3; ++i) h.p[k][i] = handasset::kHands[right].bind[kFromMediaPipe[k]][i];
    return h;
}

// ------------------------------------------------------------------------------- demo hands

bool DemoPose(const char *name, bool right, const double at[3], const double palm[3], const double up[3], double t,
              handcut::Hand &out) {
    // A right hand in its own frame: +y along the fingers, +z out of the palm, +x toward the thumb
    // (y x z). Each finger from its knuckle: spread (radians toward the thumb), then three bends
    // toward the palm.
    struct Finger {
        double knuckle[2], spread, len[3];
    };
    static const Finger kFingers[4] = {{{0.022, 0.085}, 0.10, {0.040, 0.025, 0.020}},
                                       {{0.000, 0.088}, 0.00, {0.044, 0.028, 0.022}},
                                       {{-0.020, 0.083}, -0.10, {0.040, 0.026, 0.020}},
                                       {{-0.038, 0.075}, -0.22, {0.030, 0.019, 0.017}}};
    double spread = 0.4, bend[4][3] = {}, thumbBend = 0.1;
    bool pinch = false;
    auto all = [&](double a, double b, double c) {
        for (auto &f : bend) f[0] = a, f[1] = b, f[2] = c;
    };
    if (!std::strcmp(name, "open")) all(0.10, 0.12, 0.08);
    else if (!std::strcmp(name, "spread")) spread = 1.6, all(0, 0.02, 0);
    else if (!std::strcmp(name, "fist")) spread = 0, all(1.45, 1.65, 0.9), thumbBend = 1;
    else if (!std::strcmp(name, "point")) {
        spread = 0, all(1.45, 1.65, 0.9), thumbBend = 0.9;
        bend[0][0] = 0.05, bend[0][1] = 0.05, bend[0][2] = 0.03;
    } else if (!std::strcmp(name, "pinch")) {
        spread = 0.3, all(0.35, 0.55, 0.30), pinch = true;
        bend[0][0] = 0.55, bend[0][1] = 0.75, bend[0][2] = 0.45;
    } else if (!std::strcmp(name, "wave")) {
        const double c = 0.5 + 0.5 * std::sin(t * 1.3);
        all(0.4 * c, 0.6 * c, 0.3 * c), thumbBend = 0.1 + 0.5 * c;
    } else {
        return false;
    }
    double P[21][3] = {};
    for (int f = 0; f < 4; ++f) {
        const Finger &F = kFingers[f];
        const double a = F.spread * (0.3 + spread);
        const double dx = std::sin(a), dy = std::cos(a);
        double *p = P[5 + 4 * f];
        p[0] = F.knuckle[0], p[1] = F.knuckle[1], p[2] = 0;
        double phi = 0;
        for (int k = 0; k < 3; ++k) {
            phi += bend[f][k];
            const double *q = P[5 + 4 * f + k];
            double *n = P[6 + 4 * f + k];
            n[0] = q[0] + F.len[k] * dx * std::cos(phi), n[1] = q[1] + F.len[k] * dy * std::cos(phi), n[2] = q[2] + F.len[k] * std::sin(phi);
        }
    }
    // the thumb: from its base along the palm's edge, out and up; bent, it lies across the
    // fingers; pinching, its tip meets the index tip
    const double cmc[3] = {0.024, 0.022, 0.006};
    std::memcpy(P[1], cmc, sizeof cmc);
    if (pinch) {
        const double *tip = P[8];
        for (int k = 0; k < 3; ++k) {
            const double f = (k + 1) / 3.0;
            for (int i = 0; i < 3; ++i) P[2 + k][i] = cmc[i] + (tip[i] - cmc[i]) * f;
            P[2 + k][0] += 0.016 * std::sin(f * M_PI) * (k < 2), P[2 + k][2] += 0.004 * (k < 2);
        }
        P[4][2] += 0.004;   // just touching
    } else {
        const double open[3][3] = {{0.046, 0.040, 0.012}, {0.060, 0.060, 0.020}, {0.070, 0.080, 0.026}};
        const double bent[3][3] = {{0.040, 0.046, 0.026}, {0.024, 0.060, 0.046}, {0.004, 0.066, 0.054}};
        const double b = std::clamp(thumbBend, 0.0, 1.0);
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 3; ++i) P[2 + k][i] = open[k][i] + (bent[k][i] - open[k][i]) * b;
    }
    // into the room: z along `palm`, y along `up` (square to it), x = y x z; a left hand mirrored
    const V Z = V(palm[0], palm[1], palm[2]).unit();
    V Y = V(up[0], up[1], up[2]);
    Y = (Y - Z * Y.dot(Z)).unit();
    if (Z.norm() < 0.5 || Y.norm() < 0.5) return false;
    const V X = Y.cross(Z), at3(at[0], at[1], at[2]);
    handcut::Hand h;
    h.id = right ? 2 : 1, h.right = right, h.stereo = true, h.fit = true, h.confidence = 1;
    for (int j = 0; j < 21; ++j) {
        const V q = at3 + X * (right ? P[j][0] : -P[j][0]) + Y * P[j][1] + Z * P[j][2];
        h.p[j][0] = float(q.x), h.p[j][1] = float(q.y), h.p[j][2] = float(q.z);
        h.radius[j] = 0.008f;
    }
    for (int b = 0; b < 20; ++b) {
        const int j = b + 1, parent = j % 4 == 1 ? 0 : j - 1;
        h.bone[b] = float((V(h.p[j]) - V(h.p[parent])).norm());
    }
    const V elbow = at3 - Y * 0.26 - Z * 0.04;
    h.elbow[0] = float(elbow.x), h.elbow[1] = float(elbow.y), h.elbow[2] = float(elbow.z);
    h.hasElbow = true, h.elbowConf = 1;
    std::memcpy(h.pinchPoint, h.p[9], sizeof h.pinchPoint);
    std::memcpy(h.contact, h.p[8], sizeof h.contact);
    h.pinchD[0] = float((V(h.p[4]) - V(h.p[8])).norm()), h.pinchD[1] = float((V(h.p[4]) - V(h.p[12])).norm());
    h.v[0] = h.v[1] = h.v[2] = 0;
    out = h;
    return true;
}

}  // namespace handskin
