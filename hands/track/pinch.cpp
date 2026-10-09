#include "pinch.h"

#include "io.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kThumbTip = 4;

V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

void put3(float out[3], V3 v) {
    for (int k = 0; k < 3; ++k) out[k] = float(v[k]);
}

}  // namespace

void Pinch::update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns,
                   const std::vector<int> &gripping, const std::vector<int> &blocked) {
    events.clear();
    auto grips = [&](int id) { return std::find(gripping.begin(), gripping.end(), id) != gripping.end(); };
    auto other_down = [&](int id) { return std::find(blocked.begin(), blocked.end(), id) != blocked.end(); };
    const int tip = p_.finger, other = p_.other_finger;
    for (int s = 0; s < 2; ++s)   // a grip took this pinch's hand: it's a drag now, not a click
        if ((side_[s].flags & FH_PINCH_DOWN) && grips(follow_[s])) end(s, t_ns, true);
    // A hand a pinch is down on belongs to that side until it ends. The left/right call is a
    // running average of the model's, and when it flips mid-pinch the other side would take
    // the same hand and pinch too (4 times in the 2026-09-30 lit recording).
    int taken[2] = {0, 0};
    for (int s = 0; s < 2; ++s)
        if (side_[s].flags & FH_PINCH_DOWN) taken[s] = follow_[s];
    for (int s = 0; s < 2; ++s) {
        fh_pinch_t &o = side_[s];
        const bool down = o.flags & FH_PINCH_DOWN;
        // the hand: while down, the one the pinch began on; else the best tracked hand of this side
        const Hand *h = nullptr;
        for (const Hand *c : hands) {
            if (down ? c->id != follow_[s] : c->right() != (s == 1) || c->id == taken[1 - s]) continue;
            if (!h || c->frames > h->frames) h = c;
        }
        world_d[s] = tri_d[s] = fit_d[s] = palm_down[s] = other_d[s] = -1;
        if (!h) {
            o.flags &= ~FH_PINCH_TRACKED;
            if (down && (t_ns - seen_ns_[s]) / 1e9 > p_.grace_s) end(s, t_ns, true);
            continue;
        }
        seen_ns_[s] = t_ns;
        tri_d[s] = norm(h->pts[kThumbTip] - h->pts[tip]);
        if (h->fit.valid) fit_d[s] = norm(h->fit.pts[kThumbTip] - h->fit.pts[tip]);
        const V3 normal = cross(h->smooth[5] - h->smooth[0], h->smooth[17] - h->smooth[0]);
        palm_down[s] = norm(normal) > 0 ? std::fabs(normal[1]) / norm(normal) : 0;
        // thumb tip to fingertip f: the model's world landmarks averaged over the views (at the hand's
        // scale), the fitted tips, or the triangulated ones, as configured
        auto measure = [&](int f, double *world, double *tri, double *fit) {
            double sum = 0;
            int n = 0;
            for (const Seen &v : views) {
                if (v.hand != h->id) continue;
                const V3 a{v.lm.world[kThumbTip][0], v.lm.world[kThumbTip][1], v.lm.world[kThumbTip][2]};
                const V3 b{v.lm.world[f][0], v.lm.world[f][1], v.lm.world[f][2]};
                sum += norm(a - b), ++n;
            }
            *world = n ? sum / n * h->scale : -1;
            *tri = norm(h->pts[kThumbTip] - h->pts[f]);
            *fit = h->fit.valid ? norm(h->fit.pts[kThumbTip] - h->fit.pts[f]) : -1;
            return p_.fitted && *fit >= 0 ? *fit : p_.triangulated || *world < 0 ? *tri : *world;
        };
        double unused_tri, unused_fit;
        const double d = measure(tip, &world_d[s], &unused_tri, &unused_fit);
        double ow = -1, ot = -1, of = -1;
        if (other) other_d[s] = measure(other, &ow, &ot, &of);
        // the thumb clearly nearer this fingertip than the other one, the extended finger straight
        // enough (from the model's world landmarks, else the tracked points), the hand tracked a while
        bool own = !other || d + p_.other_margin < other_d[s];
        if (own && p_.extended_finger > 0) {
            const int f = p_.extended_finger, knuckle = f - 3;
            double ratio = -1;
            for (const Seen &v : views) {
                if (v.hand != h->id) continue;
                auto w = [&](int i) { return V3{v.lm.world[i][0], v.lm.world[i][1], v.lm.world[i][2]}; };
                const double k = norm(w(knuckle) - w(0));
                if (k > 1e-4) ratio = std::max(ratio, norm(w(f) - w(0)) / k);
            }
            if (ratio < 0) {
                const double k = norm(h->smooth[knuckle] - h->smooth[0]);
                ratio = k > 1e-4 ? norm(h->smooth[f] - h->smooth[0]) / k : 0;
            }
            own = ratio >= p_.extended_min;
        }
        if (own && h->frames < p_.min_frames) own = false;
        // Where the pinch is, for drags: the index and middle knuckles, which hold still while
        // the fingers open and close. The point between the tips moved 1-2 cm as a pinch
        // opened, so every release dragged the pointer off what it pressed (headset test,
        // 2026-09-30).
        const V3 point = (h->smooth[5] + h->smooth[9]) * 0.5;
        o.flags |= FH_PINCH_TRACKED;
        o.hand_id = uint32_t(h->id);
        o.distance = float(d);
        o.strength = float(std::clamp((p_.end_m - d) / (p_.end_m - p_.begin_m), 0.0, 1.0));
        put3(o.point, point);
        if (!down) {
            // a close held back (palm down) has to open again before a pinch can begin, so
            // turning the hand with the fingers still closed doesn't start one
            if (d > p_.end_m) held_[s] = false;
            if (grips(h->id) || other_down(h->id)) {
                // closed, or the other pinch is down on it: no pinch until it opens
            } else if (d < p_.begin_m && !own) {
                // the thumb is on the other fingertip
            } else if (d < p_.begin_m && !held_[s] && palm_down[s] > p_.palm_down_max) {
                held_[s] = true;
                ++held_back[s];
            } else if (d < p_.begin_m && !held_[s]) {
                o.flags = (o.flags | FH_PINCH_DOWN) & ~FH_PINCH_LOST;
                ++o.begins;
                o.begin_ns = uint64_t(t_ns);
                put3(o.begin_point, point);
                follow_[s] = h->id;
                open_frames_[s] = 0;
                events.push_back({s, "begin", t_ns, d, point});
            }
        } else if (d > p_.end_m) {
            if (++open_frames_[s] >= p_.end_frames) end(s, t_ns, false);
        } else {
            open_frames_[s] = 0;
        }
    }
}

void Pinch::end(int s, int64_t t_ns, bool lost) {
    fh_pinch_t &o = side_[s];
    o.flags = (o.flags & ~FH_PINCH_DOWN) | (lost ? FH_PINCH_LOST : 0);
    ++o.ends;
    o.end_ns = uint64_t(t_ns);
    follow_[s] = 0;
    events.push_back({s, lost ? "lost" : "end", t_ns, o.distance, {o.point[0], o.point[1], o.point[2]}});
}

void Pinch::release(int64_t t_ns) {
    events.clear();
    for (int s = 0; s < 2; ++s) {
        side_[s].flags &= ~FH_PINCH_TRACKED;
        if (side_[s].flags & FH_PINCH_DOWN) end(s, t_ns, true);
    }
}

std::vector<int> Pinch::down() const {
    std::vector<int> out;
    for (int s = 0; s < 2; ++s)
        if (side_[s].flags & FH_PINCH_DOWN) out.push_back(follow_[s]);
    return out;
}

bool Pinch::engaged() const {
    for (const fh_pinch_t &o : side_)
        if ((o.flags & FH_PINCH_DOWN) || ((o.flags & FH_PINCH_TRACKED) && o.strength > 0.3f)) return true;
    return false;
}

// ---------------------------------------------------------------------------- grip

namespace {

constexpr int kWrist = 0, kFingers[4][2] = {{5, 8}, {9, 12}, {13, 16}, {17, 20}};   // knuckle, tip

// Each finger's curl (see GripParams), and the thumb tip's distance from the index tip (m):
// from the model's world landmarks averaged over the hand's views this step, or the
// tracker's 3D points without any.
bool curls(const Hand &h, const std::vector<Seen> &views, double out[4], double *thumb) {
    double sum[4] = {}, gap = 0;
    int n = 0;
    for (const Seen &v : views) {
        if (v.hand != h.id) continue;
        auto p = [&](int i) { return V3{v.lm.world[i][0], v.lm.world[i][1], v.lm.world[i][2]}; };
        for (int f = 0; f < 4; ++f) {
            const double k = norm(p(kFingers[f][0]) - p(kWrist));
            sum[f] += k > 1e-4 ? norm(p(kFingers[f][1]) - p(kWrist)) / k : 2;
        }
        gap += norm(p(4) - p(8)) * h.scale;
        ++n;
    }
    *thumb = n ? gap / n : norm(h.smooth[4] - h.smooth[8]);
    for (int f = 0; f < 4; ++f) {
        if (n) {
            out[f] = sum[f] / n;
            continue;
        }
        const double k = norm(h.smooth[kFingers[f][0]] - h.smooth[kWrist]);
        if (k < 1e-4) return false;
        out[f] = norm(h.smooth[kFingers[f][1]] - h.smooth[kWrist]) / k;
    }
    return true;
}

V3 palm_centre(const Hand &h) {
    return (h.smooth[0] + h.smooth[5] + h.smooth[9] + h.smooth[13] + h.smooth[17]) * 0.2;
}

}  // namespace

void Grip::update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns) {
    events.clear();
    int taken[2] = {0, 0};
    for (int s = 0; s < 2; ++s)
        if (side_[s].flags & FH_PINCH_DOWN) taken[s] = follow_[s];
    for (int s = 0; s < 2; ++s) {
        fh_pinch_t &o = side_[s];
        const bool down = o.flags & FH_PINCH_DOWN;
        const Hand *h = nullptr;
        for (const Hand *c : hands) {
            if (down ? c->id != follow_[s] : c->right() != (s == 1) || c->id == taken[1 - s]) continue;
            if (!h || c->frames > h->frames) h = c;
        }
        curl[s] = -1;
        double f[4], thumb = 1;
        if (!h || !curls(*h, views, f, &thumb)) {
            o.flags &= ~FH_PINCH_TRACKED;
            if (down && (t_ns - seen_ns_[s]) / 1e9 > p_.grace_s) end(s, t_ns, true);
            continue;
        }
        seen_ns_[s] = t_ns;
        const double mean = (f[0] + f[1] + f[2] + f[3]) / 4, most = std::max({f[0], f[1], f[2], f[3]});
        curl[s] = mean;
        const V3 point = palm_centre(*h);
        o.flags |= FH_PINCH_TRACKED;
        o.hand_id = uint32_t(h->id);
        o.distance = float(mean);
        o.strength = float(std::clamp((p_.end - mean) / (p_.end - p_.begin), 0.0, 1.0));
        put3(o.point, point);
        if (!down) {
            if (mean > p_.end) open_ns_[s] = t_ns;
            const bool ahead = -point[2] >= p_.min_ahead_m &&
                               std::atan2(-point[1], -point[2]) * 180 / M_PI <= p_.max_down_deg;
            if (most < p_.begin && ahead && thumb >= p_.thumb_off_m && open_ns_[s] && (t_ns - open_ns_[s]) / 1e9 <= p_.armed_s) {
                o.flags = (o.flags | FH_PINCH_DOWN) & ~FH_PINCH_LOST;
                ++o.begins;
                o.begin_ns = uint64_t(t_ns);
                put3(o.begin_point, point);
                follow_[s] = h->id;
                open_frames_[s] = 0;
                events.push_back({s, "begin", t_ns, mean, point});
            }
        } else if (mean > p_.end) {
            if (++open_frames_[s] >= p_.end_frames) {
                end(s, t_ns, false);
                open_ns_[s] = t_ns;
            }
        } else {
            open_frames_[s] = 0;
        }
    }
}

void Grip::end(int s, int64_t t_ns, bool lost) {
    fh_pinch_t &o = side_[s];
    o.flags = (o.flags & ~FH_PINCH_DOWN) | (lost ? FH_PINCH_LOST : 0);
    ++o.ends;
    o.end_ns = uint64_t(t_ns);
    follow_[s] = 0;
    events.push_back({s, lost ? "lost" : "end", t_ns, o.distance, {o.point[0], o.point[1], o.point[2]}});
}

void Grip::release(int64_t t_ns) {
    events.clear();
    for (int s = 0; s < 2; ++s) {
        side_[s].flags &= ~FH_PINCH_TRACKED;
        if (side_[s].flags & FH_PINCH_DOWN) end(s, t_ns, true);
    }
}

std::vector<int> Grip::gripping() const {
    std::vector<int> out;
    for (int s = 0; s < 2; ++s)
        if (side_[s].flags & FH_PINCH_DOWN) out.push_back(follow_[s]);
    return out;
}

bool Grip::engaged() const {
    for (const fh_pinch_t &o : side_)
        if ((o.flags & FH_PINCH_DOWN) || ((o.flags & FH_PINCH_TRACKED) && o.strength > 0.3f)) return true;
    return false;
}

// ----------------------------------------------------------------------- publisher

bool GesturePublisher::open(const Pinch &pinch, const Grip &grip, std::string &err) {
    const std::string path = run_dir() + "/gestures";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(fh_gestures_t)) < 0) return err = path + ": " + std::strerror(errno), false;
    void *m = mmap(nullptr, sizeof(fh_gestures_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return err = path + ": can't map it", false;
    out_ = static_cast<fh_gestures_t *>(m);
    // keep the counters a previous tracker left, so a reader doesn't see them jump back
    const bool ours = !std::memcmp(out_->magic, FH_GESTURES_MAGIC, 8) && out_->version == FH_GESTURES_VERSION;
    if (!ours) {
        std::memset(out_, 0, sizeof *out_);
        std::memcpy(out_->magic, FH_GESTURES_MAGIC, 8);
        out_->version = FH_GESTURES_VERSION;
        out_->size = sizeof(fh_gestures_t);
    }
    seq_ = out_->seq / 2 + 1;
    // a gesture the last tracker left down (it crashed) is over: count its end, as lost
    __atomic_store_n(&out_->seq, 2 * ++seq_ - 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (fh_pinch_t *slots : {out_->pinch, out_->grip, out_->pinch2})
        for (int s = 0; s < 2; ++s) {
            fh_pinch_t &o = slots[s];
            if (o.begins == o.ends) continue;
            o.ends = o.begins;
            o.end_ns = mono_ns();
            o.flags = (o.flags & ~FH_PINCH_DOWN) | FH_PINCH_LOST;
        }
    __atomic_store_n(&out_->seq, 2 * seq_, __ATOMIC_RELEASE);
    out_->begin_m = float(pinch.params().begin_m);
    out_->end_m = float(pinch.params().end_m);
    out_->grip_begin = float(grip.params().begin);
    out_->grip_end = float(grip.params().end);
    return true;
}

void GesturePublisher::write(const Pinch &pinch, const Grip &grip, uint64_t capture_ns, const Pinch *middle) {
    __atomic_store_n(&out_->seq, 2 * ++seq_ - 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    static const fh_pinch_t none{};
    for (int g = 0; g < 3; ++g)
        for (int s = 0; s < 2; ++s) {
            // counters carry on from what's in the file (a restarted tracker starts its own at 0)
            const fh_pinch_t &in = g == 2 ? (middle ? middle->side(s) : none) : g ? grip.side(s) : pinch.side(s);
            fh_pinch_t &o = g == 2 ? out_->pinch2[s] : g ? out_->grip[s] : out_->pinch[s];
            const uint32_t base_b = o.begins - last_begins_[g][s], base_e = o.ends - last_ends_[g][s];
            o = in;
            o.begins = base_b + in.begins;
            o.ends = base_e + in.ends;
            last_begins_[g][s] = in.begins, last_ends_[g][s] = in.ends;
        }
    out_->capture_ns = capture_ns;
    out_->publish_ns = mono_ns();
    __atomic_store_n(&out_->seq, 2 * seq_, __ATOMIC_RELEASE);
}
