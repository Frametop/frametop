// Rendered hands, the CPU side (see handmesh.h).
#include "handmesh.h"

#include <algorithm>
#include <cmath>

namespace handmesh {
namespace {

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

// Any unit vector square to d.
V Square(const V &d) {
    const V a = std::fabs(d.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
    return d.cross(a).unit();
}

void Push(Mesh &m, const V &p, const V &n) {
    m.v.push_back({{float(p.x), float(p.y), float(p.z)}, {float(n.x), float(n.y), float(n.z)}, 0});
}

constexpr int kAround = 10;   // a capsule's segments around its axis
constexpr int kCap = 3;       // ...and along each round end
constexpr float kFaded = 0.11f;   // metres behind the wrist: the forearm has faded out by then (handview.cpp)

}  // namespace

void AddCapsule(const handcut::Capsule &c, Mesh &out) {
    const V a(c.a), b(c.b);
    V d = b - a;
    if (d.norm() < 1e-6) d = V(0, 1, 0);
    d = d.unit();
    const V u = Square(d), w = d.cross(u);
    const size_t base = out.v.size();
    if (base + (2 * kCap + 2) * kAround > 65535) return;
    static double around[kAround][2], along[2 * kCap + 2][2];   // cos, sin
    static const bool once = [] {
        for (int i = 0; i < kAround; ++i) around[i][0] = std::cos(i * 2 * M_PI / kAround), around[i][1] = std::sin(i * 2 * M_PI / kAround);
        for (int r = 0; r < 2 * kCap + 2; ++r) {
            const double th = (r <= kCap ? r - kCap : r - kCap - 1) * (M_PI / 2) / kCap;
            along[r][0] = std::cos(th), along[r][1] = std::sin(th);
        }
        return true;
    }();
    (void)once;
    // rings from a's pole to b's: a's round end (-90..0 degrees), then b's (0..90)
    const int rings = 2 * kCap + 2;
    for (int k = 0; k < rings; ++k) {
        const bool end = k > kCap;
        const V centre = end ? b : a;
        const double r = end ? c.rb : c.ra;
        for (int i = 0; i < kAround; ++i) {
            const V n = d * along[k][1] + (u * around[i][0] + w * around[i][1]) * along[k][0];
            Push(out, centre + n * r, n);
        }
    }
    for (int r = 0; r + 1 < rings; ++r)
        for (int i = 0; i < kAround; ++i) {
            const int j = (i + 1) % kAround;
            const uint16_t p0 = uint16_t(base + r * kAround + i), p1 = uint16_t(base + r * kAround + j);
            const uint16_t q0 = uint16_t(p0 + kAround), q1 = uint16_t(p1 + kAround);
            out.idx.insert(out.idx.end(), {p0, p1, q1, p0, q1, q0});
        }
}

void AddHull(const std::vector<std::array<float, 3>> &pts, Mesh &out) {
    const size_t n = pts.size();
    if (n < 4) return;
    V mid;
    for (const auto &p : pts) mid = mid + V(p.data()) * (1.0 / n);
    // every triangle with all the points on one side of it is on the hull (n is at most 16:
    // a few thousand dot products); a face of more than 3 points comes out as overlapping
    // triangles in its plane, which draw the same
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            for (size_t k = j + 1; k < n; ++k) {
                const V a(pts[i].data()), b(pts[j].data()), c(pts[k].data());
                V nrm = (b - a).cross(c - a);
                const double len = nrm.norm();
                if (len < 1e-9) continue;
                nrm = nrm * (1 / len);
                bool above = false, below = false;
                for (size_t q = 0; q < n && !(above && below); ++q) {
                    const double s = (V(pts[q].data()) - a).dot(nrm);
                    above |= s > 1e-5, below |= s < -1e-5;
                }
                if (above && below) continue;
                if ((mid - a).dot(nrm) > 0) nrm = nrm * -1;   // outward
                if (out.v.size() + 3 > 65535) return;
                const uint16_t at = uint16_t(out.v.size());
                Push(out, a, nrm), Push(out, b, nrm), Push(out, c, nrm);
                out.idx.insert(out.idx.end(), {at, uint16_t(at + 1), uint16_t(at + 2)});
            }
}

void Build(const handcut::Hand &h, const double headPos[3], Mesh &out) {
    out.clear();
    handcut::MatteParams p;
    p.shape = handcut::MatteShape::Mesh;
    p.marginDeg = p.motionMaxDeg = p.minMarginM = 0;
    handcut::Matte matte;
    handcut::BuildMatte({h}, {}, headPos, p, matte);
    // the palm's pieces fill the gaps between the fingers' first bones and the knuckles: a
    // capsule from the wrist to each knuckle, under the palm's slab
    const float *w = h.p[0];
    for (int j : {5, 9, 13, 17}) {
        handcut::Capsule c{{w[0], w[1], w[2]}, {h.p[j][0], h.p[j][1], h.p[j][2]}, 0.012f, 0.0095f};
        AddCapsule(c, out);
    }
    for (const handcut::Capsule &c : matte.caps) AddCapsule(c, out);
    for (const handcut::Hull &hull : matte.hulls) AddHull(hull.v, out);
    // how far behind the wrist each point is, along the hand; the forearm's stub ends just past
    // where it has faded out (the matte's goes on to 18 cm), so its square isn't bigger than that
    const V wrist(h.p[0]), fwd = (V(h.p[9]) - wrist).unit();
    const double f[3] = {fwd.x, fwd.y, fwd.z};
    for (Vertex &v : out.v) {
        v.back = float(-(V(v.p) - wrist).dot(fwd));
        if (v.back <= kFaded) continue;
        for (int i = 0; i < 3; ++i) v.p[i] += float(f[i] * (v.back - kFaded));
        v.back = kFaded;
    }
}

bool Place(const Mesh &m, const double eyes[2][3], double margin, Quad &out) {
    if (m.v.empty()) return false;
    const V e0(eyes[0][0], eyes[0][1], eyes[0][2]), e1(eyes[1][0], eyes[1][1], eyes[1][2]);
    const V head = (e0 + e1) * 0.5;
    V lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
    for (const Vertex &v : m.v) {
        lo = V(std::min(lo.x, double(v.p[0])), std::min(lo.y, double(v.p[1])), std::min(lo.z, double(v.p[2])));
        hi = V(std::max(hi.x, double(v.p[0])), std::max(hi.y, double(v.p[1])), std::max(hi.z, double(v.p[2])));
    }
    const V c = (lo + hi) * 0.5;
    const double dist = (head - c).norm();
    if (dist < 0.08) return false;
    const V z = (head - c) * (1 / dist);
    V y = V(0, 1, 0) - z * z.y;
    if (y.norm() < 1e-3) y = Square(z);
    y = y.unit();
    const V x = y.cross(z);
    // what each eye sees of the mesh, on the square's plane
    double half = 0;
    for (const V &e : {e0, e1}) {
        const double ez = (e - c).dot(z);
        if (ez < 0.08) return false;
        for (const Vertex &v : m.v) {
            const V d = V(v.p) - e;
            const double dz = d.dot(z);        // negative: away from the eye, toward the square
            if (dz > -0.04) continue;          // at or behind the eye: clipped when drawn
            const V q = e + d * (-ez / dz) - c;
            half = std::max({half, std::fabs(q.dot(x)), std::fabs(q.dot(y))});
        }
    }
    if (half <= 0) return false;
    half = std::min(half * (1 + 2 * margin), 0.6);
    handcut::Mat &P = out.pose;
    const V cols[3] = {x, y, z};
    for (int col = 0; col < 3; ++col) {
        P.m[0][col] = float(cols[col].x), P.m[1][col] = float(cols[col].y), P.m[2][col] = float(cols[col].z);
    }
    P.m[0][3] = float(c.x), P.m[1][3] = float(c.y), P.m[2][3] = float(c.z);
    out.size = 2 * half;
    out.distance = dist;
    return true;
}

void EyeMatrix(const Quad &q, const double eye[3], double nearM, double farM, float out[16]) {
    const handcut::Mat &P = q.pose;
    // room -> the square's frame: R^T (p - t)
    double R[3][4];
    for (int r = 0; r < 3; ++r) {
        double t = 0;
        for (int i = 0; i < 3; ++i) R[r][i] = P.m[i][r], t += P.m[i][r] * P.m[i][3];
        R[r][3] = -t;
    }
    double E[3];   // the eye in the square's frame
    for (int r = 0; r < 3; ++r) E[r] = R[r][0] * eye[0] + R[r][1] * eye[1] + R[r][2] * eye[2] + R[r][3];
    // the view from the eye, V = local - E; what it meets on the square, as clip coordinates:
    //   x = (ez Vx - ex Vz) / h,  y = -(ez Vy - ey Vz) / h (the top first),  w = -Vz
    //   z = A w + B (depth from nearM to farM along the view)
    const double h = q.size / 2, ex = E[0], ey = E[1], ez = E[2];
    const double A = (farM + nearM) / (farM - nearM), B = -2 * farM * nearM / (farM - nearM);
    const double K[4][4] = {{ez / h, 0, -ex / h, 0}, {0, -ez / h, ey / h, 0}, {0, 0, -A, B}, {0, 0, -1, 0}};
    // K * (local - E): fold -E into the translation, then K * [R | t - E]
    double L[4][4] = {};
    for (int r = 0; r < 3; ++r) {
        for (int i = 0; i < 3; ++i) L[r][i] = R[r][i];
        L[r][3] = R[r][3] - E[r];
    }
    L[3][3] = 1;
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += K[r][k] * L[k][col];
            out[col * 4 + r] = float(s);
        }
}

}  // namespace handmesh
