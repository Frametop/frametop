// An avatar's face, the sums (avatar/face/face.h): when the eyes blink, and how the mouth moves
// with a voice. The voices are made here: a pulse train at a pitch, tilted as a voice is, through
// a vowel's three formants. No microphone.
#include "../face/face.h"

#include <cstdio>
#include <limits>
#include <vector>

using bodyfile::Shape;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

constexpr double kPi = 3.14159265358979323846, kRate = 16000, kTick = 1.0 / 45;   // the rate micear hears at; a mirror's draw

// A vowel with formants `f` (Hz), `db` of full scale loud.
static std::vector<float> vowel(const double f[3], double seconds, double db, double pitch, double rate = kRate) {
    const int n = int(seconds * rate);
    std::vector<double> x(size_t(n), 0.0);
    for (double t = 0; t < n; t += rate / pitch) x[size_t(t)] = 1;
    double low = 0;
    const double k = 1 - std::exp(-2 * kPi * 150 / rate);
    for (double &v : x) low += (v - low) * k, v = low;
    const double wide[3] = {80, 100, 140};
    for (int i = 0; i < 3; ++i) {
        const double C = -std::exp(-2 * kPi * wide[i] / rate), B = 2 * std::exp(-kPi * wide[i] / rate) * std::cos(2 * kPi * f[i] / rate), A = 1 - B - C;
        double y1 = 0, y2 = 0;
        for (double &v : x) {
            const double y = A * v + B * y1 + C * y2;
            y2 = y1, y1 = y, v = y;
        }
    }
    double e = 0;
    for (double v : x) e += v * v;
    const double g = std::pow(10, db / 20) / std::sqrt(e / n);
    std::vector<float> out(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) out[size_t(i)] = float(x[size_t(i)] * g);
    return out;
}

// A room's hiss, `db` loud: the same every run.
static std::vector<float> hiss(double seconds, double db, double rate = kRate) {
    std::vector<float> out(size_t(seconds * rate), 0.0f);
    uint32_t r = 7;
    const double g = std::pow(10, db / 20) * std::sqrt(3.0);
    for (float &v : out) r = r * 1664525u + 1013904223u, v = float(g * (double(r >> 8) / double(1 << 23) - 1));
    return out;
}

// A mouth hears `s` a draw at a time. The weights after the last draw.
static void hear(face::Mouth &m, const std::vector<float> &s, float w[face::kWeights], double rate = kRate) {
    const size_t step = size_t(rate * kTick);
    for (size_t at = 0; at + step <= s.size(); at += step) m.Hear(&s[at], step, rate), m.After(kTick, w);
}

static double open(const float w[face::kWeights]) { return w[Shape::kAa] + w[Shape::kIh] + w[Shape::kOu] + w[Shape::kEe] + w[Shape::kOh]; }

static void blinks() {
    face::Blink b(3);
    int n = 0, pairs = 0;
    double was = 0, top = 0, lastEnd = -1, shortest = 1e9, longest = 0, shutFor = 0;
    bool ranged = true, smooth = true;
    const double dt = 0.005;
    for (double t = 0; t < 600; t += dt) {
        const double v = b.After(dt);
        ranged = ranged && v >= 0 && v <= 1;
        smooth = smooth && std::fabs(v - was) <= dt / face::Blink::kShutS + 1e-9;
        if (v > 0) shutFor += dt;
        if (was == 0 && v > 0) {
            ++n, top = 0;
            if (lastEnd >= 0) {
                const double gap = t - lastEnd;
                if (gap < 0.5) ++pairs;
                else shortest = std::min(shortest, gap), longest = std::max(longest, gap);
            }
        }
        top = std::max(top, v);
        if (was > 0 && v == 0) lastEnd = t, smooth = smooth && top > 0.9;
        was = v;
    }
    check("the eyelids stay between open and shut, and never jump", ranged && smooth);
    check("a blink every few seconds: ten to twenty a minute", n >= 100 && n <= 200);
    check("between blinks, two to six and a half seconds", shortest >= face::Blink::kShortestS - 0.02 && longest <= face::Blink::kLongestS + 0.02 && longest > 5);
    check("now and then two together", pairs >= 5 && pairs <= n / 4);
    check("the eyes are open nearly all the time", shutFor / 600 < 0.08);

    face::Blink one(1), other(2), same(1);
    bool together = true, alike = true;
    for (int i = 0; i < 20000; ++i) {
        const double a = one.After(dt), c = other.After(dt), d = same.After(dt);
        together = together && (a > 0) == (c > 0);
        alike = alike && a == d;
    }
    check("two faces with different seeds don't blink together; the same seed blinks the same", !together && alike);

    face::Blink odd(0);
    bool fine = true;
    for (double step : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 100.0, 1e-9}) {
        const double v = odd.After(step);
        fine = fine && v >= 0 && v <= 1;
    }
    check("odd steps of time (none, backward, not a number, a long one) leave the eyelids sound", fine);

    float w[face::kWeights] = {};
    face::Lids(0.7, true, w);
    const bool one_ = w[Shape::kBlink] == 0.7f && w[Shape::kBlinkLeft] == 0 && w[Shape::kBlinkRight] == 0;
    face::Lids(0.7, false, w);
    check("a face with a shape for both eyes uses it; one without shuts each eye", one_ && w[Shape::kBlink] == 0 && w[Shape::kBlinkLeft] == 0.7f && w[Shape::kBlinkRight] == 0.7f);
}

// The vowels a mouth has shapes for, as formants: father, feet, food, bed, more.
static const double kAa[3] = {730, 1090, 2440}, kIh[3] = {270, 2290, 3010}, kOu[3] = {300, 870, 2240}, kEe[3] = {530, 1840, 2480}, kOh[3] = {570, 840, 2410};

static void vowels() {
    const struct {
        const char *name;
        const double *f;
        int shape;
    } kVowels[5] = {{"aa", kAa, Shape::kAa}, {"ih", kIh, Shape::kIh}, {"ou", kOu, Shape::kOu}, {"ee", kEe, Shape::kEe}, {"oh", kOh, Shape::kOh}};
    for (const auto &v : kVowels) {
        bool mostly = true, wide = true;
        for (double pitch : {110.0, 160.0, 210.0, 260.0})
            for (double rate : {16000.0, 48000.0}) {
                face::Mouth m;
                float w[face::kWeights] = {};
                hear(m, hiss(1, -62, rate), w, rate);
                hear(m, vowel(v.f, 0.5, -22, pitch, rate), w, rate);
                mostly = mostly && w[v.shape] > 0.55 * open(w);   // some sit between two shapes: the rest is a neighbour's
                wide = wide && open(w) > 0.9 && open(w) < 1.0001;
            }
        char what[128];
        std::snprintf(what, sizeof what, "a voice's %s opens the mouth whole and no further, mostly in that vowel's shape, at any pitch and rate", v.name);
        check(what, mostly && wide);
    }

    double out[face::kWeights];
    bool whole = true;
    for (double low : {1e-9, 1e-4, 1.0})
        for (double mid : {1e-9, 1e-4, 1.0})
            for (double high : {0.0, 1e-9, 1e-4, 1.0}) {
                face::Mouth::Vowels(low, mid, high, out);
                double sum = 0;
                for (int id = 0; id < face::kWeights; ++id) sum += out[id], whole = whole && out[id] >= 0;
                whole = whole && std::fabs(sum - 1) < 1e-9 && out[0] == 0 && out[Shape::kBlink] == 0;
            }
    check("whatever the bands hold, the vowels' shares are a whole and no other shape's", whole);
}

static void loudness() {
    float w[face::kWeights] = {};
    {
        face::Mouth m;
        hear(m, std::vector<float>(size_t(kRate) * 2, 0.0f), w);
        check("silence leaves the mouth shut", open(w) == 0);
    }
    {
        face::Mouth m;
        hear(m, hiss(6, -38), w);   // a fan
        const bool shut = open(w) < 0.01;
        hear(m, vowel(kAa, 0.4, -14, 140), w);
        check("a steady noise is the room's: the mouth stays shut in it, and a voice over it still opens it", shut && open(w) > 0.5 && m.floorDb() > -40);
    }
    {
        face::Mouth m;
        hear(m, hiss(1, -62), w);
        hear(m, vowel(kAa, 0.4, -47, 140), w);   // 15 dB over the room: a murmur
        const double soft = open(w);
        hear(m, hiss(0.5, -62), w);
        hear(m, vowel(kAa, 0.4, -30, 140), w);
        check("a soft voice opens the mouth a little, a full one fully", soft > 0.1 && soft < 0.5 && open(w) > 0.95);
    }
    {
        face::Mouth m;
        hear(m, hiss(1, -62), w);
        const std::vector<float> voice = vowel(kAa, 1, -22, 140);
        const size_t step = size_t(kRate * kTick);
        int opened = -1, shut = -1;
        bool gentle = true;
        double was = 0;
        for (int i = 0; i < 18; ++i) {
            m.Hear(&voice[size_t(i) * step], step, kRate), m.After(kTick, w);
            gentle = gentle && open(w) - was < 0.75;
            if (opened < 0 && open(w) > 0.8) opened = i;
            was = open(w);
        }
        const std::vector<float> quiet = hiss(1, -62);
        for (int i = 0; i < 30; ++i) {
            m.Hear(&quiet[size_t(i) * step], step, kRate), m.After(kTick, w);
            gentle = gentle && was - open(w) < 0.5;
            if (shut < 0 && open(w) < 0.1) shut = i;
            was = open(w);
        }
        check("the mouth opens with the voice within a tenth of a second, and shuts a little slower, never in one step",
              gentle && opened >= 1 && opened <= 4 && shut >= 3 && shut <= 12);
    }
    {
        face::Mouth m;
        hear(m, hiss(1, -62), w);
        hear(m, vowel(kAa, 0.4, -22, 140), w);
        const bool was = open(w) > 0.9;
        for (int i = 0; i < 6; ++i) m.After(kTick, w);   // a draw or two with nothing new is no reason to shut
        const bool held = open(w) > 0.9;
        for (int i = 0; i < 40; ++i) m.After(kTick, w);
        check("a microphone that stops shuts the mouth after a quarter of a second, not before", was && held && open(w) < 0.01);
    }
    {
        face::Mouth m;
        std::vector<float> bad = vowel(kAa, 0.4, -22, 140);
        for (size_t i = 0; i < bad.size(); i += 7) bad[i] = i % 2 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
        hear(m, hiss(1, -62), w);
        hear(m, bad, w);
        bool sound = true;
        for (int id = 0; id < face::kWeights; ++id) sound = sound && std::isfinite(w[id]) && w[id] >= 0 && w[id] <= 1;
        const float before = w[Shape::kAa];
        m.Hear(bad.data(), bad.size(), 4000), m.Hear(bad.data(), bad.size(), -1), m.Hear(bad.data(), 0, kRate), m.After(-1, w), m.After(0, w);
        check("samples that aren't numbers, a rate too slow to tell vowels by and steps of no time leave the mouth sound", sound && open(w) > 0.5 && w[Shape::kAa] == before);
    }
    {
        face::Mouth m;
        float all[face::kWeights];
        for (float &v : all) v = 0.25f;
        hear(m, hiss(1, -62), all);
        check("the mouth sets its five shapes and leaves the eyes' alone", all[0] == 0.25f && all[Shape::kBlink] == 0.25f && all[Shape::kBlinkLeft] == 0.25f && all[Shape::kBlinkRight] == 0.25f && open(all) < 0.01);
    }
}

int main() {
    blinks();
    vowels();
    loudness();
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
