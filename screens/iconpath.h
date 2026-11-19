// Small filled icons from SVG path data, on the 24-unit viewBox the Option C design draws them on
// (user, 2026-11-19: "Use that as the design"), into an RGBA buffer with soft edges. Pure: no OpenVR
// (its tests are in screens/tests/menulayout-test.cpp).
//   Path data: M L H V C S Q T A Z, absolute and relative; curves and arcs are cut into short lines.
// Outlines fill by the nonzero rule, or even-odd for a shape that asks (a frame with a hole).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace iconpath {

struct Pt { double x, y; };
using Outline = std::vector<Pt>;   // closed

struct Shape {
    std::vector<Outline> outlines;
    bool evenOdd = false;
    double opacity = 1;
};

namespace detail {

inline void Skip(const char *&p) {
    while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
}
inline bool Number(const char *&p, double *out) {
    Skip(p);
    char *end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    *out = v, p = end;
    return true;
}
// An arc's flag: one digit, which path data may run straight into the next number ("011-7.4").
inline bool Flag(const char *&p, bool *out) {
    Skip(p);
    if (*p != '0' && *p != '1') return false;
    *out = *p++ == '1';
    return true;
}
constexpr int kSteps = 16;   // lines per curve
inline void Cubic(Outline &o, Pt a, Pt b, Pt c, Pt d) {
    for (int i = 1; i <= kSteps; ++i) {
        const double t = double(i) / kSteps, u = 1 - t;
        o.push_back({u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                     u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y});
    }
}
inline void Quad(Outline &o, Pt a, Pt b, Pt c) {
    for (int i = 1; i <= kSteps; ++i) {
        const double t = double(i) / kSteps, u = 1 - t;
        o.push_back({u * u * a.x + 2 * u * t * b.x + t * t * c.x, u * u * a.y + 2 * u * t * b.y + t * t * c.y});
    }
}
// SVG's endpoint arc (its implementation notes, F.6.5): from a to b on an ellipse rx, ry turned phi degrees.
inline void Arc(Outline &o, Pt a, double rx, double ry, double phiDeg, bool large, bool sweep, Pt b) {
    if (a.x == b.x && a.y == b.y) return;   // (SVG: an arc to where it is draws nothing)
    if (rx == 0 || ry == 0) return o.push_back(b);
    rx = std::fabs(rx), ry = std::fabs(ry);
    const double phi = phiDeg * M_PI / 180, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (a.x - b.x) / 2, dy = (a.y - b.y) / 2;
    const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1) rx *= std::sqrt(lambda), ry *= std::sqrt(lambda);
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    const double k = (large != sweep ? 1 : -1) * std::sqrt(std::max(0.0, den > 0 ? num / den : 0));
    const double cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx;
    const double cx = cp * cxp - sp * cyp + (a.x + b.x) / 2, cy = sp * cxp + cp * cyp + (a.y + b.y) / 2;
    const double t1 = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
    double dt = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx) - t1;
    if (sweep && dt < 0) dt += 2 * M_PI;
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    if (!std::isfinite(dt) || !std::isfinite(t1)) return o.push_back(b);   // (a NaN or infinity in the data: a line)
    const int n = std::max(2, int(std::ceil(std::fabs(dt) / (M_PI / 16))));
    for (int i = 1; i <= n; ++i) {
        const double t = t1 + dt * i / n, ex = rx * std::cos(t), ey = ry * std::sin(t);
        o.push_back({cp * ex - sp * ey + cx, sp * ex + cp * ey + cy});
    }
}

}  // namespace detail

// A path's outlines. Bad data stops the parse where it went wrong (what came before stays).
inline std::vector<Outline> Parse(const char *d) {
    using namespace detail;
    std::vector<Outline> out;
    Outline cur;
    Pt at{0, 0}, start{0, 0}, ctrl{0, 0};
    char cmd = 0, last = 0;
    auto close = [&] {
        if (cur.size() > 2) out.push_back(cur);
        cur.clear();
    };
    const char *p = d;
    for (;;) {
        Skip(p);
        if (!*p) break;
        if (std::isalpha((unsigned char)*p)) cmd = *p++;
        else if (!cmd) break;
        const bool rel = std::islower((unsigned char)cmd);
        const Pt o = rel ? at : Pt{0, 0};
        double v[6];
        auto nums = [&](int n) {
            for (int i = 0; i < n; ++i)
                if (!Number(p, &v[i])) return false;
            return true;
        };
        switch (std::toupper((unsigned char)cmd)) {
            case 'M':
                if (!nums(2)) return close(), out;
                close();
                at = start = {o.x + v[0], o.y + v[1]};
                cur.push_back(at);
                cmd = rel ? 'l' : 'L';   // (more pairs after a move are lines)
                break;
            case 'L':
                if (!nums(2)) return close(), out;
                at = {o.x + v[0], o.y + v[1]}, cur.push_back(at);
                break;
            case 'H':
                if (!nums(1)) return close(), out;
                at.x = o.x + v[0], cur.push_back(at);
                break;
            case 'V':
                if (!nums(1)) return close(), out;
                at.y = (rel ? at.y : 0) + v[0], cur.push_back(at);
                break;
            case 'C': {
                if (!nums(6)) return close(), out;
                const Pt b{o.x + v[0], o.y + v[1]}, c{o.x + v[2], o.y + v[3]}, e{o.x + v[4], o.y + v[5]};
                Cubic(cur, at, b, c, e);
                ctrl = c, at = e;
                break;
            }
            case 'S': {
                if (!nums(4)) return close(), out;
                const bool chain = last == 'C' || last == 'S';
                const Pt b = chain ? Pt{2 * at.x - ctrl.x, 2 * at.y - ctrl.y} : at;
                const Pt c{o.x + v[0], o.y + v[1]}, e{o.x + v[2], o.y + v[3]};
                Cubic(cur, at, b, c, e);
                ctrl = c, at = e;
                break;
            }
            case 'Q': {
                if (!nums(4)) return close(), out;
                const Pt b{o.x + v[0], o.y + v[1]}, e{o.x + v[2], o.y + v[3]};
                Quad(cur, at, b, e);
                ctrl = b, at = e;
                break;
            }
            case 'T': {
                if (!nums(2)) return close(), out;
                const bool chain = last == 'Q' || last == 'T';
                const Pt b = chain ? Pt{2 * at.x - ctrl.x, 2 * at.y - ctrl.y} : at, e{o.x + v[0], o.y + v[1]};
                Quad(cur, at, b, e);
                ctrl = b, at = e;
                break;
            }
            case 'A': {
                bool large = false, sweep = false;
                if (!nums(3) || !Flag(p, &large) || !Flag(p, &sweep) || !Number(p, &v[3]) || !Number(p, &v[4])) return close(), out;
                const Pt e{o.x + v[3], o.y + v[4]};
                Arc(cur, at, v[0], v[1], v[2], large, sweep, e);
                at = e;
                break;
            }
            case 'Z':
                close();
                at = start;
                cur.push_back(start);   // (what's drawn next without an M starts where this one did)
                cmd = 0;                // (Z takes no numbers: one after it ends the data, as an error)
                break;
            default: return close(), out;
        }
        last = char(std::toupper((unsigned char)cmd));
    }
    close();
    return out;
}

// SVG's <rect> (rounded by r), <ellipse> and <circle> as outlines.
inline Outline Rect(double x, double y, double w, double h, double r = 0) {
    r = std::min({r, w / 2, h / 2});
    Outline o;
    if (r <= 0) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    const Pt c[4] = {{x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r}};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= 6; ++i) {
            const double t = (k - 1) * M_PI / 2 + i * M_PI / 12;   // from the top-right corner, clockwise on screen
            o.push_back({c[k].x + r * std::cos(t), c[k].y + r * std::sin(t)});
        }
    return o;
}
inline Outline Ellipse(double cx, double cy, double rx, double ry) {
    Outline o;
    for (int i = 0; i < 48; ++i) o.push_back({cx + rx * std::cos(i * M_PI / 24), cy + ry * std::sin(i * M_PI / 24)});
    return o;
}
// An outline turned `deg` degrees about (cx, cy), as SVG's rotate().
inline Outline Turned(Outline o, double deg, double cx, double cy) {
    const double a = deg * M_PI / 180, c = std::cos(a), s = std::sin(a);
    for (Pt &q : o) q = {cx + (q.x - cx) * c - (q.y - cy) * s, cy + (q.x - cx) * s + (q.y - cy) * c};
    return o;
}

// How much of each pixel of a size x size square the shapes cover (0..1), the 24-unit viewBox scaled
// into it: 5 lines a pixel down, exact across.
inline std::vector<float> Coverage(const std::vector<Shape> &shapes, int size) {
    std::vector<float> cover(size_t(size) * size, 0.f);
    if (size <= 0) return cover;
    const double k = size / 24.0;
    constexpr int kLines = 5;
    std::vector<float> row(size_t(size) + 1);
    struct Cross { double x; int dir; };
    std::vector<Cross> xs;
    for (const Shape &sh : shapes) {
        for (int y = 0; y < size; ++y) {
            std::fill(row.begin(), row.end(), 0.f);
            bool any = false;
            for (int l = 0; l < kLines; ++l) {
                const double sy = (y + (l + 0.5) / kLines) / k;
                xs.clear();
                for (const Outline &o : sh.outlines)
                    for (size_t i = 0; i < o.size(); ++i) {
                        const Pt a = o[i], b = o[(i + 1) % o.size()];
                        if ((a.y <= sy) == (b.y <= sy)) continue;
                        const double x = (a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x)) * k;
                        if (std::isfinite(x)) xs.push_back({x, b.y > a.y ? 1 : -1});   // (NaN or inf data: no crossing)
                    }
                std::sort(xs.begin(), xs.end(), [](const Cross &p, const Cross &q) { return p.x < q.x; });
                int wind = 0;
                for (size_t i = 0; i + 1 < xs.size(); ++i) {
                    wind += xs[i].dir;
                    const bool in = sh.evenOdd ? (i % 2 == 0) : wind != 0;
                    if (!in) continue;
                    const double x0 = std::clamp(xs[i].x, 0.0, double(size)), x1 = std::clamp(xs[i + 1].x, 0.0, double(size));
                    if (x1 <= x0) continue;
                    any = true;
                    const int a = int(x0), b = int(x1);
                    const float w = 1.f / kLines;
                    if (a == b) { row[size_t(a)] += float(x1 - x0) * w; continue; }
                    row[size_t(a)] += float(a + 1 - x0) * w;
                    for (int x = a + 1; x < b; ++x) row[size_t(x)] += w;
                    if (b < size) row[size_t(b)] += float(x1 - b) * w;
                }
            }
            if (!any) continue;
            for (int x = 0; x < size; ++x) {
                const float c = std::min(1.f, row[size_t(x)]) * float(sh.opacity);
                float &dst = cover[size_t(y) * size + x];
                dst = dst + c - dst * c;   // (over what's there)
            }
        }
    }
    return cover;
}

// The shapes drawn at (x0, y0), size pixels square, in rgb over the buffer's pixels.
inline void Paint(std::vector<uint8_t> &px, int W, int H, const std::vector<Shape> &shapes, int x0, int y0, int size,
                  const uint8_t rgb[3]) {
    const std::vector<float> cover = Coverage(shapes, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int X = x0 + x, Y = y0 + y;
            const float a = cover[size_t(y) * size + x];
            if (X < 0 || Y < 0 || X >= W || Y >= H || a <= 0) continue;
            uint8_t *p = &px[(size_t(Y) * W + X) * 4];
            for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] + (rgb[c] - p[c]) * a));
            p[3] = uint8_t(std::lround(p[3] + (255 - p[3]) * a));
        }
}

}  // namespace iconpath
