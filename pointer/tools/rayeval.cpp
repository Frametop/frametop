// rayeval: the hand ray (hands/include/fh_ray.h) run offline over a recording, to see how
// often it shows, why it doesn't, how much it wobbles, and whether pinches would be ray pinches
// or gaze pinches. Probe P9 of frame-hands/notes/hand-interaction.md, without the headset.
//
//   rayeval RAY.txt|DUMP PROBE.jsonl [--side L|R|both] [--near-panel] [--csv FILE] [--set NAME=VALUE...]
//
// RAY.txt: ft-handreplay DIR --ray RAY.txt (per set and hand: the points the helper would read).
// Or DUMP: ft-handreplay DIR --publish-dump DUMP (the hands and gestures files as ft-hands would have
// published them; frame-hands/dumps), which needs no replay of its own.
// PROBE.jsonl: the ft-handtest probe's log from the same session (frame-hands/captures/probe-*),
// which has the head's pose at about 90 Hz on CLOCK_MONOTONIC and the panel it showed. The
// recording's capture times are CLOCK_MONOTONIC_RAW: the offset is found by matching the sets
// the live tracker published (the probe's "cap") with the recorded ones.
// --near-panel: count a hand as near (ft-screens' near claim: the index tip over the probe's
// panel, within 15 cm in front of it) as the helper would hear from ft-screens.
// --csv: per hand frame: t, side, shown, the tests' values, ray yaw and pitch (raw, filtered).
// --set: an fh_ray Params value (predict, minCutoff, beta, dCutoff, holdMinCutoff, reachMinCutoff,
// reachBeta, palmShow, palmHide, raisedShow, raisedHide, forwardShow, forwardHide, showFor, hideFor),
// to try settings.
//
// Smoothness at the display's rate (fh_smooth.h's point): the run also reads each shown ray at
// 90 Hz ticks from when its frame would have arrived (capture time plus the published latency) to the
// next, through fh_ray's filter (At) and through the filter before it (One Euro per yaw and pitch,
// updated at frames only: what the pointer held between frames), and reports for each: the judder
// (the direction's second difference per tick, deg: steps show there), the wobble of a still hand,
// the lag behind the raw ray while it turns faster than 10 deg/s (the delay that fits best, ms,
// tracking latency included), the reach's wobble (push and pull) and how much of the ray's motion
// with a still hand came from the body model (the head moving the shoulder). --held: the filters as
// during a ray pinch (holdMinCutoff) throughout, for a drag's precision.
//
// Build: g++ -std=c++17 -O2 -I. pointer/tools/rayeval.cpp -o build/rayeval (no dependencies).
#include "hands/include/fh_ray.h"
extern "C" {
#include "hands/include/fh_gestures.h"
#include "hands/include/fh_hands.h"
}

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
    bool haveKnuckle = false;  // the fit's pinch point (a dump); else the knuckles' midpoint
    V3 knuckle;
    uint64_t latency = 0;      // capture to publish, ns (a dump; else 35 ms)
};

// A publish dump (ft-handreplay --publish-dump: FHDUMP01, or FHDUMP02 with each set's dequeue time)
// as the --ray file's frames.
bool LoadDump(const char *path, std::vector<Frame> &out) {
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    uint32_t sizes[2];
    if (!f.read(magic, 8) || (std::memcmp(magic, "FHDUMP01", 8) && std::memcmp(magic, "FHDUMP02", 8)) ||
        !f.read(reinterpret_cast<char *>(sizes), 8) || sizes[0] < sizeof(fh_hands_t) || sizes[1] > 4096)
        return false;
    const bool v2dump = !std::memcmp(magic, "FHDUMP02", 8);
    std::vector<char> hb(sizes[0]), gb(sizes[1]);
    static fh_hands_v2_t h;
    fh_gestures_t g;
    uint32_t begins[2] = {0, 0};
    bool first = true;
    while (f.read(hb.data(), sizes[0]) && f.read(gb.data(), sizes[1])) {
        int64_t dq;
        if (v2dump && !f.read(reinterpret_cast<char *>(&dq), sizeof dq)) break;
        std::memset(&h, 0, sizeof h), std::memset(&g, 0, sizeof g);
        std::memcpy(&h, hb.data(), std::min(sizeof h, hb.size()));
        std::memcpy(&g, gb.data(), std::min(sizeof g, gb.size()));
        const bool v2 = h.v1.version >= 2 && sizes[0] >= sizeof h;
        for (uint32_t k = 0; k < std::min<uint32_t>(h.v1.nhands, FH_HANDS_MAX_HANDS); ++k) {
            const fh_hand_t &hh = h.v1.hands[k];
            const fh_hand_fit_t *fit = v2 && (h.fits[k].flags & FH_FIT_VALID) && h.fits[k].id == hh.id ? &h.fits[k] : nullptr;
            Frame fr;
            fr.t = h.v1.capture_ns, fr.id = int(hh.id), fr.side = (hh.flags & FH_HAND_RIGHT) ? 1 : 0;
            fr.stereo = hh.flags & FH_HAND_STEREO, fr.fitted = fit != nullptr;
            auto v = [](const float p[3]) { return V3{p[0], p[1], p[2]}; };
            for (int i = 0; i < 21; ++i) fr.pts[i] = v(fit ? fit->joints[i].p : hh.pts[i]);
            if (fit) {
                for (int i = 0; i < 4; ++i) fr.q[i] = fit->joints[0].q[i];
                fr.elbow = v(fit->elbow), fr.knuckle = v(fit->pinch_point), fr.haveKnuckle = true;
            } else {
                fr.q[0] = fr.q[1] = fr.q[2] = 0, fr.q[3] = 1;
                fr.elbow = fr.pts[0];
            }
            const fh_pinch_t &pp = g.pinch[fr.side];
            fr.pinch = (pp.flags & FH_PINCH_DOWN) && pp.hand_id == hh.id;
            fr.grip = (g.grip[fr.side].flags & FH_PINCH_DOWN) && g.grip[fr.side].hand_id == hh.id;
            fr.began = !first && pp.begins != begins[fr.side] && pp.hand_id == hh.id;
            fr.latency = h.v1.publish_ns > h.v1.capture_ns ? h.v1.publish_ns - h.v1.capture_ns : 0;
            out.push_back(fr);
        }
        for (int s = 0; s < 2; ++s) begins[s] = g.pinch[s].begins;
        first = false;
    }
    return !out.empty();
}

// The ray's filter before fh_smooth.h (2026-10-09 hands-full, its settings: min cutoff 0.8 Hz, beta
// 0.02, d cutoff 1 Hz): One Euro per yaw and pitch (degrees, the speed from the filtered value),
// updated at tracker frames only.
class LegacyEuro {
public:
    void Set(double mc, double b, double dc) { min_ = mc, beta_ = b, dc_ = dc; }
    void Reset() { init_ = false; }
    double operator()(double v, double t) {
        if (!init_ || t <= t_) {
            if (!init_) x_ = v, dx_ = 0, t_ = t, init_ = true;
            return x_;
        }
        const double dt = t - t_;
        t_ = t;
        dx_ += Alpha(dc_, dt) * ((v - x_) / dt - dx_);
        x_ += Alpha(min_ + beta_ * std::fabs(dx_), dt) * (v - x_);
        return x_;
    }

private:
    static double Alpha(double c, double dt) { return 1 / (1 + 1 / (2 * M_PI * c * dt)); }
    double min_ = 1, beta_ = 0, dc_ = 1, x_ = 0, dx_ = 0, t_ = 0;
    bool init_ = false;
};
class LegacyRayFilter {
public:
    LegacyRayFilter() { yaw_.Set(0.8, 0.02, 1.0), pitch_.Set(0.8, 0.02, 1.0); }
    void Reset() { yaw_.Reset(), pitch_.Reset(), have_ = false; }
    V3 operator()(V3 dir, double t) {
        double y = YawOf(dir) / kDeg;
        if (have_) y = last_ + std::remainder(y - last_, 360.0);
        last_ = y, have_ = true;
        return Dir(yaw_(y, t) * kDeg, pitch_(PitchOf(dir) / kDeg, t) * kDeg);
    }

private:
    LegacyEuro yaw_, pitch_;
    double last_ = 0;
    bool have_ = false;
};

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s RAY.txt|DUMP PROBE.jsonl [--side L|R|both] [--near-panel] [--held] [--csv FILE] [--set NAME=VALUE]\n", argv[0]);
        return 2;
    }
    int only = -1;
    bool nearPanel = false, held = false;
    const char *csvPath = nullptr;
    std::vector<std::string> sets;  // --set name=value: fh_ray Params
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--side") && i + 1 < argc) {
            const char *s = argv[++i];
            only = !std::strcmp(s, "L") ? 0 : !std::strcmp(s, "R") ? 1 : -1;
        } else if (!std::strcmp(argv[i], "--near-panel")) nearPanel = true;
        else if (!std::strcmp(argv[i], "--held")) held = true;
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
                  : k == "showFor" ? &params.showFor : k == "hideFor" ? &params.hideFor
                  : k == "holdMinCutoff" ? &params.holdMinCutoff : k == "reachMinCutoff" ? &params.reachMinCutoff
                  : k == "reachBeta" ? &params.reachBeta : nullptr;
        if (!f) {
            std::fprintf(stderr, "unknown --set %s\n", k.c_str());
            return 2;
        }
        *f = v;
    }
    if (held) params.minCutoff = params.holdMinCutoff;
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
    if (!LoadDump(argv[1], frames)) {
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
    // Smoothness at the display's rate (see the top): per side, the shown ray read at 90 Hz ticks
    // through the filter (At) and the legacy one (held between frames), and the raw rays.
    struct Tick {
        double t;
        int run;
        V3 now, old;
        double reach;
        bool still;
    };
    struct RawRay {
        double t;
        int run;
        V3 dir;
        double reach;
    };
    struct Smooth {
        std::vector<Tick> ticks;
        std::vector<RawRay> raw;
        int run = 0;
        bool still = false, haveS = false;
        V3 legacy, lastS, lastRaw;
        std::vector<double> bodyDegS;   // with the hand still: the ray's turn from the shoulder moving, deg/s
        double bodyAngle = 0, rawAngle = 0;
    } sm[2];
    LegacyRayFilter legacy[2];
    auto runTicks = [&](double from, double to) {
        for (int k = 0; k < 2; ++k) {
            if (!rays[k].Shown() || (only >= 0 && k != only)) continue;
            for (double tk = from; tk < to; tk += 1 / 90.0)
                sm[k].ticks.push_back({tk, sm[k].run, rays[k].At(tk).dir, sm[k].legacy, rays[k].Reach(), sm[k].still});
        }
    };
    double setArrive = -1;
    uint64_t setT = 0;
    const double t0 = frames.front().t / 1e9;
    size_t hi = 0;
    int skipped = 0;
    for (const Frame &f : frames) {
        const int64_t mono = int64_t(f.t) - off;
        if (f.t != setT) {  // a new set: the last one's ticks, up to when this one arrives
            const double arrive = (mono + int64_t(f.latency ? f.latency : 35'000'000)) / 1e9;
            if (setArrive >= 0) runTicks(setArrive, std::min(arrive, setArrive + 0.2));
            setT = f.t, setArrive = arrive;
        }
        if (only >= 0 && f.side != only) continue;
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
        hf.knuckle = room(f.haveKnuckle ? f.knuckle : (f.pts[5] + f.pts[9]) * 0.5);
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
        r.Frame(body, hf, true, setArrive);
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
        // the smoothness series (see the top)
        Smooth &q = sm[f.side];
        if (r.Shown()) {
            if (q.raw.empty() || q.raw.back().run != q.run || t - q.raw.back().t > 0.2) ++q.run, legacy[f.side].Reset();
            q.legacy = legacy[f.side](r.Raw().dir, t);
            q.raw.push_back({t, q.run, r.Raw().dir, r.RawReach()});
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
        q.still = still;
        {   // the body model's share: the ray's turn from the shoulder moving, the knuckles where they are
            const V3 sh = body.Shoulder(f.side), d = Norm(hf.knuckle - sh);
            if (q.haveS && still && r.Shown() && t - s.lastT > 0 && t - s.lastT < 0.1) {
                const double a = std::atan2(Len(fhsmooth::Cross(Norm(hf.knuckle - q.lastS), d)), Dot(Norm(hf.knuckle - q.lastS), d)) / kDeg;
                q.bodyDegS.push_back(a / (t - s.lastT));
                q.bodyAngle += a, q.rawAngle += fhsmooth::Angle(q.lastRaw, r.Raw().dir) / kDeg;
            }
            q.lastS = sh, q.lastRaw = r.Raw().dir, q.haveS = true;
        }
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
    runTicks(setArrive, setArrive + 1 / 30.0);
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
        Smooth &q = sm[k];
        // the raw ray between frames (linear, normalised), within one run
        auto rawAt = [&](double t, int run, V3 &out) {
            auto it = std::lower_bound(q.raw.begin(), q.raw.end(), t, [](const RawRay &r, double v) { return r.t < v; });
            if (it == q.raw.begin() || it == q.raw.end()) return false;
            const RawRay &b = *it, &a = *(it - 1);
            if (a.run != run || b.run != run || b.t - a.t > 0.2) return false;
            const double u = (t - a.t) / (b.t - a.t);
            out = Norm(a.dir * (1 - u) + b.dir * u);
            return true;
        };
        std::vector<double> judNew, judOld, judNewMov, judOldMov;
        std::vector<char> moving(q.ticks.size(), 0);
        for (size_t i = 0; i < q.ticks.size(); ++i) {
            V3 a, b;
            const Tick &x = q.ticks[i];
            moving[i] = rawAt(x.t - 0.05, x.run, a) && rawAt(x.t + 0.05, x.run, b) && fhsmooth::Angle(a, b) / kDeg / 0.1 > 10;
        }
        for (size_t i = 1; i + 1 < q.ticks.size(); ++i) {
            const Tick &a = q.ticks[i - 1], &b = q.ticks[i], &c = q.ticks[i + 1];
            if (a.run != b.run || b.run != c.run || c.t - a.t > 0.03) continue;
            const double jn = Len(a.now - b.now * 2 + c.now) / kDeg, jo = Len(a.old - b.old * 2 + c.old) / kDeg;
            judNew.push_back(jn), judOld.push_back(jo);
            if (moving[i]) judNewMov.push_back(jn), judOldMov.push_back(jo);
        }
        auto rms = [](const std::vector<double> &v) {
            double q2 = 0;
            for (double x : v) q2 += x * x;
            return v.empty() ? NAN : std::sqrt(q2 / double(v.size()));
        };
        std::printf("  judder p50 %.4f p90 %.4f, before p50 %.4f p90 %.4f\n", Pct(judNew, 0.5), Pct(judNew, 0.9), Pct(judOld, 0.5), Pct(judOld, 0.9));
        std::printf("  display rate (90 Hz ticks, %zu): judder (second difference per tick) rms %.3f p99 %.3f deg, "
                    "turning > 10 deg/s rms %.3f p99 %.3f deg; before (filtered at frames, held between) rms %.3f p99 %.3f, "
                    "turning rms %.3f p99 %.3f\n",
                    judNew.size(), rms(judNew), Pct(judNew, 0.99), rms(judNewMov), Pct(judNewMov, 0.99), rms(judOld),
                    Pct(judOld, 0.99), rms(judOldMov), Pct(judOldMov, 0.99));
        // wobble at ticks: 0.5 s windows (45 ticks) of one run, the hand still throughout
        std::vector<double> wobNew, wobOld, reachRaw, reachNew;
        for (size_t i = 0; i + 45 <= q.ticks.size(); i += 45) {
            bool ok = true;
            for (size_t j = i; j < i + 45 && ok; ++j) ok = q.ticks[j].run == q.ticks[i].run && q.ticks[j].still;
            if (!ok || q.ticks[i + 44].t - q.ticks[i].t > 0.6) continue;
            auto spread = [&](bool now) {
                V3 m;
                for (size_t j = i; j < i + 45; ++j) m = m + (now ? q.ticks[j].now : q.ticks[j].old);
                m = Norm(m);
                double s2 = 0;
                for (size_t j = i; j < i + 45; ++j) {
                    const double a = fhsmooth::Angle(now ? q.ticks[j].now : q.ticks[j].old, m) / kDeg;
                    s2 += a * a;
                }
                return std::sqrt(s2 / 45);
            };
            wobNew.push_back(spread(true)), wobOld.push_back(spread(false));
            double m = 0, s2 = 0;
            for (size_t j = i; j < i + 45; ++j) m += q.ticks[j].reach / 45;
            for (size_t j = i; j < i + 45; ++j) s2 += (q.ticks[j].reach - m) * (q.ticks[j].reach - m) / 45;
            reachNew.push_back(std::sqrt(s2));
            // the raw reach over the same 0.5 s
            std::vector<double> rr;
            for (const RawRay &r : q.raw)
                if (r.run == q.ticks[i].run && r.t >= q.ticks[i].t - 0.05 && r.t <= q.ticks[i + 44].t - 0.05) rr.push_back(r.reach);
            if (rr.size() >= 4) {
                double mm = 0, ss = 0;
                for (double x : rr) mm += x / double(rr.size());
                for (double x : rr) ss += (x - mm) * (x - mm) / double(rr.size());
                reachRaw.push_back(std::sqrt(ss));
            }
        }
        std::printf("  display rate, hand still (0.5 s windows, %zu): wobble p50 %.3f p90 %.3f deg, before p50 %.3f p90 %.3f\n",
                    wobNew.size(), Pct(wobNew, 0.5), Pct(wobNew, 0.9), Pct(wobOld, 0.5), Pct(wobOld, 0.9));
        std::printf("  reach (push and pull), hand still: sd p50 %.1f p90 %.1f mm, raw p50 %.1f p90 %.1f mm "
                    "(a panel at 2 m moves 3 x 2 m / 0.45 m = 13x that past the 1.5 cm dead zone)\n",
                    Pct(reachNew, 0.5) * 1000, Pct(reachNew, 0.9) * 1000, Pct(reachRaw, 0.5) * 1000, Pct(reachRaw, 0.9) * 1000);
        // lag: the delay d that best fits each output to the raw ray at t - d, while it turns
        struct Fit { double ms, deg; };
        auto lag = [&](bool now) {
            double best = -1, bestErr = 1e9;
            for (double d = 0; d <= 0.3001; d += 0.005) {
                double e = 0;
                int n = 0;
                for (size_t i = 0; i < q.ticks.size(); ++i) {
                    V3 r;
                    if (!moving[i] || !rawAt(q.ticks[i].t - d, q.ticks[i].run, r)) continue;
                    e += fhsmooth::Angle(now ? q.ticks[i].now : q.ticks[i].old, r), ++n;
                }
                if (n > 50 && e / n < bestErr) bestErr = e / n, best = d;
            }
            return Fit{best * 1000, bestErr / kDeg};
        };
        const auto ln = lag(true), lo = lag(false);
        std::printf("  lag turning > 10 deg/s (%zu ticks): %.0f ms (left %.2f deg off), before %.0f ms (%.2f deg off); "
                    "latency to the helper included\n",
                    size_t(std::count(moving.begin(), moving.end(), 1)), ln.ms, ln.deg, lo.ms, lo.deg);
        std::printf("  body model, hand still: the shoulder turned the ray %.0f%% of its raw motion; p50 %.2f p90 %.2f deg/s\n",
                    q.rawAngle > 0 ? 100 * q.bodyAngle / q.rawAngle : NAN, Pct(q.bodyDegS, 0.5), Pct(q.bodyDegS, 0.9));
    }
    return 0;
}
