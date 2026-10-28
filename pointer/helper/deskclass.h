// Desk hands: a small trained model that says, per tracked hand, whether it's on the keyboard, on the
// mouse or free (raised, pointing, in use), from its skeleton and the head. The pointer helper's typing
// guard (typing.h) parks a hand the model puts at the desk, keys or no keys (user, 2026-10-28: resting
// desk hands kept taking the pointer from the mouse; "Let's create a classifier and an accompanying
// bounty for training it"). The hand recorder's desk-work round (hands/rec/bounties/desk-work.json)
// records the training data; pointer/tools/deskfeat.cpp turns a recording into these features and
// hands/tools/desk_train.py trains the model. Both sides use Features below, so the model sees the
// same numbers offline and live.
//
// Features (Features::kCount), per hand and frame, in the head's level frame (origin at the eyes, +x
// right, +y up, +z back, turned with the head's heading only), x mirrored for a left hand so one model
// serves both: the palm's place, its normal, the hand's direction (wrist to the middle knuckle), each
// fingertip's distance from the palm and thumb to index (over the hand's size), the spread, the head's
// pitch, how far off the line of sight the palm is, the palm's speed and climb, how much the palm wandered
// over the last second (the mouse slides, typing stays), how busy the fingertips were over the last half
// second (typing), the other hand (seen, how far, how level), and whether the hand was triangulated and
// fitted. A hand unseen for more than kGap starts its history over.
//
// The model file (text, hands/tools/desk_train.py writes it; "#" starts a comment): "deskclass 1",
// "features N", "classes free keys mouse", "mean" and "std" lines of N numbers, then layers:
// "layer IN OUT relu|none", OUT rows of IN weights and a row of OUT biases. Model::Desk gives
// P(keys) + P(mouse).
//
// Header-only and free of OpenVR, so pointer/tests/deskclass-test.cpp and the offline tools can use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace desk {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Unit(V3 a) {
    const double l = Len(a);
    return l > 1e-9 ? a * (1 / l) : V3{};
}

// One tracked hand in the room (the standing universe, metres): its side (0 left, 1 right), the 21
// landmarks, the palm's normal (unit, out of the palm) and how it was tracked.
struct Hand {
    bool seen = false;
    int side = 0;
    bool stereo = false, fitted = false;
    V3 pts[21];
    V3 normal;
};

// The head: where the eyes are and where it faces (its -z, unit).
struct Head {
    V3 pos, fwd{0, 0, -1};
};

class Features {
public:
    static constexpr int kCount = 30;
    static constexpr double kGap = 0.3;      // s unseen: the hand's history starts over
    static constexpr double kWander = 1.0;   // s: the palm's spread over this long
    static constexpr double kFingers = 0.5;  // s: the fingertips' activity over this long

    static const char *Name(int i) {
        static const char *names[kCount] = {
            "palm_x", "palm_y", "palm_z", "normal_x", "normal_y", "normal_z", "dir_x", "dir_y", "dir_z",
            "tip_thumb", "tip_index", "tip_middle", "tip_ring", "tip_pinky", "pinch", "spread",
            "head_pitch", "off_sight", "speed", "climb", "wander_x", "wander_y", "wander_z", "fingers",
            "other_seen", "other_dist", "other_dy", "stereo", "fitted", "hand_size"};
        return i >= 0 && i < kCount ? names[i] : "";
    }

    // The frame at t (seconds, one steady clock) for both sides; out[s] gets side s's features when
    // its hand is seen (returns which were).
    void Frame(double t, const Head &head, const Hand hands[2], double out[2][kCount], bool ok[2]) {
        V3 right{-head.fwd.z, 0, head.fwd.x};
        if (Len(right) < 1e-6) right = {1, 0, 0};
        right = Unit(right);
        const V3 back{-right.z, 0, right.x};  // level, behind the head
        for (int s = 0; s < 2; ++s) {
            ok[s] = hands[s].seen;
            if (!ok[s]) continue;
            const Hand &h = hands[s];
            History &hist = hist_[s];
            if (!hist.at.empty() && (t - hist.at.back() > kGap || t < hist.at.back())) hist = History{};
            const double m = s ? 1 : -1;  // mirror a left hand
            auto level = [&](V3 v, bool mirror) { return V3{Dot(v, right) * (mirror ? m : 1), v.y, Dot(v, back)}; };
            const V3 palm = (h.pts[0] + h.pts[5] + h.pts[9] + h.pts[13] + h.pts[17]) * 0.2;
            const double size = std::clamp(Len(h.pts[9] - h.pts[0]), 0.06, 0.13);
            const V3 lp = level(palm - head.pos, true);
            const V3 ln = level(Unit(h.normal), true);
            const V3 ld = level(Unit(h.pts[9] - h.pts[0]), true);
            double *f = out[s];
            int k = 0;
            f[k++] = lp.x, f[k++] = lp.y, f[k++] = lp.z;
            f[k++] = ln.x, f[k++] = ln.y, f[k++] = ln.z;
            f[k++] = ld.x, f[k++] = ld.y, f[k++] = ld.z;
            static const int tips[5] = {4, 8, 12, 16, 20};
            V3 rel[5];
            for (int i = 0; i < 5; ++i) {
                rel[i] = h.pts[tips[i]] - palm;
                f[k++] = Len(rel[i]) / size;
            }
            f[k++] = Len(h.pts[4] - h.pts[8]) / size;
            f[k++] = Len(h.pts[8] - h.pts[20]) / size;
            const V3 fwd = Unit(head.fwd);
            f[k++] = std::asin(std::clamp(fwd.y, -1.0, 1.0));
            f[k++] = 1 - Dot(fwd, Unit(palm - head.pos));
            // the palm's speed and climb, from the last frame
            double speed = 0, climb = 0;
            if (!hist.at.empty() && t > hist.at.back() + 1e-4) {
                const double dt = t - hist.at.back();
                const V3 d = palm - hist.palm.back();
                speed = std::min(Len(d) / dt, 3.0), climb = std::clamp(d.y / dt, -3.0, 3.0);
            }
            f[k++] = speed, f[k++] = climb;
            // the history: the palm (level frame) and the fingertips (the palm's own, mirrored)
            hist.at.push_back(t), hist.palm.push_back(palm), hist.level.push_back(lp);
            Tips tp;
            for (int i = 0; i < 5; ++i) tp.v[i] = level(rel[i], true);
            hist.tips.push_back(tp);
            while (!hist.at.empty() && t - hist.at.front() > kWander + 1e-9)
                hist.at.pop_front(), hist.palm.pop_front(), hist.level.pop_front(), hist.tips.pop_front();
            V3 mean, sq;
            for (const V3 &p : hist.level) mean = mean + p, sq = sq + V3{p.x * p.x, p.y * p.y, p.z * p.z};
            const double n = double(hist.level.size());
            mean = mean * (1 / n), sq = sq * (1 / n);
            f[k++] = std::sqrt(std::max(0.0, sq.x - mean.x * mean.x));
            f[k++] = std::sqrt(std::max(0.0, sq.y - mean.y * mean.y));
            f[k++] = std::sqrt(std::max(0.0, sq.z - mean.z * mean.z));
            // fingertip travel relative to the palm per second, over the last kFingers
            double travel = 0, span = 0;
            for (size_t i = 1; i < hist.at.size(); ++i) {
                if (t - hist.at[i - 1] > kFingers + 1e-9) continue;
                for (int j = 0; j < 5; ++j) travel += Len(hist.tips[i].v[j] - hist.tips[i - 1].v[j]);
                span += hist.at[i] - hist.at[i - 1];
            }
            f[k++] = span > 1e-3 ? std::min(travel / span / 5, 2.0) : 0;
            // the other hand
            const Hand &o = hands[1 - s];
            if (o.seen) {
                const V3 op = (o.pts[0] + o.pts[5] + o.pts[9] + o.pts[13] + o.pts[17]) * 0.2;
                f[k++] = 1, f[k++] = Len(op - palm), f[k++] = op.y - palm.y;
            } else {
                f[k++] = 0, f[k++] = 0, f[k++] = 0;
            }
            f[k++] = h.stereo ? 1 : 0, f[k++] = h.fitted ? 1 : 0;
            f[k++] = size;
        }
    }
    void Reset() { hist_[0] = hist_[1] = History{}; }

private:
    struct Tips {
        V3 v[5];
    };
    struct History {
        std::deque<double> at;
        std::deque<V3> palm, level;
        std::deque<Tips> tips;
    };
    History hist_[2];
};

// The trained model: a small multilayer perceptron over standardised features.
class Model {
public:
    enum { kFree, kKeys, kMouse, kClasses };

    // Loads path; false (and error says why) if it isn't a model for these features.
    bool Load(const std::string &path) {
        std::ifstream in(path);
        if (!in) return Fail("can't open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        return Parse(ss.str());
    }
    bool Parse(const std::string &text) {
        layers_.clear(), mean_.clear(), std_.clear();
        std::string body;  // without comments ("#" to the line's end)
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '#') i = std::min(text.find('\n', i), text.size()) - 1;
            else body += text[i];
        }
        std::istringstream in(body);
        std::string word;
        int version = 0, n = 0;
        if (!(in >> word >> version) || word != "deskclass" || version != 1) return Fail("not a deskclass 1 model");
        if (!(in >> word >> n) || word != "features" || n != Features::kCount)
            return Fail("made for other features (" + std::to_string(n) + ", these have " + std::to_string(Features::kCount) + ")");
        std::string c1, c2, c3;
        if (!(in >> word >> c1 >> c2 >> c3) || word != "classes" || c1 != "free" || c2 != "keys" || c3 != "mouse")
            return Fail("not the classes free keys mouse");
        if (!Row(in, "mean", n, mean_) || !Row(in, "std", n, std_)) return Fail("no mean or std");
        int width = n;
        while (in >> word) {
            Layer l;
            std::string act;
            if (word != "layer" || !(in >> l.in >> l.out >> act) || l.in != width || l.out < 1 || l.out > 4096 ||
                (act != "relu" && act != "none"))
                return Fail("a bad layer");
            l.relu = act == "relu";
            l.w.resize(size_t(l.in) * size_t(l.out)), l.b.resize(size_t(l.out));
            for (double &v : l.w)
                if (!(in >> v) || !std::isfinite(v)) return Fail("a layer's weights end early");
            for (double &v : l.b)
                if (!(in >> v) || !std::isfinite(v)) return Fail("a layer's biases end early");
            width = l.out;
            layers_.push_back(std::move(l));
        }
        if (layers_.empty() || width != kClasses) return Fail("no layers, or not 3 outputs");
        for (double &s : std_) s = s > 1e-9 ? s : 1;
        error.clear();
        return true;
    }
    bool Loaded() const { return !layers_.empty(); }

    // Each class's probability for one hand's features.
    void Probs(const double f[Features::kCount], double p[kClasses]) const {
        std::vector<double> x(f, f + Features::kCount), y;
        for (int i = 0; i < Features::kCount; ++i) x[size_t(i)] = (x[size_t(i)] - mean_[size_t(i)]) / std_[size_t(i)];
        for (const Layer &l : layers_) {
            y.assign(size_t(l.out), 0);
            for (int o = 0; o < l.out; ++o) {
                double v = l.b[size_t(o)];
                const double *w = &l.w[size_t(o) * size_t(l.in)];
                for (int i = 0; i < l.in; ++i) v += w[i] * x[size_t(i)];
                y[size_t(o)] = l.relu ? std::max(0.0, v) : v;
            }
            x.swap(y);
        }
        const double top = *std::max_element(x.begin(), x.end());
        double sum = 0;
        for (int c = 0; c < kClasses; ++c) sum += p[c] = std::exp(x[size_t(c)] - top);
        for (int c = 0; c < kClasses; ++c) p[c] /= sum;
    }
    // P(on the keyboard or the mouse).
    double Desk(const double f[Features::kCount]) const {
        double p[kClasses];
        Probs(f, p);
        return p[kKeys] + p[kMouse];
    }

    std::string error;

private:
    struct Layer {
        int in = 0, out = 0;
        bool relu = false;
        std::vector<double> w, b;
    };
    bool Fail(const std::string &why) {
        error = why, layers_.clear();
        return false;
    }
    static bool Row(std::istream &in, const char *name, int n, std::vector<double> &out) {
        std::string word;
        if (!(in >> word) || word != name) return false;
        out.resize(size_t(n));
        for (double &v : out)
            if (!(in >> v) || !std::isfinite(v)) return false;
        return true;
    }
    std::vector<double> mean_, std_;
    std::vector<Layer> layers_;
};

}  // namespace desk
