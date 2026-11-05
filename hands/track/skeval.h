// ft-handreplay's measures of the skeleton fit (track/skeleton.h): every published hand's
// measured landmarks, the One Euro filtered ones (what version 1 published) and the fitted
// skeleton's, compared after the replay. See hands/README.md, "Skeleton".
#pragma once

#include "skeleton.h"
#include "tracker.h"

#include <json/json.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace skeval {

inline double quantile(std::vector<double> v, double q) {
    if (v.empty()) return NAN;
    const size_t k = std::min(v.size() - 1, size_t(q * v.size()));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct Rec {
    int id;
    double ts;
    bool stereo, fit_ok, right;
    double palm_speed;   // the tracker's filtered palm speed, m/s
    V3 pts[4][21];       // measured, One Euro, skeleton (filtered, published), skeleton before filtering
    double rms, us;
    V3 elbow;
    V3 normal[2];        // the back of the hand's normal: from the One Euro landmarks, the skeleton's palm frame
    V3 up;               // the head's up from the HMD pose, or 0
    // the fit's inputs, to time it again on its own (bench)
    double right_score, residual, scale, res_pt[21];
    int nviews;
    V3 eye;
    int64_t t_ns;
};

struct Label {
    bool right;
    double conf;
    V3 kp[21];
};

constexpr int kBones[20][2] = {{0, 1},   {1, 2},   {2, 3},   {3, 4},   {0, 5},   {5, 6},   {6, 7},
                               {7, 8},   {0, 9},   {9, 10},  {10, 11}, {11, 12}, {0, 13}, {13, 14},
                               {14, 15}, {15, 16}, {0, 17},  {17, 18}, {18, 19}, {19, 20}};


inline V3 palm(const V3 *p) { return (p[0] + p[5] + p[9] + p[13] + p[17]) * 0.2; }

class Eval {
public:
    std::vector<Rec> recs;
    std::map<int64_t, std::vector<Label>> labels;   // by set capture time
    // per side: when the calibration settled (s into the replay, -1 never) and the shape then
    double settle_ts[2] = {-1, -1};
    skel::Shape settled_shape[2];
    double first_ts[2] = {-1, -1};

    bool load_labels(const std::string &path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        Json::CharReaderBuilder b;
        while (std::getline(f, line)) {
            Json::Value v;
            std::string e;
            std::istringstream in(line);
            if (!Json::parseFromStream(b, in, &v, &e)) continue;
            std::vector<Label> hs;
            for (const Json::Value &h : v["hands"]) {
                Label l{h["side"].asString() == "R", h["conf"].asDouble(), {}};
                for (int i = 0; i < 21; ++i)
                    for (int k = 0; k < 3; ++k) l.kp[i][k] = h["kp3d"][i][k].asDouble();
                hs.push_back(l);
            }
            labels[v["t_ns"].asInt64()] = hs;
        }
        return true;
    }

    void add(const Hand &h, double ts, V3 up) {
        Rec r;
        r.id = h.id, r.ts = ts, r.stereo = h.nviews >= 2, r.fit_ok = h.fit.valid, r.right = h.right();
        r.palm_speed = norm(h.dpalm);
        for (int i = 0; i < 21; ++i) r.pts[0][i] = h.pts[i], r.pts[1][i] = h.smooth[i], r.pts[2][i] = h.fit.valid ? h.fit.pts[i] : h.smooth[i];
        for (int i = 0; i < 21; ++i) r.pts[3][i] = h.fit.valid ? h.fit.raw[i] : h.smooth[i];
        {
            const V3 *p = h.smooth;
            const V3 a = p[5] - p[0], b = p[17] - p[0];
            const V3 c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            r.normal[0] = unit(c) * (h.right() ? 1.0 : -1.0);
            r.normal[1] = h.fit.valid ? h.fit.frames[0].col(1) : r.normal[0];
        }
        r.right_score = h.right_score, r.residual = h.residual, r.scale = h.scale, r.nviews = h.nviews, r.eye = h.eye;
        r.t_ns = int64_t(ts * 1e9);
        std::copy(h.res_pt, h.res_pt + 21, r.res_pt);
        r.rms = h.fit.rms, r.us = h.fit.us, r.elbow = h.fit.elbow, r.up = up;
        recs.push_back(r);
    }

    void calibration(const skel::Skeleton &sk, double ts) {
        for (int s = 0; s < 2; ++s) {
            const skel::Calibration &c = sk.calibration(s ? 1 : -1);
            if (first_ts[s] < 0 && c.samples(0) > 0) first_ts[s] = ts;
            if (settle_ts[s] < 0 && c.settled() >= 1) settle_ts[s] = ts, settled_shape[s] = sk.shape(s ? 1 : -1, 1.0);
        }
    }

    std::string dump;   // ft-handreplay --skeleton-dump: a line per hand matched to a label

    // The fit's cost on its own: every hand update again, in order, through a fresh Skeleton on
    // this thread, without the models' threads competing (the replay's own times include them),
    // pinned to a little core (CPU 0, a Cortex-A520 on the Frame) and a big one (CPU 3, an A720).
    void bench(const skel::Params &p) const {
        cpu_set_t was;
        pthread_getaffinity_np(pthread_self(), sizeof was, &was);
        for (int cpu : {0, 3}) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            if (pthread_setaffinity_np(pthread_self(), sizeof one, &one) != 0) continue;
            for (int round = 0; round < 2; ++round) {   // the first warms the caches
                skel::Skeleton sk(p);
                std::map<int, skel::Fit> fits;
                std::vector<double> us;
                for (const Rec &r : recs) {
                    skel::Fit &f = fits[r.id];
                    const auto a = std::chrono::steady_clock::now();
                    sk.update(f, r.pts[0], r.right, r.right_score, r.nviews, r.residual, r.res_pt, r.eye, r.scale, r.t_ns);
                    us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
                }
                if (round)
                    std::printf("fit time on its own, CPU %d (%zu hand updates again): median %.0f us, 90%% %.0f us, 99%% %.0f us, max %.0f us\n",
                                cpu, us.size(), quantile(us, 0.5), quantile(us, 0.9), quantile(us, 0.99),
                                us.empty() ? 0.0 : *std::max_element(us.begin(), us.end()));
            }
        }
        pthread_setaffinity_np(pthread_self(), sizeof was, &was);
    }

    void report(const skel::Skeleton &sk, int64_t t0, const std::vector<std::pair<int64_t, int>> &label_hits) const {
        FILE *dp = dump.empty() ? nullptr : std::fopen(dump.c_str(), "w");
        std::printf("\n-- skeleton fit (track/skeleton.h) --\n");
        // 1. cost
        std::vector<double> us, rms[2];
        int fits = 0;
        for (const Rec &r : recs)
            if (r.fit_ok) us.push_back(r.us), rms[r.stereo].push_back(r.rms * 1000), ++fits;
        std::printf("fits %d of %zu hand updates, %d fitted as the other side; time per hand: median %.0f us, 90%% %.0f us, max %.0f us\n",
                    fits, recs.size(), sk.flips, quantile(us, 0.5), quantile(us, 0.9), us.empty() ? 0.0 : *std::max_element(us.begin(), us.end()));
        bench(sk.params());
        std::printf("fit rms against the measured landmarks: stereo median %.1f mm, 90%% %.1f mm; one view median %.1f mm, 90%% %.1f mm\n",
                    quantile(rms[1], 0.5), quantile(rms[1], 0.9), quantile(rms[0], 0.5), quantile(rms[0], 0.9));
        {
            std::vector<double> e[21];
            for (const Rec &r : recs)
                if (r.fit_ok && r.stereo)
                    for (int i = 0; i < 21; ++i) e[i].push_back(norm(r.pts[3][i] - r.pts[0][i]) * 1000);
            std::printf("    stereo fit error per landmark, median mm:");
            for (int i = 0; i < 21; ++i) std::printf(" %.1f", quantile(e[i], 0.5));
            std::printf("\n");
        }
        // 2. calibration
        for (int s = 0; s < 2; ++s) {
            const skel::Calibration &c = sk.calibration(s ? 1 : -1);
            const skel::Shape now = sk.shape(s ? 1 : -1, 1.0);
            std::printf("%s calibration: settled %.2f", s ? "right" : "left ", c.settled());
            if (settle_ts[s] >= 0) {
                double drift = 0;
                for (int b = 0; b < 20; ++b) drift = std::max(drift, std::fabs(now.len[b] - settled_shape[s].len[b]));
                std::printf(" at %.1f s (%.1f s after its first sample); largest bone change since %.1f mm", settle_ts[s],
                            settle_ts[s] - first_ts[s], drift * 1000);
            }
            std::printf("\n    bones mm:");
            for (int b = 0; b < 20; ++b) std::printf(" %.1f", now.len[b] * 1000);
            std::printf("\n");
        }
        // 3. bone-length stability: coefficient of variation per hand track and bone
        {
            std::map<int, std::vector<const Rec *>> by;
            for (const Rec &r : recs) by[r.id].push_back(&r);
            std::vector<double> cv[3], cv_stereo;
            for (auto &[id, rs] : by) {
                if (rs.size() < 30) continue;
                for (int b = 0; b < 20; ++b) {
                    for (int v = 0; v < 3; ++v) {
                        double s = 0, s2 = 0;
                        for (const Rec *r : rs) {
                            const double l = norm(r->pts[v][kBones[b][1]] - r->pts[v][kBones[b][0]]);
                            s += l, s2 += l * l;
                        }
                        const double m = s / rs.size(), var = std::max(0.0, s2 / rs.size() - m * m);
                        cv[v].push_back(m > 0 ? std::sqrt(var) / m : 0);
                    }
                    double s = 0, s2 = 0;
                    int n = 0;
                    for (const Rec *r : rs)
                        if (r->stereo) {
                            const double l = norm(r->pts[0][kBones[b][1]] - r->pts[0][kBones[b][0]]);
                            s += l, s2 += l * l, ++n;
                        }
                    if (n > 10) cv_stereo.push_back(std::sqrt(std::max(0.0, s2 / n - s * s / n / n)) / (s / n));
                }
            }
            std::printf("bone length CV (per track and bone, tracks of 30+ updates): measured median %.3f (stereo only %.3f), "
                        "One Euro %.3f, skeleton %.3f; 90%%: %.3f, %.3f, %.3f\n",
                        quantile(cv[0], 0.5), quantile(cv_stereo, 0.5), quantile(cv[1], 0.5), quantile(cv[2], 0.5),
                        quantile(cv[0], 0.9), quantile(cv[1], 0.9), quantile(cv[2], 0.9));
        }
        // 4. jitter: off a straight line through the last two updates (as the replay's palm
        // jitter), for the palm and the thumb and index tips, at rest and moving
        {
            std::map<int, std::vector<const Rec *>> by;
            for (const Rec &r : recs) by[r.id].push_back(&r);
            const char *what[3] = {"palm", "index tip", "thumb tip"};
            for (int w = 0; w < 3; ++w) {
                std::vector<double> j[2][4];   // rest/moving, variant
                for (auto &[id, rs] : by)
                    for (size_t k = 2; k < rs.size(); ++k) {
                        const Rec &a = *rs[k - 2], &b = *rs[k - 1], &c = *rs[k];
                        if (c.ts - b.ts >= 0.1 || b.ts - a.ts >= 0.1 || b.ts - a.ts < 1e-3) continue;
                        // at rest: the palm still, and the fingers too (typing moves them on a still
                        // palm: that's motion, not jitter); by a centred average of the measured tips
                        bool fingers_still = false;
                        if (k >= 4 && k + 2 < rs.size() && rs[k + 2]->ts - rs[k - 4]->ts < 0.35) {
                            auto rel = [&](size_t i) {
                                V3 s{};
                                for (size_t j = i - 2; j <= i + 2; ++j)
                                    s = s + (rs[j]->pts[0][8] - palm(rs[j]->pts[0])) * 0.1 + (rs[j]->pts[0][4] - palm(rs[j]->pts[0])) * 0.1;
                                return s;
                            };
                            fingers_still = norm(rel(k) - rel(k - 2)) / (rs[k]->ts - rs[k - 2]->ts) < 0.1;
                        }
                        const int moving = c.palm_speed > 0.3 ? 1 : c.palm_speed < 0.1 && fingers_still ? 0 : -1;
                        if (moving < 0) continue;
                        const double f = (c.ts - b.ts) / (b.ts - a.ts);
                        for (int v = 0; v < 4; ++v) {
                            auto P = [&](const Rec &r) { return w == 0 ? palm(r.pts[v]) : r.pts[v][w == 1 ? 8 : 4]; };
                            j[moving][v].push_back(norm(P(c) - P(b) - (P(b) - P(a)) * f) * 1000);
                        }
                    }
                for (int m = 0; m < 2; ++m)
                    std::printf("%-9s jitter %s (%zu): median measured %.1f, One Euro %.1f, skeleton %.1f (unfiltered %.1f) mm; "
                                "90%% %.1f, %.1f, %.1f (%.1f)\n",
                                what[w], m ? "moving" : "at rest (fingers too)", j[m][0].size(), quantile(j[m][0], 0.5), quantile(j[m][1], 0.5),
                                quantile(j[m][2], 0.5), quantile(j[m][3], 0.5), quantile(j[m][0], 0.9), quantile(j[m][1], 0.9),
                                quantile(j[m][2], 0.9), quantile(j[m][3], 0.9));
            }
        }
        // the palm's normal, update to update: still, and with the fingers moving on a still palm
        // (a pinch: ray and grab orientation want it steady)
        {
            std::map<int, std::vector<const Rec *>> by;
            for (const Rec &r : recs) by[r.id].push_back(&r);
            std::vector<double> d[2][2];   // still / fingers moving, One Euro / skeleton
            for (auto &[id, rs] : by)
                for (size_t k = 1; k < rs.size(); ++k) {
                    const Rec &a = *rs[k - 1], &b = *rs[k];
                    if (b.ts - a.ts >= 0.1 || !a.fit_ok || !b.fit_ok || b.palm_speed > 0.1) continue;
                    const double tip = norm((b.pts[0][8] - palm(b.pts[0])) - (a.pts[0][8] - palm(a.pts[0]))) / (b.ts - a.ts);
                    const int fingers = tip > 0.3 ? 1 : tip < 0.1 ? 0 : -1;
                    if (fingers < 0) continue;
                    for (int v = 0; v < 2; ++v)
                        d[fingers][v].push_back(std::acos(std::clamp(dot(a.normal[v], b.normal[v]), -1.0, 1.0)) * 180 / M_PI);
                }
            for (int f = 0; f < 2; ++f)
                std::printf("palm normal change per update, palm still, fingers %s (%zu): median One Euro %.2f, skeleton %.2f deg; "
                            "90%% %.2f, %.2f\n", f ? "moving" : "still", d[f][0].size(), quantile(d[f][0], 0.5),
                            quantile(d[f][1], 0.5), quantile(d[f][0], 0.9), quantile(d[f][1], 0.9));
        }
        // 5. lag: the published tip against a centred average of the measured one (+-2 updates,
        // so it doesn't lag), along the tip's motion, where the tip moves fast
        {
            std::map<int, std::vector<const Rec *>> by;
            for (const Rec &r : recs) by[r.id].push_back(&r);
            for (int rel = 0; rel < 2; ++rel) {
                std::vector<double> lag[4], err[4];
                for (auto &[id, rs] : by)
                    for (size_t k = 3; k + 3 < rs.size(); ++k) {
                        bool ok = true;
                        for (size_t i = k - 3; i < k + 3; ++i) ok = ok && rs[i + 1]->ts - rs[i]->ts < 0.1;
                        if (!ok) continue;
                        for (int tip : {4, 8}) {
                            // relative to the palm (fingers moving on a still hand) or in the head frame
                            auto P = [&](const Rec &r, int v) { return rel ? r.pts[v][tip] - palm(r.pts[v]) : r.pts[v][tip]; };
                            auto ref = [&](size_t i) {
                                V3 s{};
                                for (size_t j = i - 2; j <= i + 2; ++j) s = s + P(*rs[j], 0) * 0.2;
                                return s;
                            };
                            const V3 vel = (ref(k + 1) - ref(k - 1)) * (1 / (rs[k + 1]->ts - rs[k - 1]->ts));
                            if (norm(vel) < (rel ? 0.3 : 0.5)) continue;
                            const V3 truth = ref(k), dir = unit(vel);
                            for (int v = 0; v < 4; ++v) {
                                lag[v].push_back(dot(truth - P(*rs[k], v), dir) * 1000);
                                err[v].push_back(norm(truth - P(*rs[k], v)) * 1000);
                            }
                        }
                    }
                std::printf("tip lag, %s (%zu): median measured %.1f, One Euro %.1f, skeleton %.1f (unfiltered %.1f) mm behind; "
                            "distance median %.1f, %.1f, %.1f (%.1f) mm\n",
                            rel ? "moving against the palm faster than 0.3 m/s" : "moving faster than 0.5 m/s", lag[0].size(),
                            quantile(lag[0], 0.5), quantile(lag[1], 0.5), quantile(lag[2], 0.5), quantile(lag[3], 0.5),
                            quantile(err[0], 0.5), quantile(err[1], 0.5), quantile(err[2], 0.5), quantile(err[3], 0.5));
            }
        }
        // 6. the elbow: how far it turns from the stub (the hand's axis carried on), and from the
        // same arm model with gravity's up (when the HMD poses are known)
        {
            std::vector<double> stub, grav, len;
            for (const Rec &r : recs) {
                if (!r.fit_ok) continue;
                const V3 w = r.pts[2][0], d = unit(r.elbow - w), s = unit(w - r.pts[2][9]);
                stub.push_back(std::acos(std::clamp(dot(d, s), -1.0, 1.0)) * 180 / M_PI);
                len.push_back(norm(r.elbow - w) * 1000);
                if (norm(r.up) > 0.5) {
                    const V3 e = skel::Skeleton::elbow(w, r.pts[2][9], r.right ? 1 : -1, norm(r.elbow - w), r.up);
                    grav.push_back(std::acos(std::clamp(dot(d, unit(e - w)), -1.0, 1.0)) * 180 / M_PI);
                }
            }
            std::printf("elbow: forearm median %.0f mm; angle from the stub median %.0f deg, 90%% %.0f", quantile(len, 0.5),
                        quantile(stub, 0.5), quantile(stub, 0.9));
            if (!grav.empty())
                std::printf("; head-up against gravity-up arm model median %.1f deg, 90%% %.1f (%zu)", quantile(grav, 0.5),
                            quantile(grav, 0.9), grav.size());
            std::printf("\n");
        }
        // 7. the labels (frame-hands' auto-labels: one teacher, triangulated, smoothed and fitted;
        // not ground truth)
        if (!labels.empty()) {
            std::vector<double> e[4], e_tip[4], per[2][21], by_views[2][2][2];   // [stereo][measured/skeleton][all/tip]
            int matched = 0, unmatched = 0;
            std::map<int, std::vector<double>> label_len[2];   // per side, bone -> label lengths
            for (auto &[t, n] : label_hits) {
                (void)n;
                const auto it = labels.find(t);
                if (it == labels.end()) continue;
                const double ts = (t - t0) / 1e9;
                for (const Rec &r : recs) {
                    if (std::fabs(r.ts - ts) > 1e-6) continue;
                    const Label *best = nullptr;
                    double bd = 0.06;
                    for (const Label &l : it->second) {
                        const double d = norm(l.kp[0] - r.pts[0][0]);
                        if (l.conf >= 0.3 && d < bd) bd = d, best = &l;
                    }
                    if (!best) {
                        ++unmatched;
                        continue;
                    }
                    ++matched;
                    if (dp) {   // time id stereo, then measured, unfiltered fit and label landmarks
                        std::fprintf(dp, "%.4f %d %d", r.ts, r.id, int(r.stereo));
                        for (const V3 *P : {r.pts[0], r.pts[3], best->kp})
                            for (int i = 0; i < 21; ++i) std::fprintf(dp, " %.4f %.4f %.4f", P[i][0], P[i][1], P[i][2]);
                        std::fputc('\n', dp);
                    }
                    for (int v = 0; v < 4; ++v) {
                        double s = 0;
                        for (int i = 0; i < 21; ++i) s += norm(r.pts[v][i] - best->kp[i]) / 21;
                        e[v].push_back(s * 1000);
                        e_tip[v].push_back(norm(r.pts[v][8] - best->kp[8]) * 1000);
                        if (v == 0 || v == 3) {
                            by_views[r.stereo][v ? 1 : 0][0].push_back(s * 1000);
                            by_views[r.stereo][v ? 1 : 0][1].push_back(norm(r.pts[v][8] - best->kp[8]) * 1000);
                        }
                        if (v == 0 || v == 3)
                            for (int i = 0; i < 21; ++i) per[v ? 1 : 0][i].push_back(norm(r.pts[v][i] - best->kp[i]) * 1000);
                    }
                    for (int b = 0; b < 20; ++b)
                        label_len[r.right][b].push_back(norm(best->kp[kBones[b][1]] - best->kp[kBones[b][0]]));
                }
            }
            std::printf("against the labels (%d hands matched within 6 cm of the wrist, %d not): mean landmark error median "
                        "measured %.1f, One Euro %.1f, skeleton %.1f (unfiltered %.1f) mm (90%% %.1f, %.1f, %.1f, %.1f); "
                        "index tip median %.1f, %.1f, %.1f (%.1f) mm\n",
                        matched, unmatched, quantile(e[0], 0.5), quantile(e[1], 0.5), quantile(e[2], 0.5), quantile(e[3], 0.5),
                        quantile(e[0], 0.9), quantile(e[1], 0.9), quantile(e[2], 0.9), quantile(e[3], 0.9), quantile(e_tip[0], 0.5),
                        quantile(e_tip[1], 0.5), quantile(e_tip[2], 0.5), quantile(e_tip[3], 0.5));
            for (int st = 1; st >= 0; --st)
                std::printf("    %s (%zu): mean landmark error median measured %.1f, skeleton unfiltered %.1f mm; index tip %.1f, %.1f mm\n",
                            st ? "stereo" : "one view", by_views[st][0][0].size(), quantile(by_views[st][0][0], 0.5),
                            quantile(by_views[st][1][0], 0.5), quantile(by_views[st][0][1], 0.5), quantile(by_views[st][1][1], 0.5));
            for (int v = 0; v < 2; ++v) {
                std::printf("    per landmark, %s, median mm:", v ? "skeleton unfiltered" : "measured");
                for (int i = 0; i < 21; ++i) std::printf(" %.1f", quantile(per[v][i], 0.5));
                std::printf("\n");
            }
            if (dp) std::fclose(dp);
            for (int s = 0; s < 2; ++s) {
                if (label_len[s].empty()) continue;
                const skel::Shape now = sk.shape(s ? 1 : -1, 1.0);
                std::printf("    %s bones, ours - the labels' median, mm:", s ? "right" : "left ");
                for (int b = 0; b < 20; ++b) std::printf(" %+.1f", (now.len[b] - quantile(label_len[s].at(b), 0.5)) * 1000);
                std::printf("\n");
            }
        }
    }
};

}  // namespace skeval
