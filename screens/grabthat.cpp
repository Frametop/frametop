// "Grab that": an open hand held out to a window, then a fist pulled back (see grabthat.h).
#include "grabthat.h"

#include "palmmenu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace grabthat {
namespace {

using handcut::Hand;

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double Dist3(const float a[3], const float b[3]) {
    const double d[3] = {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]};
    return std::sqrt(Dot(d, d));
}

// A finger's extension: its tip's distance from the wrist over its knuckle's (`mcp`: 5 index,
// 9 middle, 13 ring, 17 little), as handinput's.
double Extension(const Hand &h, int mcp) { return Dist3(h.p[mcp + 3], h.p[0]) / std::max(1e-6, Dist3(h.p[mcp], h.p[0])); }

void Forward(const Mat &head, double out[3]) { out[0] = -head.m[0][2], out[1] = -head.m[1][2], out[2] = -head.m[2][2]; }

// A pose at c facing the point `at` (upright), as handinput's.
Mat Facing(const double c[3], const double at[3]) {
    double z[3] = {at[0] - c[0], at[1] - c[1], at[2] - c[2]};
    const double zl = std::sqrt(Dot(z, z));
    if (zl < 1e-6) z[0] = 0, z[1] = 0, z[2] = 1;
    else
        for (int i = 0; i < 3; ++i) z[i] /= zl;
    double x[3] = {z[2], 0, -z[0]};
    const double xl = std::hypot(x[0], x[2]);
    if (xl < 1e-6) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = float(y[i]), m.m[i][2] = float(z[i]), m.m[i][3] = float(c[i]);
    return m;
}

struct Quat {
    double w, x, y, z;
};
Quat ToQuat(const Mat &m) {
    const double t = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (t > 0) {
        const double s = std::sqrt(t + 1) * 2;
        q = {s / 4, (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s};
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const double s = std::sqrt(1 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q = {(m.m[2][1] - m.m[1][2]) / s, s / 4, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s};
    } else if (m.m[1][1] > m.m[2][2]) {
        const double s = std::sqrt(1 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q = {(m.m[0][2] - m.m[2][0]) / s, (m.m[0][1] + m.m[1][0]) / s, s / 4, (m.m[1][2] + m.m[2][1]) / s};
    } else {
        const double s = std::sqrt(1 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q = {(m.m[1][0] - m.m[0][1]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, s / 4};
    }
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}
void FromQuat(const Quat &q, Mat *m) {
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m->m[i][j] = float(r[i][j]);
}

}  // namespace

Features Measure(const Hand &h, const Mat &head) {
    Features f;
    double e[4];
    for (int k = 0; k < 4; ++k) e[k] = Extension(h, 5 + 4 * k);
    f.open = (e[0] + e[1] + e[2] + e[3]) / 4, f.curl3 = (e[1] + e[2] + e[3]) / 3;
    double fw[3], n[3];
    Forward(head, fw);
    palmmenu::PalmNormal(h, n);
    f.palmFwd = Dot(n, fw);
    const double w[3] = {h.p[0][0] - head.m[0][3], h.p[0][1] - head.m[1][3], h.p[0][2] - head.m[2][3]};
    f.fwd = Dot(w, fw);
    double palm[3];
    h.Palm(palm);
    double a[3] = {palm[0] - head.m[0][3], palm[1] - head.m[1][3], palm[2] - head.m[2][3]};
    const double al = std::sqrt(Dot(a, a));
    f.aim = al > 1e-6 ? Dot(n, a) / al : 0;
    return f;
}

void Detector::Reset() {
    for (Side &sd : side_) sd = Side{};
    fired_.clear();
}

void Detector::Quiet(int64_t until) {
    for (Side &sd : side_) {
        sd.quietUntil = std::max(sd.quietUntil, until);
        if (sd.state != State::Idle) sd.state = State::Idle, sd.recent.clear();
    }
}

void Detector::Open(Side &sd, int64_t t, const Hand &h, const Mat &head) {
    sd.lastOpen = t;
    for (int i = 0; i < 3; ++i) sd.eye[i] = head.m[i][3];
    h.Palm(sd.palm);
    palmmenu::PalmNormal(h, sd.normal);
}

void Detector::Frame(int64_t t, const std::vector<Hand> &hands, const Mat &head, const bool allowed[2]) {
    fired_.clear();
    for (int s = 0; s < 2; ++s) {
        Side &sd = side_[s];
        if (!p_.on || !allowed[s]) {
            sd.state = State::Idle, sd.recent.clear(), sd.claimUntil = 0;
            continue;
        }
        // the hand it follows, else that side's most confident one
        const Hand *h = nullptr;
        if (sd.state != State::Idle)
            for (const Hand &c : hands)
                if (c.id == sd.handId) h = &c;
        if (!h)
            for (const Hand &c : hands)
                if (c.right == (s == 1) && (!h || c.confidence > h->confidence)) h = &c;
        if (!h || !h->stereo) {   // one camera's depth is 2-6 cm off: it neither arms nor fires
            if (sd.state != State::Idle && t - sd.lastSeen > int64_t(p_.lostMs * 1e6)) sd.state = State::Idle, sd.recent.clear();
            continue;
        }
        if (h->id != sd.handId) sd.recent.clear();
        sd.lastSeen = t, sd.handId = h->id, sd.head = head;
        const Features f = Measure(*h, head);
        sd.thumb = Dist3(h->p[4], h->p[8]);
        sd.recent.push_back({t, f.fwd, f.palmFwd});
        while (!sd.recent.empty() && t - sd.recent.front().t > int64_t(p_.pullMs * 1e6)) sd.recent.erase(sd.recent.begin());
        double palmNow[3];
        h->Palm(palmNow);
        const double facingDeg = std::acos(std::clamp(f.aim, -1.0, 1.0)) * 180 / M_PI;
        const bool enter = f.open >= p_.openArm && f.palmFwd >= p_.palmArm && f.fwd >= p_.fwdArm;
        const bool keep = f.open >= p_.openKeep && f.palmFwd >= p_.palmKeep && f.fwd >= p_.fwdKeep;
        const bool closing = f.curl3 < p_.claimCurl;
        const bool stale = t - sd.lastOpen > int64_t(p_.closeMs * 1e6);
        // the arming hold: from here, its facing summed and where the palm started
        auto hold = [&] {
            sd.since = t, sd.facingSum = 0, sd.facingN = 0;
            std::copy(palmNow, palmNow + 3, sd.holdFrom);
        };
        switch (sd.state) {
            case State::Idle:
                if (enter && t >= sd.quietUntil) {
                    sd.state = State::Opening, hold(), Open(sd, t, *h, head);
                    sd.facingSum += facingDeg, ++sd.facingN;
                }
                break;
            case State::Opening: {
                if (!keep || t < sd.quietUntil) {
                    sd.state = State::Idle;
                    break;
                }
                Open(sd, t, *h, head);
                sd.facingSum += facingDeg, ++sd.facingN;
                if (t - sd.since < int64_t(p_.armMs * 1e6)) break;
                // held armMs: facing along the aim on average (Params::aimFacing), and still (the palm's
                // way over it, Params::holdSpeed); else the hold starts again from here
                const double secs = (t - sd.since) / 1e9;
                const double d[3] = {palmNow[0] - sd.holdFrom[0], palmNow[1] - sd.holdFrom[1], palmNow[2] - sd.holdFrom[2]};
                const double facing = sd.facingSum / std::max(1, sd.facingN), moved = std::sqrt(Dot(d, d)) / std::max(1e-3, secs);
                if ((p_.aimFacing > 0 && facing > p_.aimFacing) || (p_.holdSpeed > 0 && moved > p_.holdSpeed)) {
                    ++stats.unsteady;
                    hold();
                    break;
                }
                sd.facing = facing, sd.speed = moved;
                sd.state = State::Armed, ++stats.arms;
                break;
            }
            case State::Armed:
            case State::Closing:
                if (sd.state == State::Armed) {
                    if (keep && !closing) {
                        Open(sd, t, *h, head);
                        break;
                    }
                    if (!closing) {   // turned away or lowered, still open: armed a little longer
                        if (stale) sd.state = State::Idle;
                        break;
                    }
                    double palm = f.palmFwd;   // closing on the window: the palm toward it as the fingers began
                    for (const Side::Sample &q : sd.recent)
                        if (t - q.t <= int64_t(p_.closePalmMs * 1e6)) palm = std::max(palm, q.palm);
                    if (palm < p_.closePalm) {
                        sd.state = State::Idle;
                        break;
                    }
                    sd.state = State::Closing, ++stats.closings;
                } else if (enter && !closing) {   // opened out again
                    sd.state = State::Armed;
                    Open(sd, t, *h, head);
                    break;
                }
                if (stale) {
                    sd.state = State::Idle, ++stats.timeouts;
                    break;
                }
                double farthest = f.fwd;
                for (const Side::Sample &q : sd.recent) farthest = std::max(farthest, q.fwd);
                if (f.open <= p_.fist && farthest - f.fwd >= p_.pull && f.palmFwd <= p_.palmFlip) {
                    Fire x;
                    x.side = s, x.t = t, x.head = head, x.facing = sd.facing, x.speed = sd.speed;
                    std::copy(sd.eye, sd.eye + 3, x.eye), std::copy(sd.palm, sd.palm + 3, x.palm);
                    std::copy(sd.normal, sd.normal + 3, x.normal);
                    h->Palm(x.fist);
                    fired_.push_back(x);
                    ++stats.fires;
                    sd.state = State::Idle, sd.recent.clear();
                    sd.quietUntil = t + int64_t(p_.againMs * 1e6), sd.claimUntil = t + int64_t(p_.claimMs * 1e6);
                }
                break;
        }
    }
}

bool Detector::Aim(int side, Fire *out, double *progress) const {
    const Side &sd = side_[side];
    if (!p_.on || sd.state == State::Idle) return false;
    out->side = side, out->t = sd.lastSeen, out->head = sd.head;
    std::copy(sd.eye, sd.eye + 3, out->eye), std::copy(sd.palm, sd.palm + 3, out->palm);
    std::copy(sd.normal, sd.normal + 3, out->normal);
    for (int i = 0; i < 3; ++i) out->fist[i] = sd.palm[i];
    *progress = sd.state == State::Opening ? std::clamp((sd.lastSeen - sd.since) / (p_.armMs * 1e6), 0.0, 1.0) : 1.0;
    return true;
}

bool Detector::claims(int side, int64_t t) const {
    const Side &sd = side_[side];
    return p_.on && ((sd.state == State::Closing && sd.thumb >= p_.claimThumb) || t < sd.claimUntil);
}

std::string Detector::Describe() const {
    static const char *names[] = {"idle", "opening", "armed", "closing"};
    return std::string("left ") + names[int(side_[0].state)] + ", right " + names[int(side_[1].state)];
}

bool RayHit(const Target &p, const double from[3], const double dir[3], double *u, double *v, double *t) {
    // the ray in the panel's frame
    const double d0[3] = {from[0] - p.pose.m[0][3], from[1] - p.pose.m[1][3], from[2] - p.pose.m[2][3]};
    double o[3], d[3];
    for (int i = 0; i < 3; ++i) {
        o[i] = p.pose.m[0][i] * d0[0] + p.pose.m[1][i] * d0[1] + p.pose.m[2][i] * d0[2];
        d[i] = p.pose.m[0][i] * dir[0] + p.pose.m[1][i] * dir[1] + p.pose.m[2][i] * dir[2];
    }
    if (p.curve <= 0) {
        if (std::fabs(d[2]) < 1e-9) return false;
        *t = -o[2] / d[2];
        if (*t <= 0) return false;
        *u = o[0] + *t * d[0], *v = o[1] + *t * d[1];
        return true;
    }
    // the cylinder's axis at (0, *, r), the panel the part of it nearest the origin (Locate's)
    const double r = p.curve, a = d[0] * d[0] + d[2] * d[2], b = 2 * (o[0] * d[0] + (o[2] - r) * d[2]),
                 c = o[0] * o[0] + (o[2] - r) * (o[2] - r) - r * r, disc = b * b - 4 * a * c;
    if (a < 1e-12 || disc < 0) return false;
    const double q = std::sqrt(disc), roots[2] = {(-b - q) / (2 * a), (-b + q) / (2 * a)};
    for (double k : roots) {
        const double x = o[0] + k * d[0], z = o[2] + k * d[2];
        if (k <= 0 || r - z <= 0) continue;
        *t = k, *u = r * std::atan2(x, r - z), *v = o[1] + k * d[1];
        return true;
    }
    return false;
}

double OffDeg(const Target &p, const double from[3], const double dir[3]) {
    // the outline, sampled (n steps an edge): seen from outside it, the nearest point is on it
    const int n = 48;
    double best = 180;
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i <= n; ++i) {
            const double s = double(i) / n - 0.5;
            const double u = e < 2 ? s * p.width : (e == 2 ? -0.5 : 0.5) * p.width;
            const double v = e < 2 ? (e == 0 ? -0.5 : 0.5) * p.height : s * p.height;
            double x = u, z = 0;   // (on a curved one along its curve, as RayHit)
            if (p.curve > 0) x = p.curve * std::sin(u / p.curve), z = p.curve * (1 - std::cos(u / p.curve));
            double d[3];
            for (int r = 0; r < 3; ++r) d[r] = p.pose.m[r][0] * x + p.pose.m[r][1] * v + p.pose.m[r][2] * z + p.pose.m[r][3] - from[r];
            const double l = std::sqrt(Dot(d, d));
            if (l < 1e-6) continue;
            best = std::min(best, std::acos(std::clamp(Dot(d, dir) / l, -1.0, 1.0)) * 180 / M_PI);
        }
    return best;
}

int Pick(const std::vector<Target> &panels, const Fire &f, const Params &k, Closest *closest) {
    double dir[3] = {f.palm[0] - f.eye[0], f.palm[1] - f.eye[1], f.palm[2] - f.eye[2]};
    const double l = std::sqrt(Dot(dir, dir));
    int best = -1;
    double bestT = 1e9;
    // (not one near the eyes: Params::nearest)
    auto near = [&](const Target &p) {
        return k.nearest > 0 && std::hypot(p.pose.m[0][3] - f.eye[0], p.pose.m[1][3] - f.eye[1], p.pose.m[2][3] - f.eye[2]) < k.nearest;
    };
    if (l > 1e-6) {
        for (double &x : dir) x /= l;
        // one the line meets (within aimMargin), the nearest
        for (const Target &p : panels) {
            double u, v, t;
            if (near(p) || !RayHit(p, f.eye, dir, &u, &v, &t)) continue;
            if (std::fabs(u) <= p.width / 2 + k.aimMargin && std::fabs(v) <= p.height / 2 + k.aimMargin && t < bestT) {
                bestT = t, best = p.index;
                if (closest) closest->index = p.index, closest->deg = 0;
            }
        }
        // else the one it passes nearest, within aimMarginDeg
        // (a panel that looks small, e.g. a little window pushed far out, gets a wider margin: up to
        // smallDeg more for one that spans nothing, less as it spans more; user, 2026-10-11)
        if (best < 0) {
            double bestScore = 1e9;
            for (const Target &p : panels) {
                if (near(p)) continue;
                const double off = OffDeg(p, f.eye, dir);
                if (closest && off < closest->deg) closest->index = p.index, closest->deg = off;
                const double dist = std::max(0.1, std::hypot(p.pose.m[0][3] - f.eye[0], p.pose.m[1][3] - f.eye[1],
                                                             p.pose.m[2][3] - f.eye[2]));
                const double half = std::atan(std::max(p.width, p.height) / 2 / dist) * 180 / M_PI;
                const double extra = std::max(0.0, k.smallDeg - half);
                if (off <= k.aimMarginDeg + extra && off - extra < bestScore) bestScore = off - extra, best = p.index;
            }
        }
    }
    if (best >= 0 || k.headCone <= 0) return best;
    // where the head points
    double fw[3];
    Forward(f.head, fw);
    double bestA = k.headCone;
    for (const Target &p : panels) {
        if (near(p)) continue;
        double to[3] = {p.pose.m[0][3] - f.head.m[0][3], p.pose.m[1][3] - f.head.m[1][3], p.pose.m[2][3] - f.head.m[2][3]};
        const double tl = std::sqrt(Dot(to, to));
        if (tl < 1e-6) continue;
        const double a = std::acos(std::clamp(Dot(to, fw) / tl, -1.0, 1.0)) * 180 / M_PI;
        if (a <= bestA) bestA = a, best = p.index;
    }
    return best;
}

Mat Arrival(const Fire &f, const Params &k) {
    double fw[3];
    Forward(f.head, fw);
    double hx = fw[0], hz = fw[2];
    const double hl = std::hypot(hx, hz);
    if (hl < 1e-6) hx = 0, hz = -1;
    else hx /= hl, hz /= hl;
    const double eye[3] = {f.head.m[0][3], f.head.m[1][3], f.head.m[2][3]};
    const double c[3] = {eye[0] + hx * k.distance, std::clamp(f.fist[1], eye[1] - k.below, eye[1] + k.above),
                         eye[2] + hz * k.distance};
    return Facing(c, eye);
}

Outcome Decide(bool came, const Mat &now, const Mat &head, const Params &k) {
    const double d = std::hypot(now.m[0][3] - head.m[0][3], now.m[1][3] - head.m[1][3], now.m[2][3] - head.m[2][3]);
    const bool near = d <= k.distance + k.reachSlack;
    return near ? (came ? Outcome::Back : Outcome::Centre) : Outcome::Bring;
}

const char *OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Back: return "goes back";
        case Outcome::Centre: return "is near already: centred at arm's reach";
        default: return "comes to arm's reach";
    }
}

Mat Blend(const Mat &a, const Mat &b, double k) {
    Quat qa = ToQuat(a), qb = ToQuat(b);
    double c = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
    if (c < 0) qb = {-qb.w, -qb.x, -qb.y, -qb.z}, c = -c;
    double wa = 1 - k, wb = k;
    if (c < 0.9995) {
        const double th = std::acos(c), s = std::sin(th);
        wa = std::sin((1 - k) * th) / s, wb = std::sin(k * th) / s;
    }
    Quat q{wa * qa.w + wb * qb.w, wa * qa.x + wb * qb.x, wa * qa.y + wb * qb.y, wa * qa.z + wb * qb.z};
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    q = {q.w / l, q.x / l, q.y / l, q.z / l};
    Mat m{};
    FromQuat(q, &m);
    for (int i = 0; i < 3; ++i) m.m[i][3] = float(a.m[i][3] + (b.m[i][3] - a.m[i][3]) * k);
    return m;
}

double Ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

Mat InFront(const Mat &head, double distance, double drop) {
    double fw[3];
    Forward(head, fw);
    double hx = fw[0], hz = fw[2];
    const double hl = std::hypot(hx, hz);
    if (hl < 1e-6) hx = 0, hz = -1;
    else hx /= hl, hz /= hl;
    const double eye[3] = {head.m[0][3], head.m[1][3], head.m[2][3]};
    const double c[3] = {eye[0] + hx * distance, eye[1] - drop, eye[2] + hz * distance};
    return Facing(c, eye);
}

double ViewDistance(double width, double maxDeg, double nearest, double farthest, double *fit) {
    const double t = std::tan(std::clamp(maxDeg, 1.0, 170.0) * M_PI / 360);
    const double d = std::clamp(width / (2 * t), nearest, std::max(nearest, farthest));
    if (fit) *fit = std::min(width, 2 * d * t);
    return d;
}

Readable ReadableSize(double pxW, double pxH, double distance, const ReadParams &k) {
    Readable r;
    if (pxW < 1 || pxH < 1 || distance <= 0) return r;
    const double ppd = std::max(1.0, k.ppd);
    double w = pxW / ppd, h = pxH / ppd;
    const double maxW = pxW / pxH >= kWideAspect ? std::max(k.maxW, k.maxWideW) : k.maxW;
    const double shrink = std::min({1.0, maxW / w, k.maxH / h});
    w *= shrink, h *= shrink;
    if (w < k.minW) h *= k.minW / w, w = k.minW;
    r.degW = w, r.degH = h, r.ppd = pxW / w;
    if (w <= k.flatDeg) {
        r.width = 2 * distance * std::tan(w * M_PI / 360);
    } else {
        r.width = distance * w * M_PI / 180;   // round the eyes: the arc
        r.curve = distance;
    }
    return r;
}

int SusanPick(const std::vector<Bearing> &panels, int current, double from, bool next, double aheadDeg) {
    int best = -1;
    double bestA = 1e9;
    for (const Bearing &b : panels) {
        if (b.index == current) continue;
        if (current < 0 && std::fabs(b.deg) <= aheadDeg) continue;   // (in front already)
        double a = (next ? 1 : -1) * (b.deg - from);
        while (a <= 1e-6) a += 360;   // round past where it started
        while (a > 360) a -= 360;
        if (a < bestA) bestA = a, best = b.index;
    }
    return best;
}

}  // namespace grabthat
