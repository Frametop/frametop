// The palm menu (see palmmenu.h).
#include "palmmenu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace palmmenu {
namespace {

using handcut::Hand;

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double Dist3(const float a[3], const float b[3]) {
    const double d[3] = {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]};
    return std::sqrt(Dot(d, d));
}

double Deg(double cosine) { return std::acos(std::clamp(cosine, -1.0, 1.0)) * 180 / M_PI; }

// How open the hand is: the middle, ring and little fingers' tip-to-wrist over knuckle-to-wrist.
double Openness(const Hand &h) {
    double sum = 0;
    for (int mcp : {9, 13, 17}) sum += Dist3(h.p[mcp + 3], h.p[0]) / std::max(1e-6, Dist3(h.p[mcp], h.p[0]));
    return sum / 3;
}

}  // namespace

void PalmNormal(const Hand &h, double out[3]) {
    const double a[3] = {double(h.p[5][0]) - h.p[0][0], double(h.p[5][1]) - h.p[0][1], double(h.p[5][2]) - h.p[0][2]};
    const double b[3] = {double(h.p[17][0]) - h.p[0][0], double(h.p[17][1]) - h.p[0][1], double(h.p[17][2]) - h.p[0][2]};
    double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double l = std::sqrt(Dot(n, n)) * (h.right ? 1 : -1);
    for (int i = 0; i < 3; ++i) out[i] = l != 0 ? n[i] / l : 0;
}

bool PalmUp(const Hand &h, const Mat &head, const Params &k, bool was, Pose *pose) {
    double n[3], palm[3];
    PalmNormal(h, n);
    h.Palm(palm);
    double to[3] = {head.m[0][3] - palm[0], head.m[1][3] - palm[1], head.m[2][3] - palm[2]};
    const double dist = std::sqrt(Dot(to, to));
    if (dist < 1e-6) return false;
    for (double &v : to) v /= dist;
    const double forward[3] = {-head.m[0][2], -head.m[1][2], -head.m[2][2]};
    Pose p;
    p.dist = dist;
    p.facingDeg = Deg(Dot(n, to));
    p.lookDeg = Deg(-Dot(forward, to));
    p.open = Openness(h);
    if (pose) *pose = p;
    return dist >= k.nearest && dist <= k.farthest && p.facingDeg <= (was ? k.facingLeave : k.facing) &&
           p.lookDeg <= (was ? k.lookLeave : k.look) && p.open >= (was ? k.openLeave : k.open);
}

const Hand *PalmMenu::HandFor(int side, const std::vector<Hand> &hands) const {
    const Side &sd = side_[side];
    if (sd.state != State::Idle && sd.handId)
        for (const Hand &h : hands)
            if (h.id == sd.handId) return &h;   // the hand it follows, whatever its side reads now
    const Hand *best = nullptr;
    for (const Hand &h : hands)
        if (h.right == (side == 1) && (!best || h.confidence > best->confidence)) best = &h;
    return best;
}

bool PalmMenu::Plain(const Hand *h) const {
    if (!h) return false;
    const double mid = Dist3(h->p[12], h->p[0]) / std::max(1e-6, Dist3(h->p[9], h->p[0]));
    return Dist3(h->p[4], h->p[12]) >= p_.plainMiddle && mid >= p_.plainExtended && h->speed() < p_.pinchStill;
}

void PalmMenu::Release(int s, bool lost, int64_t now) {
    Side &sd = side_[s];
    sd.state = State::Shown, sd.awaySince = -1;
    // lost, or the hand turned away before letting go: nothing
    if (lost || !sd.up) {
        ++stats.cancels;
        return;
    }
    if (now - lastToggle_ < int64_t(p_.againMs * 1e6)) return;
    lastToggle_ = now, toggled_ = s;
    ++stats.toggles;
}

void PalmMenu::Update(const Input &in) {
    toggled_ = -1;
    const handcut::Hands &H = *in.hands;
    const bool newFrame = H.frame() != frame_;
    frame_ = H.frame();
    for (int s = 0; s < 2; ++s) {
        Side &sd = side_[s];
        if (!p_.on || !in.allowed[s] || in.busy[s] || !in.haveHead) {
            if (sd.state == State::Pinched) ++stats.cancels;
            sd = Side{};
            sd.state = State::Off;
            if (in.busy[s]) sd.quietUntil = in.nowNs + int64_t(p_.quietMs * 1e6);
            continue;
        }
        // at a panel: no ring now, nor for quietMs (a pinch on the ring held goes on)
        if (in.atPanel[s]) {
            sd.quietUntil = in.nowNs + int64_t(p_.quietMs * 1e6);
            if (sd.state == State::Shown) sd.state = State::Idle;
            sd.upSince = -1;
        }
        if (sd.state == State::Off) sd.state = State::Idle;
        const Hand *h = HandFor(s, H.seen());
        if (h) sd.lastSeen = in.nowNs, sd.handId = h->id;
        if (!h && in.nowNs - sd.lastSeen > int64_t(p_.lostMs * 1e6)) {
            if (sd.state == State::Pinched) ++stats.cancels;
            sd.state = State::Idle, sd.up = false, sd.upSince = -1;
        }
        if (newFrame) sd.up = h && PalmUp(*h, in.head, p_, shown(s));
        const handcut::Gesture &g = H.pinch(s);
        if (!sd.knowBegins) sd.begins = g.begins, sd.knowBegins = true;   // not a pinch that began before
        const bool began = g.begins != sd.begins;
        sd.begins = g.begins;
        switch (sd.state) {
            case State::Off:
                break;
            case State::Idle:
                // a pinch now is the helper's (gaze); the ring shows after showMs of palm up
                if (!newFrame) break;
                if (!sd.up || in.nowNs < sd.quietUntil || (h && h->speed() >= p_.still)) {
                    sd.upSince = -1;
                    break;
                }
                if (sd.upSince < 0) sd.upSince = in.nowNs;
                if (in.nowNs - sd.upSince < int64_t(p_.showMs * 1e6)) break;
                sd.state = State::Shown, sd.shownNs = H.captureNs(), sd.awaySince = -1;
                ++stats.shows;
                // a pinch beginning on this very frame: the near report for it says ours
                if (began && g.down && g.beginNs >= sd.shownNs) {
                    if (Plain(h)) sd.state = State::Pinched, ++stats.pinches;
                    else ++stats.notPlain;
                }
                break;
            case State::Shown:
                // A pinch that began while the ring showed (the helper saw this hand near then).
                if (began && g.beginNs >= sd.shownNs) {
                    if (!Plain(h)) {   // the middle finger in it, the fingers bunched, or the hand moving: not ours to toggle
                        ++stats.notPlain;
                    } else {
                        ++stats.pinches;
                        if (g.down) {
                            sd.state = State::Pinched;
                            break;
                        }
                        Release(s, g.lost, in.nowNs);   // began and ended between two reads: a tap
                    }
                }
                if (!newFrame) break;
                if (sd.up) {
                    sd.awaySince = -1;
                } else {
                    if (sd.awaySince < 0) sd.awaySince = in.nowNs;
                    if (in.nowNs - sd.awaySince >= int64_t(p_.hideMs * 1e6)) sd.state = State::Idle, sd.upSince = -1;
                }
                break;
            case State::Pinched:
                // held: the ring stays whatever the palm does; the release decides
                if (began || !g.down) {
                    Release(s, g.lost, in.nowNs);
                    if (began && g.down && Plain(h)) sd.state = State::Pinched, ++stats.pinches;   // and another began
                }
                break;
        }
    }
}

bool PalmMenu::Ring(int side, const std::vector<Hand> &hands, double out[3]) const {
    if (!shown(side)) return false;
    const Hand *h = HandFor(side, hands);
    if (!h) return false;
    double n[3];
    PalmNormal(*h, n);
    h->Palm(out);
    for (int i = 0; i < 3; ++i) out[i] += n[i] * p_.lift;
    return true;
}

std::string PalmMenu::Describe() const {
    static const char *names[] = {"off", "idle", "shown", "pinched"};
    char b[64];
    std::snprintf(b, sizeof b, "left %s, right %s", names[int(side_[0].state)], names[int(side_[1].state)]);
    return b;
}

}  // namespace palmmenu
