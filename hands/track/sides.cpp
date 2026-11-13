#include "sides.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {

bool is_side(const std::string &name) { return name == "slam_left" || name == "slam_right"; }

double median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

constexpr int kMaxPairs = 12;          // per step
constexpr int kMinFront = 15;          // of 21 landmarks: rays meeting in front of both cameras
constexpr double kNear = 0.05, kFar = 1.5;      // m along each ray: where a hand can be
constexpr double kRatioLo = 0.6, kRatioHi = 1.9;   // as Tracker's size check (tracker.cpp)
constexpr double kProbeDepths[] = {0.8, 1.0, 1.25};   // probe(): times the one-view distance

}  // namespace

SideCheck::SideCheck(const std::map<std::string, Camera> &cams) {
    for (const auto &[name, cam] : cams) cams_[name] = &cam;
    if (cams_.count("slam_left")) left_ = cams_["slam_left"];
    if (cams_.count("slam_right")) right_ = cams_["slam_right"];
}

const Camera *SideCheck::cam(const std::string &name, bool swapped) const {
    if (swapped && name == "slam_left") return right_;
    if (swapped && name == "slam_right") return left_;
    const auto it = cams_.find(name);
    return it == cams_.end() ? nullptr : it->second;
}

double SideCheck::miss(const Camera &ca, const Landmarks &a, const Camera &cb, const Landmarks &b) const {
    std::vector<double> d;
    V3 palm{};
    bool have_palm = false;
    for (int k = 0; k < 21; ++k) {
        const V3 oa = ca.origin, da = ca.ray(a.pts[k]), ob = cb.origin, db = cb.ray(b.pts[k]);
        const V3 w = oa - ob;
        const double ab = dot(da, db), p = dot(da, w), q = dot(db, w), den = 1 - ab * ab;
        if (den < 1e-9) continue;   // parallel rays
        const double ta = (ab * q - p) / den, tb = (q - ab * p) / den;
        if (ta < kNear || tb < kNear || ta > kFar || tb > kFar) continue;
        const V3 pa = oa + da * ta, pb = ob + db * tb;
        d.push_back(norm(pa - pb));
        if (k == 9) palm = (pa + pb) * 0.5, have_palm = true;
    }
    if (int(d.size()) < kMinFront || !have_palm) return -1;
    // as far from each camera as the hand's apparent size says (a hand of the model's size)
    for (const auto &[c, lm] : {std::pair<const Camera *, const Landmarks *>{&ca, &a}, {&cb, &b}}) {
        V3 mono[21];
        if (!Tracker::single_view(*c, *lm, 1.0, mono)) return -1;
        const double r = norm(palm - c->origin) / std::max(norm(mono[9] - c->origin), 1e-6);
        if (r < kRatioLo || r > kRatioHi) return -1;
    }
    return median(d);
}

SideCheck::Miss SideCheck::test(const std::string &cam_a, const Landmarks &a, const std::string &cam_b,
                                const Landmarks &b) const {
    Miss out{{-1, -1}};
    for (int way = 0; way < 2; ++way) {
        const Camera *ca = cam(cam_a, way == 1), *cb = cam(cam_b, way == 1);
        if (ca && cb) out.m[way] = miss(*ca, a, *cb, b);
    }
    return out;
}

int SideCheck::vote(const Miss &m) {
    for (int w = 0; w < 2; ++w) {
        const double win = m.m[w], other = m.m[1 - w];
        if (win >= 0 && win < kGood && (other < 0 || other > kClear * win)) return w;
    }
    return -1;
}

int SideCheck::add(const std::vector<Seen> &views, int64_t t_ns) {
    if (!usable()) return 0;
    std::vector<const Seen *> vs;
    for (const Seen &v : views)
        if (cams_.count(v.cam)) vs.push_back(&v);
    int pairs = 0, voted = 0;
    for (size_t i = 0; i < vs.size(); ++i)
        for (size_t j = i + 1; j < vs.size() && pairs < kMaxPairs; ++j) {
            if (vs[i]->cam == vs[j]->cam || (!is_side(vs[i]->cam) && !is_side(vs[j]->cam))) continue;
            ++pairs;
            const Miss m = test(vs[i]->cam, vs[i]->lm, vs[j]->cam, vs[j]->lm);
            const int v = vote(m);
            if (v < 0) continue;
            ++votes[v], ++voted;
            misses_[0].push_back(m.m[0]), misses_[1].push_back(m.m[1]);
            if (first_ns_ < 0) first_ns_ = t_ns;
            last_ns_ = t_ns;
        }
    return voted;
}

std::vector<Seen> SideCheck::probe(const Nets &nets, Pool &pool, const std::map<std::string, Image> &images,
                                   const std::vector<Seen> &views, int64_t t_ns) {
    if (!usable() || (probe_ns_ >= 0 && (t_ns - probe_ns_) / 1e9 < probe_interval_s - 0.01)) return {};
    struct Job {
        int way;
        std::string cam;
        Roi roi;
        Landmarks lm;
    };
    std::vector<Job> jobs;
    // the views to look from: confident ones, a different hand each time (round robin)
    std::vector<const Seen *> order;
    for (const Seen &v : views)
        if (cams_.count(v.cam) && v.lm.presence >= 0.5) order.push_back(&v);
    std::sort(order.begin(), order.end(), [](const Seen *a, const Seen *b) { return a->lm.presence > b->lm.presence; });
    if (!order.empty()) std::rotate(order.begin(), order.begin() + (probe_turn_++ % order.size()), order.end());
    // Only the other naming: the tracker hands hands over to the other cameras under the names
    // as they are every step, and add() tests what that finds.
    for (int way = 1; way < 2; ++way) {
        bool found = false;
        for (const Seen *v : order) {
            for (const char *target : {"slam_left", "slam_right"}) {
                if (v->cam == target || !images.count(target)) continue;
                bool paired = false;   // the tracker has this hand there already: add() tests that pair
                for (const Seen &u : views) paired = paired || (u.cam == target && u.hand == v->hand && v->hand > 0);
                if (paired) continue;
                const Camera *cs = cam(v->cam, way == 1), *ct = cam(target, way == 1);
                V3 pts[21];
                if (!cs || !ct || !Tracker::single_view(*cs, v->lm, 1.0, pts)) continue;
                // the hand at a few distances around the one-view guess, as the tracker hands
                // a hand over to another camera (a crop around where its landmarks land)
                const V3 axis{ct->R[0][2], ct->R[1][2], ct->R[2][2]}, o = cs->origin;
                for (double f : kProbeDepths) {
                    V2 uv[21];
                    bool visible = true;
                    for (int k = 0; k < 21; ++k) {
                        const V3 p = o + (pts[k] - o) * f;
                        visible = visible && dot(unit(p - ct->origin), axis) > 0.17;   // within 80 degrees
                        uv[k] = ct->project(p, nullptr);
                    }
                    const V2 c = uv[9];
                    if (!visible || c[0] < 0 || c[1] < 0 || c[0] >= ct->width || c[1] >= ct->height) continue;
                    jobs.push_back({way, target, roi_from_points(uv), {}});
                    found = true;
                }
                if (found) break;
            }
            if (found) break;
        }
    }
    if (jobs.empty()) return {};
    probe_ns_ = t_ns;
    std::vector<std::function<void()>> run;
    for (Job &j : jobs) run.push_back([&nets, &images, &j] { j.lm = nets.landmarks(images.at(j.cam), j.roi); });
    pool.run(run);
    static const bool debug = std::getenv("FH_SIDES_DEBUG") != nullptr;
    std::vector<Seen> out;
    probes += int(jobs.size());
    for (int way = 1; way < 2; ++way) {   // the best look
        const Job *best = nullptr;
        for (const Job &j : jobs)
            if (j.way == way && (!best || j.lm.presence > best->lm.presence)) best = &j;
        if (debug && best)
            std::fprintf(stderr, "side probe (%s) %s at %.0f %.0f size %.0f: presence %.2f\n", way ? "swapped" : "as named",
                         best->cam.c_str(), best->roi.center[0], best->roi.center[1], best->roi.size, best->lm.presence);
        if (!best || best->lm.presence < 0.5) continue;
        ++probe_hits;
        out.push_back(Seen{best->cam, 0, best->roi, best->lm, {}});
    }
    return out;
}

SideCheck::Verdict SideCheck::verdict() const {
    const int total = votes[0] + votes[1], w = votes[0] >= votes[1] ? 0 : 1;
    const bool clean = votes[1 - w] == 0 && votes[w] >= min_clean;
    const bool clear = votes[w] >= min_votes && votes[1 - w] <= max_other * total;
    if (!(clean || clear) || span_s() < min_span_s) return Undecided;
    return w == 0 ? AsNamed : Swapped;
}

void SideCheck::reset() {
    votes[0] = votes[1] = 0;
    probes = probe_hits = 0;
    probe_ns_ = -1;
    misses_[0].clear(), misses_[1].clear();
    first_ns_ = last_ns_ = -1;
}

double SideCheck::median_miss_mm(int way) const {
    std::vector<double> v;
    for (double m : misses_[way])
        if (m >= 0) v.push_back(m * 1000);
    return median(v);
}

std::string SideCheck::summary() const {
    char s[320];
    std::snprintf(s, sizeof s,
                  "votes as named %d, swapped %d, over %.1f s; median ray miss as named %.1f mm, swapped %.1f mm "
                  "(-1: never met); probes %d, found %d",
                  votes[0], votes[1], span_s(), median_miss_mm(0), median_miss_mm(1), probes, probe_hits);
    return s;
}

std::string SideCheck::json() const {
    char s[320];
    std::snprintf(s, sizeof s,
                  "{\"as_named\": %d, \"swapped\": %d, \"seconds\": %.2f, \"miss_mm\": [%.1f, %.1f], \"probes\": %d, "
                  "\"found\": %d}",
                  votes[0], votes[1], span_s(), median_miss_mm(0), median_miss_mm(1), probes, probe_hits);
    return s;
}
