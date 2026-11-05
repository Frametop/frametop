// SteamVR's own panels for near hand input: see steampanels.h.
#include "steampanels.h"

#include <algorithm>
#include <cmath>

namespace steampanels {
namespace {

double Det3(const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

void Cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1], out[1] = a[2] * b[0] - a[0] * b[2], out[2] = a[0] * b[1] - a[1] * b[0];
}

double Len(const double a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

Mat Mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    return r;
}

Mat Inverse(const Mat &a) {   // rigid: R^T, -R^T t
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
        r.m[i][3] = -(a.m[0][i] * a.m[0][3] + a.m[1][i] * a.m[1][3] + a.m[2][i] * a.m[2][3]);
    }
    return r;
}

}  // namespace

Fit FitPanel(const std::vector<Sample> &samples, int minHits) {
    Fit f;
    f.hits = int(samples.size());
    if (f.hits < std::max(3, minHits)) return f;
    // normal equations for [1 u v] -> (x, y, z)
    double ata[3][3] = {}, atb[3][3] = {};
    double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
    for (const Sample &s : samples) {
        const double row[3] = {1, s.u, s.v};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) ata[i][j] += row[i] * row[j], atb[i][j] += row[i] * s.p[j];
        u0 = std::min(u0, s.u), u1 = std::max(u1, s.u), v0 = std::min(v0, s.v), v1 = std::max(v1, s.v);
    }
    // the hits spread over it both ways (a line of them fits no plane)
    if (u1 - u0 < 1e-3 || v1 - v0 < 1e-3) return f;
    const double d = Det3(ata);
    if (std::fabs(d) < 1e-12) return f;
    double x[3][3];   // x[k][j]: coefficient k (1, u, v) of coordinate j, by Cramer's rule
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
            double t[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) t[r][c] = c == k ? atb[r][j] : ata[r][c];
            x[k][j] = Det3(t) / d;
        }
    const double *O = x[0], *U = x[1], *V = x[2];
    const double w = Len(U), h = Len(V);
    if (w < 0.03 || h < 0.03 || w > 20 || h > 20) return f;
    double n[3];
    Cross(U, V, n);
    const double nl = Len(n);
    if (nl < 1e-9) return f;
    double bx[3], bz[3], by[3];
    for (int i = 0; i < 3; ++i) bx[i] = U[i] / w, bz[i] = n[i] / nl;
    Cross(bz, bx, by);
    for (int i = 0; i < 3; ++i) {
        f.pose.m[i][0] = float(bx[i]), f.pose.m[i][1] = float(by[i]), f.pose.m[i][2] = float(bz[i]);
        f.pose.m[i][3] = float(O[i] + U[i] * 0.5 + V[i] * 0.5);
    }
    f.width = w, f.height = h, f.ok = true;
    return f;
}

std::vector<std::array<double, 3>> Cone(const double from[3], const double at[3], double halfDeg, int n) {
    std::vector<std::array<double, 3>> out;
    double f[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    const double l = Len(f);
    if (l < 1e-6 || n < 1) return out;
    for (double &c : f) c /= l;
    // two directions across it
    const double up[3] = {0, 1, 0}, side[3] = {1, 0, 0};
    double a[3], b[3];
    Cross(f, std::fabs(f[1]) < 0.9 ? up : side, a);
    const double al = Len(a);
    for (double &c : a) c /= al;
    Cross(f, a, b);
    const double t = std::tan(halfDeg * M_PI / 180);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const double s = n == 1 ? 0 : t * (2.0 * i / (n - 1) - 1), r = n == 1 ? 0 : t * (2.0 * j / (n - 1) - 1);
            double d[3] = {f[0] + s * a[0] + r * b[0], f[1] + s * a[1] + r * b[1], f[2] + s * a[2] + r * b[2]};
            const double dl = Len(d);
            out.push_back({d[0] / dl, d[1] / dl, d[2] / dl});
        }
    return out;
}

void FromPixels(double width, double height, double x, double y, double *u, double *v) {
    *u = x / kPxPerM - width / 2;
    *v = height / 2 - y / kPxPerM;
}

Mat Touch(const Mat &panel, double u, double v, double back) {
    Mat m = panel;
    for (int i = 0; i < 3; ++i) m.m[i][3] = float(panel.m[i][3] + panel.m[i][0] * u + panel.m[i][1] * v + panel.m[i][2] * back);
    return m;
}

void GrabPoint(double width, double height, bool resize, int sx, int sy, double barBelow, double margin, double *u,
               double *v) {
    if (resize) {
        *u = (sx >= 0 ? 1 : -1) * (width / 2 + margin);
        *v = (sy >= 0 ? 1 : -1) * (height / 2 + margin);
    } else {
        *u = 0;
        *v = -height / 2 - barBelow;
    }
}

Mat Follow(const Mat &from, const Mat &to, const Mat &laser) { return Mul(to, Mul(Inverse(from), laser)); }

void Quat(const Mat &a, double q[4]) {
    const double m00 = a.m[0][0], m11 = a.m[1][1], m22 = a.m[2][2], trace = m00 + m11 + m22;
    if (trace > 0) {
        const double s = 0.5 / std::sqrt(trace + 1);
        q[0] = 0.25 / s, q[1] = (a.m[2][1] - a.m[1][2]) * s, q[2] = (a.m[0][2] - a.m[2][0]) * s, q[3] = (a.m[1][0] - a.m[0][1]) * s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = 2 * std::sqrt(1 + m00 - m11 - m22);
        q[0] = (a.m[2][1] - a.m[1][2]) / s, q[1] = 0.25 * s, q[2] = (a.m[0][1] + a.m[1][0]) / s, q[3] = (a.m[0][2] + a.m[2][0]) / s;
    } else if (m11 > m22) {
        const double s = 2 * std::sqrt(1 + m11 - m00 - m22);
        q[0] = (a.m[0][2] - a.m[2][0]) / s, q[1] = (a.m[0][1] + a.m[1][0]) / s, q[2] = 0.25 * s, q[3] = (a.m[1][2] + a.m[2][1]) / s;
    } else {
        const double s = 2 * std::sqrt(1 + m22 - m00 - m11);
        q[0] = (a.m[1][0] - a.m[0][1]) / s, q[1] = (a.m[0][2] + a.m[2][0]) / s, q[2] = (a.m[1][2] + a.m[2][1]) / s, q[3] = 0.25 * s;
    }
    if (q[0] < 0)
        for (int i = 0; i < 4; ++i) q[i] = -q[i];
}

}  // namespace steampanels
