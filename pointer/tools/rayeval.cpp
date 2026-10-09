// rayeval: the hand ray (hands/include/fh_ray.h) run offline over a recording, to see how
// often it shows, why it doesn't, how much it wobbles, and whether pinches would be ray pinches
// or gaze pinches. Probe P9 of frame-hands/notes/hand-interaction.md, without the headset.
//
//   rayeval RAY.txt PROBE.jsonl [--side L|R|both] [--near-panel] [--csv FILE] [--set NAME=VALUE...]
//
// RAY.txt: ft-handreplay DIR --ray RAY.txt (per set and hand: the points the helper would read).
// PROBE.jsonl: the ft-handtest probe's log from the same session (frame-hands/captures/probe-*),
// which has the head's pose at about 90 Hz on CLOCK_MONOTONIC and the panel it showed. The
// recording's capture times are CLOCK_MONOTONIC_RAW: the offset is found by matching the sets
// the live tracker published (the probe's "cap") with the recorded ones.
// --near-panel: count a hand as near (ft-screens' near claim: the index tip over the probe's
// panel, within 15 cm in front of it) as the helper would hear from ft-screens.
// --csv: per hand frame: t, side, shown, the tests' values, ray yaw and pitch (raw, filtered).
// --set: an fh_ray Params value (predict, minCutoff, beta, dCutoff, palmShow, palmHide, raisedShow,
// raisedHide, forwardShow, forwardHide, showFor, hideFor), to try settings.
//
// Build: g++ -std=c++17 -O2 -I. pointer/tools/rayeval.cpp -o build/rayeval (no dependencies).
#include "hands/include/fh_ray.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fhray;

namespace {

struct M34 {
    double m[3][4] = {};
};
V3 Pos(const M34 &h) { return {h.m[0][3], h.m[1][3], h.m[2][3]}; }
V3 Rot(const M34 &h, V3 v) {
    return {h.m[0][0] * v.x + h.m[0][1] * v.y + h.m[0][2] * v.z, h.m[1][0] * v.x + h.m[1][1] * v.y + h.m[1][2] * v.z,
            h.m[2][0] * v.x + h.m[2][1] * v.y + h.m[2][2] * v.z};
}
V3 RotInv(const M34 &h, V3 v) {
    return {h.m[0][0] * v.x + h.m[1][0] * v.y + h.m[2][0] * v.z, h.m[0][1] * v.x + h.m[1][1] * v.y + h.m[2][1] * v.z,
            h.m[0][2] * v.x + h.m[1][2] * v.y + h.m[2][2] * v.z};
}
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 QRot(const double q[4], V3 v) {
    const V3 u{q[0], q[1], q[2]};
    const double s = q[3];
    return u * (2 * Dot(u, v)) + v * (s * s - Dot(u, u)) + Cross(u, v) * (2 * s);
}

// The numbers after `"key":` (a number or a [list]).
bool Numbers(const std::string &line, const char *key, std::vector<double> &out) {
    out.clear();
    const std::string k = std::string("\"") + key + "\":";
    size_t at = line.find(k);
    if (at == std::string::npos) return false;
    at += k.size();
    const bool list = line[at] == '[';
    if (list) ++at;
    const char *p = line.c_str() + at;
    while (true) {
        char *end;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        out.push_back(v);
        p = end;
        if (!list || *p != ',') break;
        ++p;
    }
    return !out.empty();
}

struct Frame {
    uint64_t t;  // raw ns
    int id, side;
    bool stereo, fitted, pinch, grip, began;
    V3 pts[21];
    double q[4];
    V3 elbow;
};

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s RAY.txt PROBE.jsonl [--side L|R|both] [--near-panel] [--csv FILE]\n", argv[0]);
        return 2;
    }
    int only = -1;
    bool nearPanel = false;
    const char *csvPath = nullptr;
    std::vector<std::string> sets;  // --set name=value: fh_ray Params
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--side") && i + 1 < argc) {
            const char *s = argv[++i];
            only = !std::strcmp(s, "L") ? 0 : !std::strcmp(s, "R") ? 1 : -1;
        } else if (!std::strcmp(argv[i], "--near-panel")) nearPanel = true;
        else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csvPath = argv[++i];
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) sets.push_back(argv[++i]);
    }
    Params params;
    for (const std::string &kv : sets) {
        const size_t eq = kv.find('=');
        const std::string k = kv.substr(0, eq);
        const double v = eq == std::string::npos ? 0 : std::atof(kv.c_str() + eq + 1);
        double *f = k == "predict" ? &params.predict : k == "minCutoff" ? &params.minCutoff : k == "beta" ? &params.beta
                  : k == "dCutoff" ? &params.dCutoff : k == "palmShow" ? &params.palmShow : k == "palmHide" ? &params.palmHide
                  : k == "raisedShow" ? &params.raisedShow : k == "raisedHide" ? &params.raisedHide
                  : k == "forwardShow" ? &params.forwardShow : k == "forwardHide" ? &params.forwardHide
                  : k == "showFor" ? &params.showFor : k == "hideFor" ? &params.hideFor : nullptr;
        if (!f) {
            std::fprintf(stderr, "unknown --set %s\n", k.c_str());
            return 2;
        }
        *f = v;
    }
    // the probe: head poses by time, the published capture times, the panel
    std::vector<std::pair<uint64_t, M34>> heads;
    std::vector<uint64_t> caps;
    M34 panel;
    double panelW = 0, panelH = 0;
    {
        std::ifstream in(argv[2]);
        std::string line;
        std::vector<double> v;
        while (std::getline(in, line)) {
            if (line.find("\"panel\"") != std::string::npos) {
                const size_t at = line.find("\"panel\"");
                const std::string rest = line.substr(at);
                if (Numbers(rest, "pose", v) && v.size() == 12)
                    for (int k = 0; k < 12; ++k) panel.m[k / 4][k % 4] = v[k];
                if (Numbers(rest, "width", v)) panelW = v[0];
                if (Numbers(rest, "height", v)) panelH = v[0];
                continue;
            }
            std::vector<double> t, cap, h;
            if (!Numbers(line, "t", t) || !Numbers(line, "head", h) || h.size() != 12) continue;
            M34 m;
            for (int k = 0; k < 12; ++k) m.m[k / 4][k % 4] = h[k];
            heads.push_back({uint64_t(t[0]), m});
            if (Numbers(line, "cap", cap)) caps.push_back(uint64_t(cap[0]));
        }
    }
    std::sort(caps.begin(), caps.end());
    caps.erase(std::unique(caps.begin(), caps.end()), caps.end());
    std::sort(heads.begin(), heads.end(), [](auto &a, auto &b) { return a.first < b.first; });
    // the replay's frames
    std::vector<Frame> frames;
    {
        std::ifstream in(argv[1]);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream s(line);
            Frame f;
            std::string side;
            int st, fi, pd, gd, bg;
            if (!(s >> f.t >> f.id >> side >> st >> fi >> pd >> gd >> bg)) continue;
            f.side = side == "R", f.stereo = st, f.fitted = fi, f.pinch = pd, f.grip = gd, f.began = bg;
            for (auto &p : f.pts) s >> p.x >> p.y >> p.z;
            s >> f.q[0] >> f.q[1] >> f.q[2] >> f.q[3] >> f.elbow.x >> f.elbow.y >> f.elbow.z;
            if (s) frames.push_back(f);
        }
    }
    if (heads.empty() || frames.empty() || caps.empty()) {
        std::fprintf(stderr, "nothing to evaluate: %zu head poses, %zu captures, %zu hand frames\n", heads.size(),
                     caps.size(), frames.size());
        return 1;
    }
    // raw -> mono: the offset most published captures sit at
    std::vector<uint64_t> setTimes;
    for (const Frame &f : frames) setTimes.push_back(f.t);
    std::sort(setTimes.begin(), setTimes.end());
    setTimes.erase(std::unique(setTimes.begin(), setTimes.end()), setTimes.end());
    std::map<int64_t, int> votes;
    for (size_t i = 0; i < std::min<size_t>(caps.size(), 300); ++i)
        for (uint64_t r : setTimes) ++votes[(int64_t(r) - int64_t(caps[i])) / 500'000];
    int64_t bin = 0;
    int best = 0;
    for (auto &[b, n] : votes)
        if (n > best) best = n, bin = b;
    // refine: the median exact difference within that bin
    std::vector<int64_t> diffs;
    for (uint64_t c : caps) {
        const auto it = std::lower_bound(setTimes.begin(), setTimes.end(), uint64_t(int64_t(c) + bin * 500'000 - 1'000'000));
        for (auto j = it; j != setTimes.end() && int64_t(*j) <= int64_t(c) + bin * 500'000 + 1'500'000; ++j)
            diffs.push_back(int64_t(*j) - int64_t(c));
    }
    std::sort(diffs.begin(), diffs.end());
    const int64_t off = diffs.empty() ? bin * 500'000 : diffs[diffs.size() / 2];
    std::printf("clock: recording raw = probe mono + %.3f s (%d of %zu published captures matched)\n", off / 1e9,
                int(diffs.size()), caps.size());

    FILE *csv = csvPath ? std::fopen(csvPath, "w") : nullptr;
    if (csv) std::fprintf(csv, "t,side,stereo,shown,below,ahead,palm,pitch,yawoff,raw_yaw,raw_pitch,yaw,pitch,near,pinch\n");
    Body body(params);
    HandRay rays[2] = {HandRay(0, params), HandRay(1, params)};
    struct SideStats {
        int frames = 0, shownFrames = 0, shows = 0, shortShows = 0, pinches = 0, rayPinches = 0, nearFrames = 0;
        double showStart = -1, lastT = 0;
        std::vector<double> showLens;
        std::map<std::string, int> hides, fails;
        std::vector<double> wobble, wobbleRaw, pinchShift;
        std::vector<std::pair<double, V3>> recent;  // filtered dir, last 0.5 s
        std::vector<std::pair<double, V3>> recentRaw;
        std::vector<std::pair<double, V3>> knuckles;
        // jitter: the second difference of the ray's yaw and pitch over three frames in a row,
        // over sqrt(6) (white noise of sd s gives second differences of sd s * sqrt(6))
        std::vector<double> jitter, jitterRaw;
        double py[2][2] = {}, pp[2][2] = {}, pt[2] = {-1, -1};  // [filtered, raw][frames back]
        double pinchAt = -1;
        V3 pinchDir;
        std::vector<double> below, ahead, palm;  // when stereo and raised at most 0.5 m below
    } st[2];
    const double t0 = frames.front().t / 1e9;
    size_t hi = 0;
    int skipped = 0;
    for (const Frame &f : frames) {
        if (only >= 0 && f.side != only) continue;
        const int64_t mono = int64_t(f.t) - off;
        while (hi + 1 < heads.size() && heads[hi + 1].first <= uint64_t(mono)) ++hi;
        const size_t pick = hi + 1 < heads.size() && heads[hi + 1].first - mono < mono - heads[hi].first ? hi + 1 : hi;
        if (std::llabs(int64_t(heads[pick].first) - mono) > 30'000'000) {
            ++skipped;
            continue;
        }
        const M34 &h = heads[pick].second;
        const double t = mono / 1e9;
        body.Update(t, Pos(h), Rot(h, V3{0, 0, -1}));
        auto room = [&](V3 p) { return Pos(h) + Rot(h, p); };
        HandFrame hf;
        hf.t = t, hf.stereo = f.stereo;
        hf.knuckle = room((f.pts[5] + f.pts[9]) * 0.5);
        hf.wrist = room(f.pts[0]);
        if (f.fitted) {
            hf.palmNormal = Norm(Rot(h, QRot(f.q, V3{0, -1, 0})));
            hf.elbow = room(f.elbow);
        } else {
            const V3 n = Cross(f.pts[5] - f.pts[0], f.pts[17] - f.pts[0]);
            hf.palmNormal = Norm(Rot(h, f.side ? n : n * -1.0));
        }
        hf.grip = f.grip;
        if (nearPanel && panelW > 0) {
            const V3 tip = room(f.pts[8]);
            const V3 q = RotInv(panel, tip - Pos(panel));
            hf.near = std::fabs(q.x) <= panelW / 2 + 0.02 && std::fabs(q.y) <= panelH / 2 + 0.02 && q.z > -0.03 && q.z <= 0.15;
        }
        HandRay &r = rays[f.side];
        SideStats &s = st[f.side];
        // a pinch begin is decided from the state before this frame (5.2: at begin_ns)
        if (f.began) {
            ++s.pinches;
            if (r.Shown()) ++s.rayPinches, s.pinchAt = t, s.pinchDir = r.Current().dir;
        }
        r.Hold(f.pinch && r.Shown(), t);
        r.Frame(body, hf, true);
        ++s.frames;
        s.nearFrames += hf.near;
        const Tests &ts = r.LastTests();
        if (f.stereo && ts.below < 0.5) s.below.push_back(ts.below), s.ahead.push_back(ts.ahead), s.palm.push_back(ts.palm);
        if (!r.event.empty()) {
            if (r.event == "show") ++s.shows, s.showStart = t;
            else {
                ++s.hides[r.event.substr(5)];
                if (s.showStart >= 0) {
                    s.showLens.push_back(t - s.showStart);
                    s.shortShows += t - s.showStart < 0.5;
                }
                s.showStart = -1;
            }
            r.event.clear();
        }
        if (r.Shown()) {
            ++s.shownFrames;
        } else {
            std::string fails = r.Why();
            ++s.fails[fails.empty() ? "(timing)" : fails];
        }
        // wobble: while shown and the knuckles hold still (under 2 cm/s over the last 0.3 s), the
        // spread of the ray's direction over the last 0.5 s
        s.knuckles.push_back({t, hf.knuckle});
        while (!s.knuckles.empty() && t - s.knuckles.front().first > 0.3) s.knuckles.erase(s.knuckles.begin());
        const bool still = s.knuckles.size() >= 4 &&
                           Len(s.knuckles.back().second - s.knuckles.front().second) / std::max(1e-3, t - s.knuckles.front().first) < 0.02;
        if (r.Shown()) {
            s.recent.push_back({t, r.Current().dir}), s.recentRaw.push_back({t, r.Raw().dir});
        } else {
            s.recent.clear(), s.recentRaw.clear();
        }
        while (!s.recent.empty() && t - s.recent.front().first > 0.5) s.recent.erase(s.recent.begin());
        while (!s.recentRaw.empty() && t - s.recentRaw.front().first > 0.5) s.recentRaw.erase(s.recentRaw.begin());
        auto spread = [](const std::vector<std::pair<double, V3>> &w) {
            V3 m;
            for (auto &e : w) m = m + e.second;
            m = Norm(m);
            double q = 0;
            for (auto &e : w) {
                const double a = std::acos(std::clamp(Dot(e.second, m), -1.0, 1.0)) / kDeg;
                q += a * a;
            }
            return std::sqrt(q / double(w.size()));
        };
        if (r.Shown() && still && s.recent.size() >= 6) s.wobble.push_back(spread(s.recent)), s.wobbleRaw.push_back(spread(s.recentRaw));
        if (r.Shown() && still) {
            const double y[2] = {YawOf(r.Current().dir) / kDeg, YawOf(r.Raw().dir) / kDeg};
            const double p[2] = {PitchOf(r.Current().dir) / kDeg, PitchOf(r.Raw().dir) / kDeg};
            if (s.pt[1] >= 0 && t - s.pt[1] < 0.2)
                for (int k = 0; k < 2; ++k) {
                    const double dy = std::remainder(y[k] - 2 * s.py[k][0] + s.py[k][1], 360.0);
                    const double dp = p[k] - 2 * s.pp[k][0] + s.pp[k][1];
                    (k ? s.jitterRaw : s.jitter).push_back(std::hypot(dy, dp) / std::sqrt(6.0));
                }
            for (int k = 0; k < 2; ++k) s.py[k][1] = s.py[k][0], s.py[k][0] = y[k], s.pp[k][1] = s.pp[k][0], s.pp[k][0] = p[k];
            s.pt[1] = s.pt[0], s.pt[0] = t;
        } else {
            s.pt[0] = s.pt[1] = -1;
        }
        if (s.pinchAt >= 0 && t - s.pinchAt >= 0.15) {
            if (r.Shown()) s.pinchShift.push_back(std::acos(std::clamp(Dot(r.Current().dir, s.pinchDir), -1.0, 1.0)) / kDeg);
            s.pinchAt = -1;
        }
        s.lastT = t;
        if (csv)
            std::fprintf(csv, "%.4f,%d,%d,%d,%.3f,%.3f,%.3f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%d,%d\n", t - t0, f.side, f.stereo,
                         r.Shown(), ts.below, ts.ahead, ts.palm, ts.pitch, ts.yawOff, YawOf(r.Raw().dir) / kDeg,
                         PitchOf(r.Raw().dir) / kDeg, YawOf(r.Current().dir) / kDeg, PitchOf(r.Current().dir) / kDeg,
                         hf.near, f.pinch);
    }
    if (csv) std::fclose(csv);
    std::printf("hand frames: %zu (%d without a head pose within 30 ms)\n", frames.size(), skipped);
    for (int k = 0; k < 2; ++k) {
        SideStats &s = st[k];
        if (!s.frames) continue;
        if (s.showStart >= 0) s.showLens.push_back(s.lastT - s.showStart);
        std::printf("\n%s hand: %d frames, ray shown in %.1f%%; %d shows (%d under 0.5 s), median %.2f s%s\n",
                    k ? "right" : "left", s.frames, 100.0 * s.shownFrames / s.frames, s.shows, s.shortShows,
                    Pct(s.showLens, 0.5), nearPanel ? "" : " (no near claims: --near-panel)");
        if (nearPanel) std::printf("  near the probe's panel in %.1f%% of frames\n", 100.0 * s.nearFrames / s.frames);
        std::printf("  pinches: %d, %d of them with the ray up (ray pinches), %d gaze pinches\n", s.pinches, s.rayPinches,
                    s.pinches - s.rayPinches);
        std::printf("  hides:");
        for (auto &[w, n] : s.hides) std::printf(" %s %d,", w.c_str(), n);
        std::printf("\n  not shown because (frames):");
        std::vector<std::pair<int, std::string>> f;
        for (auto &[w, n] : s.fails) f.push_back({n, w});
        std::sort(f.rbegin(), f.rend());
        for (size_t i = 0; i < std::min<size_t>(f.size(), 8); ++i) std::printf(" %s %d,", f[i].second.c_str(), f[i].first);
        std::printf("\n  hands up (stereo, under 0.5 m below the eyes): below p10/50/90 %.2f/%.2f/%.2f m, ahead "
                    "%.2f/%.2f/%.2f m, palm.D %.2f/%.2f/%.2f\n",
                    Pct(s.below, 0.1), Pct(s.below, 0.5), Pct(s.below, 0.9), Pct(s.ahead, 0.1), Pct(s.ahead, 0.5),
                    Pct(s.ahead, 0.9), Pct(s.palm, 0.1), Pct(s.palm, 0.5), Pct(s.palm, 0.9));
        std::printf("  wobble, shown and the hand still (spread over 0.5 s): filtered p50 %.3f p90 %.3f deg, raw p50 %.3f "
                    "p90 %.3f deg (%zu windows)\n",
                    Pct(s.wobble, 0.5), Pct(s.wobble, 0.9), Pct(s.wobbleRaw, 0.5), Pct(s.wobbleRaw, 0.9), s.wobble.size());
        std::printf("  jitter, shown and the hand still (second differences): filtered p50 %.3f p90 %.3f deg, raw p50 "
                    "%.3f p90 %.3f deg (%zu)\n",
                    Pct(s.jitter, 0.5), Pct(s.jitter, 0.9), Pct(s.jitterRaw, 0.5), Pct(s.jitterRaw, 0.9), s.jitter.size());
        std::printf("  ray shift 150 ms into a ray pinch: p50 %.2f p90 %.2f deg (%zu)\n", Pct(s.pinchShift, 0.5),
                    Pct(s.pinchShift, 0.9), s.pinchShift.size());
    }
    return 0;
}
