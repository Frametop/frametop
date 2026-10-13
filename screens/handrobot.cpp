// Rendered hands, the robot (see handrobot.h).
#include "handrobot.h"

#include "handskin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace handrobot {
namespace {

struct V {
    double x, y, z;
    V(double a = 0, double b = 0, double c = 0) : x(a), y(b), z(c) {}
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

// The reference hand (handskin::DemoPose's open right hand): its segments' lengths, its palm's
// size (the wrist to the four knuckles) and span (the index knuckle to the little finger's), and
// its knuckles across the palm (the palm frame's x).
struct Reference {
    double len[kSegs], palm = 0, span = 0, knuckleX[4] = {};
};

const Reference &Ref() {
    static const Reference r = [] {
        Reference r;
        const double at[3] = {0, 0, 0}, palm[3] = {0, 0, 1}, up[3] = {0, 1, 0};
        handcut::Hand h;
        handskin::DemoPose("open", true, at, palm, up, 0, h);
        const V w(h.p[0][0], h.p[0][1], h.p[0][2]);
        auto P = [&](int j) { return V(h.p[j][0], h.p[j][1], h.p[j][2]); };
        for (int k = 0; k < kSegs; ++k) r.len[k] = (P(kTo[k]) - P(kFrom[k])).norm();
        for (int j : {5, 9, 13, 17}) r.palm += (P(j) - w).norm();
        r.span = (P(17) - P(5)).norm();
        float pf[9], bones[20][9];
        handskin::TrackedFrames(h, pf, bones);
        for (int f = 0; f < 4; ++f) r.knuckleX[f] = (P(5 + 4 * f) - w).dot(V(pf[0], pf[1], pf[2]));
        return r;
    }();
    return r;
}

// ------------------------------------------------------------------------------- building it

// A cross-section along a piece's axis: at z (from the bone's start, or from its end if `end`),
// a rounded rectangle about (cx, cy), w and h its half-sizes.
struct Ring {
    double z, cx, cy, w, h;
    int end;
};

// A piece: which segment and material, how many points round, how square its section (2 an
// ellipse, more a rounded rectangle), the section turned by `roll` about its axis, the axis along
// the segment's z or (a hinge) its x, and where it is in the segment's frame.
struct Piece {
    int seg, mat, sides;
    double sq = 3, roll = 0;
    bool alongX = false;
    V at;
};

// A point of the section at angle th, in the section's plane.
void Section(const Ring &r, double th, double sq, double &u, double &v) {
    const double c = std::cos(th), s = std::sin(th), e = 2 / sq;
    u = r.cx + r.w * std::copysign(std::pow(std::fabs(c), e), c);
    v = r.cy + r.h * std::copysign(std::pow(std::fabs(s), e), s);
}

// The section's (u, v) at z, into the segment's frame; `ref` adds the reference bone's length
// to an end-fixed point (where it is on the reference hand: normals, bands).
V Local(const Piece &p, double u, double v, double z) {
    const double c = std::cos(p.roll), s = std::sin(p.roll);
    const double a = u * c - v * s, b = u * s + v * c;
    return p.at + (p.alongX ? V(z, b, a) : V(a, b, z));
}
V RefPos(const Piece &p, const V &local, int end) { return local + V(0, 0, end ? Ref().len[p.seg] : 0); }

void Add(Mesh &m, const Piece &p, const V &pos, const V &n, int end, int mat) {
    Vertex x;
    x.p[0] = float(pos.x), x.p[1] = float(pos.y), x.p[2] = float(pos.z);
    x.n[0] = float(n.x), x.n[1] = float(n.y), x.n[2] = float(n.z);
    x.seg = float(p.seg), x.end = float(end), x.mat = float(mat);
    m.v.push_back(x);
}

// Smooth normals for the vertices from `first` on (their triangles from `firstIdx`), from the
// triangles on the reference hand, turned outward: away from `centre` of each (the axis).
void Normals(Mesh &m, size_t first, size_t firstIdx, const std::vector<V> &centre) {
    std::vector<V> ref(m.v.size() - first), acc(ref.size());
    for (size_t i = first; i < m.v.size(); ++i) {
        const Vertex &x = m.v[i];
        ref[i - first] = V(x.p[0], x.p[1], x.p[2] + (x.end > 0.5f ? Ref().len[int(x.seg)] : 0));
    }
    for (size_t t = firstIdx; t + 2 < m.idx.size(); t += 3) {
        const size_t a = m.idx[t] - first, b = m.idx[t + 1] - first, c = m.idx[t + 2] - first;
        const V n = (ref[b] - ref[a]).cross(ref[c] - ref[a]);   // area-weighted
        acc[a] = acc[a] + n, acc[b] = acc[b] + n, acc[c] = acc[c] + n;
    }
    double out = 0;
    for (size_t i = 0; i < ref.size(); ++i) out += acc[i].unit().dot((ref[i] - centre[i]).unit());
    const double flip = out < 0 ? -1 : 1;
    if (flip < 0)
        for (size_t t = firstIdx; t + 2 < m.idx.size(); t += 3) std::swap(m.idx[t + 1], m.idx[t + 2]);
    for (size_t i = 0; i < ref.size(); ++i) {
        const V n = acc[i].unit() * flip;
        Vertex &x = m.v[first + i];
        x.n[0] = float(n.x), x.n[1] = float(n.y), x.n[2] = float(n.z);
    }
}

// A closed piece through `rings`, each end capped by a fan to a point `tip0` before the first
// ring (along the axis) and `tip1` past the last (0: a flat cap).
void Loft(Mesh &m, const Piece &p, const std::vector<Ring> &rings, double tip0 = 0, double tip1 = 0) {
    const size_t first = m.v.size(), firstIdx = m.idx.size();
    const int n = p.sides;
    std::vector<V> centre;
    for (const Ring &r : rings)
        for (int j = 0; j < n; ++j) {
            double u, v;
            Section(r, 2 * M_PI * j / n, p.sq, u, v);
            Add(m, p, Local(p, u, v, r.z), V(), r.end, p.mat);
            centre.push_back(RefPos(p, Local(p, r.cx, r.cy, r.z), r.end));
        }
    const Ring &a = rings.front(), &b = rings.back();
    const size_t c0 = m.v.size();
    Add(m, p, Local(p, a.cx, a.cy, a.z - tip0), V(), a.end, p.mat);
    centre.push_back(RefPos(p, Local(p, a.cx, a.cy, a.z + 1), a.end));   // outward: back along the axis
    const size_t c1 = m.v.size();
    Add(m, p, Local(p, b.cx, b.cy, b.z + tip1), V(), b.end, p.mat);
    centre.push_back(RefPos(p, Local(p, b.cx, b.cy, b.z - 1), b.end));
    auto at = [&](size_t i, int j) { return uint16_t(first + i * n + (j % n)); };
    for (size_t i = 0; i + 1 < rings.size(); ++i)
        for (int j = 0; j < n; ++j) {
            const uint16_t q[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
            m.idx.insert(m.idx.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
        }
    const size_t last = rings.size() - 1;
    for (int j = 0; j < n; ++j) {
        m.idx.insert(m.idx.end(), {uint16_t(c0), at(0, j), at(0, j + 1)});
        m.idx.insert(m.idx.end(), {uint16_t(c1), at(last, j + 1), at(last, j)});
    }
    Normals(m, first, firstIdx, centre);
}

// The section of a loft at reference z (between its rings).
Ring At(const Piece &p, const std::vector<Ring> &rings, double z) {
    auto zr = [&](const Ring &r) { return r.z + (r.end ? Ref().len[p.seg] : 0); };
    size_t i = 0;
    while (i + 2 < rings.size() && zr(rings[i + 1]) < z) ++i;
    const Ring &a = rings[i], &b = rings[i + 1];
    const double t = std::clamp((z - zr(a)) / std::max(zr(b) - zr(a), 1e-9), 0.0, 1.0);
    Ring r;
    r.z = z, r.end = 0;
    r.cx = a.cx + (b.cx - a.cx) * t, r.cy = a.cy + (b.cy - a.cy) * t;
    r.w = a.w + (b.w - a.w) * t, r.h = a.h + (b.h - a.h) * t;
    return r;
}

// A strip on a loft's surface (a panel line, a glowing pad): from reference z0 to z1, angles th0
// to th1 round its section, lifted `lift` off it. Fixed to the bone's end if it's in the far half.
void Band(Mesh &m, const Piece &p, const std::vector<Ring> &rings, double z0, double z1, double th0, double th1, int steps,
          double lift, int mat) {
    const size_t first = m.v.size(), firstIdx = m.idx.size();
    const int end = (z0 + z1) / 2 > Ref().len[p.seg] / 2;
    const double shift = end ? Ref().len[p.seg] : 0;
    std::vector<V> centre;
    for (double z : {z0, z1}) {
        const Ring r = At(p, rings, z);
        for (int j = 0; j <= steps; ++j) {
            const double th = th0 + (th1 - th0) * j / steps;
            double u, v, u0, v0, u1, v1;
            Section(r, th, p.sq, u, v);
            Section(r, th - 0.01, p.sq, u0, v0), Section(r, th + 0.01, p.sq, u1, v1);
            // the section's outward normal there (its tangent turned a quarter)
            double nu = v1 - v0, nv = -(u1 - u0);
            const double l = std::hypot(nu, nv);
            nu /= l, nv /= l;
            if (nu * (u - r.cx) + nv * (v - r.cy) < 0) nu = -nu, nv = -nv;
            Add(m, p, Local(p, u + nu * lift, v + nv * lift, z - shift), V(), end, mat);
            centre.push_back(RefPos(p, Local(p, r.cx, r.cy, z - shift), end));
        }
    }
    const int row = steps + 1;
    for (int j = 0; j < steps; ++j) {
        const uint16_t a = uint16_t(first + j), b = uint16_t(first + j + 1), c = uint16_t(first + row + j + 1),
                       d = uint16_t(first + row + j);
        m.idx.insert(m.idx.end(), {a, b, c, a, c, d});
    }
    Normals(m, first, firstIdx, centre);
}

// A flat disc of radius r facing +y in the segment's frame at `c` (the palm's glowing dot).
void Disc(Mesh &m, int seg, const V &c, double r, int sides, int mat) {
    Piece p;
    p.seg = seg, p.mat = mat, p.sides = sides;
    const size_t first = m.v.size();
    Add(m, p, c, V(0, 1, 0), 0, mat);
    for (int j = 0; j < sides; ++j) {
        const double a = 2 * M_PI * j / sides;
        Add(m, p, c + V(r * std::cos(a), 0, r * std::sin(a)), V(0, 1, 0), 0, mat);
    }
    for (int j = 0; j < sides; ++j)
        m.idx.insert(m.idx.end(), {uint16_t(first), uint16_t(first + 1 + j), uint16_t(first + 1 + (j + 1) % sides)});
}

// A ball joint at the segment's start, radius r.
void Ball(Mesh &m, int seg, double r) {
    Piece p;
    p.seg = seg, p.mat = kJoint, p.sides = 10, p.sq = 2;
    std::vector<Ring> rings;
    for (double deg : {35.0, 72.0, 108.0, 145.0}) {
        const double a = deg * M_PI / 180;
        rings.push_back({-r * std::cos(a), 0, 0, r * std::sin(a), r * std::sin(a), 0});
    }
    Loft(m, p, rings, r - r * std::cos(35 * M_PI / 180), r - r * std::cos(35 * M_PI / 180));
}

// A hinge at the segment's start: a short cylinder along its x (the axis it bends about),
// radius r, half as long as hl.
void Hinge(Mesh &m, int seg, double r, double hl) {
    Piece p;
    p.seg = seg, p.mat = kJoint, p.sides = 10, p.sq = 2, p.alongX = true;
    const double e = std::min(0.0008, hl / 4);
    Loft(m, p, {{-hl, 0, 0, 0.8 * r, 0.8 * r, 0}, {-hl + e, 0, 0, r, r, 0}, {hl - e, 0, 0, r, r, 0}, {hl, 0, 0, 0.8 * r, 0.8 * r, 0}});
}

// A bone's shell: half-sizes (w0, h0) at its start to (w1, h1) at its end, starting gap0 past
// the start joint and ending gap1 before the end one, its ends rounded; a fingertip's (tip)
// domed and ending at the tracked tip, with a glowing pad. `roll`: where its pad side faces.
// `line`: a dark line across its back near its end.
void Shell(Mesh &m, int seg, double w0, double h0, double w1, double h1, double gap0, double gap1, bool tip, double roll,
           bool line) {
    Piece p;
    p.seg = seg, p.mat = kShell, p.sides = 12, p.sq = 3.2, p.roll = roll;
    const double b0 = 0.45 * h0, b1 = 0.45 * h1;
    std::vector<Ring> r = {{gap0, 0, 0, 0.55 * w0, 0.55 * h0, 0}, {gap0 + 0.3 * b0, 0, 0, 0.9 * w0, 0.88 * h0, 0}, {gap0 + b0, 0, 0, w0, h0, 0}};
    if (tip) {
        r.push_back({-0.0080, 0, 0, w1, h1, 1});
        r.push_back({-0.0034, 0, 0.0004, 0.86 * w1, 0.78 * h1, 1});
        r.push_back({-0.0009, 0, 0.0007, 0.50 * w1, 0.40 * h1, 1});
        Loft(m, p, r, 0, 0.0009);
    } else {
        r.push_back({-gap1 - b1, 0, 0, w1, h1, 1});
        r.push_back({-gap1 - 0.3 * b1, 0, 0, 0.9 * w1, 0.88 * h1, 1});
        r.push_back({-gap1, 0, 0, 0.55 * w1, 0.55 * h1, 1});
        Loft(m, p, r);
    }
    const double L = Ref().len[seg];
    if (tip) Band(m, p, r, L - 0.0105, L - 0.0042, M_PI / 2 - 0.75, M_PI / 2 + 0.75, 4, 0.00035, kAccent);
    if (line) {
        const double z = L - gap1 - b1 - 0.004;
        Band(m, p, r, z, z + 0.0011, -M_PI / 2 - 0.8, -M_PI / 2 + 0.8, 4, 0.00025, kLine);
    }
}

// The palm (segment 0, the palm's frame: x across from the index knuckle, y out of the palm, z
// from the wrist toward the middle knuckle): a dark core with a light plate on each face, and the
// cuff past the wrist with a dark neck between.
void Palm(Mesh &m) {
    const Reference &R = Ref();
    const double L = R.len[0];
    const double kc = (R.knuckleX[0] + R.knuckleX[3]) / 2, kw = (R.knuckleX[3] - R.knuckleX[0]) / 2 + 0.0085;
    const double c0 = 0.3 * kc;   // the wrist's middle: nearer the bone line
    Piece core;
    core.seg = 0, core.mat = kJoint, core.sides = 16, core.sq = 3;
    const std::vector<Ring> coreRings = {{-0.006, c0, 0, 0.022, 0.0090, 0}, {0.003, c0, 0, 0.027, 0.0110, 0},
                                         {0.030, kc, 0, kw, 0.0120, 0},     {-0.030, kc, 0, kw, 0.0118, 1},
                                         {-0.008, kc, 0, 0.98 * kw, 0.0105, 1}, {-0.002, kc, 0, 0.90 * kw, 0.0080, 1}};
    Loft(m, core, coreRings);
    // the plates: a little narrower than the core, so it shows as a dark seam round the sides
    Piece plate = core;
    plate.mat = kShell, plate.sq = 3.6;
    auto plateRings = [&](double cy, double scale, double z0, double z1) {
        std::vector<Ring> r;
        const double h = 0.0056;
        r.push_back({z0, c0, cy, 0.84 * 0.026 * scale, 0.6 * h, 0});
        r.push_back({z0 + 0.002, c0, cy, 0.026 * scale, h, 0});
        r.push_back({0.030, kc, cy, kw * scale, h, 0});
        r.push_back({-0.030, kc, cy, kw * scale, h, 1});
        r.push_back({z1 - 0.002, kc, cy, 0.97 * kw * scale, h, 1});
        r.push_back({z1, kc, cy, 0.86 * kw * scale, 0.6 * h, 1});
        return r;
    };
    const std::vector<Ring> palmRings = plateRings(0.0068, 0.92, 0.0, -0.011), backRings = plateRings(-0.0068, 0.95, -0.003, -0.004);
    Loft(m, plate, palmRings);
    Loft(m, plate, backRings);
    // the palm plate: a panel line across its lower half, a glowing dot in its middle
    Band(m, plate, palmRings, 0.024, 0.0252, M_PI / 2 - 1.2, M_PI / 2 + 1.2, 8, 0.00025, kLine);
    Disc(m, 0, V(kc * 0.8, 0.0068 + 0.0056 + 0.0003, 0.044), 0.0050, 12, kAccent);
    // the back plate: a panel line, and a glowing line across the knuckles
    Band(m, plate, backRings, 0.034, 0.0352, -M_PI / 2 - 1.2, -M_PI / 2 + 1.2, 8, 0.00025, kLine);
    Band(m, plate, backRings, L - 0.0165, L - 0.0140, -M_PI / 2 - 1.15, -M_PI / 2 + 1.15, 8, 0.0003, kAccent);
    // the neck and the cuff, closed at both ends, a glowing ring round it
    Piece neck = core;
    neck.sides = 12, neck.sq = 2.4;
    Loft(m, neck, {{-0.014, c0, 0, 0.019, 0.0125, 0}, {0.002, c0, 0, 0.021, 0.0135, 0}});
    Piece cuff = core;
    cuff.mat = kShell, cuff.sq = 2.6;
    const std::vector<Ring> cuffRings = {{-0.0370, c0, 0, 0.62 * 0.0265, 0.62 * 0.0185, 0}, {-0.0355, c0, 0, 0.92 * 0.0265, 0.9 * 0.0185, 0},
                                         {-0.0330, c0, 0, 0.0265, 0.0185, 0},              {-0.0130, c0, 0, 0.0252, 0.0172, 0},
                                         {-0.0105, c0, 0, 0.92 * 0.0252, 0.9 * 0.0172, 0}, {-0.0090, c0, 0, 0.62 * 0.0252, 0.62 * 0.0172, 0}};
    Loft(m, cuff, cuffRings);
    // a dark plate in its far end, so it reads as an end seen end on
    Piece plug = core;
    plug.sq = 2.6;
    Loft(m, plug, {{-0.0378, c0, 0, 0.50 * 0.0265, 0.50 * 0.0185, 0}, {-0.0360, c0, 0, 0.56 * 0.0265, 0.56 * 0.0185, 0}});
    Band(m, cuff, cuffRings, -0.0235, -0.0215, 0, 2 * M_PI, 16, 0.0003, kAccent);
    Band(m, cuff, cuffRings, -0.0300, -0.0290, -M_PI / 2 - 1.0, -M_PI / 2 + 1.0, 6, 0.00025, kLine);
}

Mesh Build() {
    Mesh m;
    Palm(m);
    // the thumb: a ball at its base, hinges after; its pad faces across the palm (its frame's -y)
    {
        const double w[4] = {0.0118, 0.0104, 0.0092, 0.0078}, mid = 0.0100;
        const double ballR = 0.0108, mcpR = 0.80 * 0.85 * mid, ipR = 0.80 * 0.85 * 0.0090;
        Ball(m, 1, ballR);
        Shell(m, 1, w[0], 0.85 * w[0], w[1], 0.85 * w[1], 0.55 * ballR, 0.5 * mcpR, false, M_PI, true);
        Hinge(m, 2, mcpR, 1.08 * w[1]);
        Shell(m, 2, mid, 0.85 * mid, w[2], 0.85 * w[2], 0.5 * mcpR, 0.5 * ipR, false, M_PI, false);
        Hinge(m, 3, ipR, 1.08 * w[2]);
        Shell(m, 3, 0.0090, 0.85 * 0.0090, w[3], 0.85 * w[3], 0.5 * ipR, 0, true, M_PI, false);
    }
    // the fingers: a ball at the knuckle, hinges at the middle and end joints
    const double kWidth[4] = {0.0088, 0.0091, 0.0084, 0.0074};
    for (int f = 0; f < 4; ++f) {
        const double fw = kWidth[f], hr = 0.84;
        const int s = 4 + 3 * f;
        const double w[6] = {fw, 0.92 * fw, 0.88 * fw, 0.82 * fw, 0.79 * fw, 0.70 * fw};
        const double ballR = 1.0 * fw, pipR = 0.80 * hr * w[2], dipR = 0.80 * hr * w[4];
        Ball(m, s, ballR);
        Shell(m, s, w[0], hr * w[0], w[1], hr * w[1], 0.55 * ballR, 0.5 * pipR, false, 0, true);
        Hinge(m, s + 1, pipR, 1.08 * w[1]);
        Shell(m, s + 1, w[2], hr * w[2], w[3], hr * w[3], 0.5 * pipR, 0.5 * dipR, false, 0, false);
        Hinge(m, s + 2, dipR, 1.08 * w[3]);
        Shell(m, s + 2, w[4], hr * w[4], w[5], hr * w[5], 0.5 * dipR, 0, true, 0, false);
    }
    return m;
}

}  // namespace

const Mesh &Model() {
    static const Mesh m = Build();
    return m;
}

float ReferenceLength(int k) { return float(Ref().len[k]); }

bool Pose(const handcut::Hand &h, Bones &out) {
    for (int k = 0; k < 21; ++k)
        if (!std::isfinite(h.p[k][0]) || !std::isfinite(h.p[k][1]) || !std::isfinite(h.p[k][2])) return false;
    // a left hand's frames: a right hand's on its mirror image, mirrored back (left-handed, so the
    // right hand's mesh comes out mirrored)
    handcut::Hand m = h;
    if (!h.right)
        for (auto &p : m.p) p[0] = -p[0];
    float pf[9], bones[20][9];
    if (!handskin::TrackedFrames(m, pf, bones)) return false;
    if (!h.right) {
        for (int i = 0; i < 9; i += 3) pf[i] = -pf[i];
        for (auto &b : bones)
            for (int i = 0; i < 9; i += 3) b[i] = -b[i];
    }
    const Reference &R = Ref();
    auto P = [&](int j) { return V(h.p[j][0], h.p[j][1], h.p[j][2]); };
    double palm = 0;
    for (int j : {5, 9, 13, 17}) palm += (P(j) - P(0)).norm();
    const double s = std::clamp(palm / R.palm, 0.7, 1.4);
    const double span = std::clamp((P(17) - P(5)).norm() / (s * R.span), 0.85, 1.2);
    for (int k = 0; k < kSegs; ++k) {
        const float *f = k == 0 ? pf : bones[kTo[k] - 1];
        const V o = P(kFrom[k]);
        float *m3 = out.seg[k];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) m3[r * 4 + c] = float(s * f[3 * c + r]);
            m3[r * 4 + 3] = float(r == 0 ? o.x : r == 1 ? o.y : o.z);
        }
        // the bone's length, in the reference hand's units; no shorter than half the reference's
        // (a shell's rounded ends would cross)
        const double len = (P(kTo[k]) - o).norm() / s;
        m3[12] = float(std::max(len, 0.5 * R.len[k]));
        m3[13] = float(k == 0 ? span : 1.0), m3[14] = 1, m3[15] = 0;
    }
    out.scale = float(s);
    out.right = h.right;
    return true;
}

void Place(const Bones &b, const Vertex &v, float out[3]) {
    const float *m = b.seg[int(v.seg)];
    const float l[3] = {v.p[0] * m[13], v.p[1] * m[14], v.p[2] + v.end * m[12]};
    for (int r = 0; r < 3; ++r) out[r] = m[r * 4] * l[0] + m[r * 4 + 1] * l[1] + m[r * 4 + 2] * l[2] + m[r * 4 + 3];
}

void Points(const Bones &b, int step, handmesh::Mesh &out) {
    out.clear();
    const Mesh &m = Model();
    for (size_t i = 0; i < m.v.size(); i += size_t(std::max(1, step))) {
        handmesh::Vertex v{};
        Place(b, m.v[i], v.p);
        out.v.push_back(v);
    }
}

}  // namespace handrobot
