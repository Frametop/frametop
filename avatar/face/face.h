// An avatar's face while someone wears it, the sums: when its eyes blink, and how its mouth
// moves with the wearer's voice. Both come out as weights for a model file's face shapes
// (bodyfile::Shape::Id), for bodygpu::Avatar::Shape.
//
// The mouth is rough on purpose: how loud the voice is over the room's own noise opens it, and
// where the sound's energy lies (low, middle, high) leans it toward a vowel's shape. It is
// enough for a mouth that moves with speech, not for reading lips. The shapes are VRM's five,
// the vowels of "father", "feet", "food", "bed" and "more": aa, ih, ou, ee, oh.
//
// Header only, no microphone, GPU or SteamVR calls (the tests use it; micear.h hears).
#pragma once

#include "../bodyfile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace face {

constexpr int kWeights = bodyfile::Shape::kLastId + 1;   // by Shape::Id: [0] is unused

// The eyes: a blink every few seconds, now and then two together.
class Blink {
public:
    // Two faces with different seeds don't blink together.
    explicit Blink(uint32_t seed = 1) : rng_(seed ? seed : 1) { gap_ = Gap(); }

    // The eyelids `dt` seconds on from the call before: 0 open, 1 shut.
    double After(double dt) {
        if (dt > 0) t_ += std::min(dt, 1.0);
        if (t_ >= gap_ + kShutS + kHeldS + kOpenS) t_ = 0, gap_ = Gap();
        const double x = t_ - gap_;
        if (x <= 0) return 0;
        if (x < kShutS) return x / kShutS;
        if (x < kShutS + kHeldS) return 1;
        return std::max(0.0, 1 - (x - kShutS - kHeldS) / kOpenS);
    }

    static constexpr double kShutS = 0.07, kHeldS = 0.03, kOpenS = 0.11;   // a blink: shutting, shut, opening
    static constexpr double kShortestS = 2.0, kLongestS = 6.5, kTwiceS = 0.16;   // between blinks; between a pair

private:
    double Gap() {
        rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
        const double u = double(rng_ >> 8) / double(1 << 24);
        return u < 1.0 / 7 ? kTwiceS : kShortestS + (kLongestS - kShortestS) * (u - 1.0 / 7) * 7 / 6;
    }
    uint32_t rng_;
    double t_ = 0, gap_ = 0;   // since the last blink ended; how long after it the next begins
};

// The eyelids (Blink::After) into the weights: the shape that shuts both eyes when the face has
// one (`hasBlink`), else each eye's own.
inline void Lids(double shut, bool hasBlink, float weights[kWeights]) {
    weights[bodyfile::Shape::kBlink] = hasBlink ? float(shut) : 0;
    weights[bodyfile::Shape::kBlinkLeft] = weights[bodyfile::Shape::kBlinkRight] = hasBlink ? 0 : float(shut);
}

// The mouth, from the wearer's voice.
class Mouth {
public:
    // The voice as it comes: mono samples (-1 to 1) at `rate` a second, any number at a time.
    void Hear(const float *samples, size_t n, double rate) {
        if (!(rate >= kSlowestRate)) return;
        if (rate != rate_) Tune(rate);
        for (size_t i = 0; i < n; ++i) {
            const double x = samples[i];
            if (!std::isfinite(x)) continue;
            all_ += x * x, ++heard_;
            for (Band &b : band_) {
                double y = x;
                for (int pass = 0; pass < 2; ++pass) {   // the band-pass twice over: what is far from the band is well out of it
                    double *z = b.z + 2 * pass;
                    const double in = y;
                    y = b.b0 * in + z[0];
                    z[0] = -b.a1 * y + z[1], z[1] = -b.b0 * in - b.a2 * y;
                }
                b.heard += y * y;
            }
        }
    }

    // The mouth `dt` seconds on from the call before, from what was heard since: the weights'
    // five mouth shapes are set (the others are left as they are).
    void After(double dt, float weights[kWeights]) {
        if (!(dt > 0)) dt = 0;
        if (heard_ > 0) {
            Tell(dt);
            deaf_ = 0;
        } else if ((deaf_ += dt) > kDeafS) {
            std::fill(want_, want_ + kWeights, 0.0);   // nothing comes from the microphone: the mouth shuts
        }
        // the five move together, quicker when the mouth is opening: between them they never open it past whole
        double wanted = 0, open = 0;
        for (int id = bodyfile::Shape::kAa; id <= int(bodyfile::Shape::kOh); ++id) wanted += want_[id], open += now_[id];
        const double step = 1 - std::exp(-dt / (wanted > open ? kOpensS : kShutsS));
        for (int id = bodyfile::Shape::kAa; id <= int(bodyfile::Shape::kOh); ++id) {
            now_[id] += (want_[id] - now_[id]) * step;
            weights[id] = float(now_[id]);
        }
    }

    // How a sound's energy in the three bands (low, middle, high; any scale) is shared between
    // the vowels' shapes: `out` by Shape::Id, summing to 1.
    static void Vowels(double low, double mid, double high, double out[kWeights]) {
        std::fill(out, out + kWeights, 0.0);
        if (!(low > 0 && mid > 0 && high > 0)) return void(out[bodyfile::Shape::kAa] = 1);
        const double midOverLow = 10 * std::log10(mid / low), highOverMid = 10 * std::log10(high / mid);   // dB
        // a high band that keeps up with the middle one is a spread mouth: ih, or ee with more in the middle
        const double spread = Ramp(highOverMid, kSpreadFromDb, kSpreadFullDb), ee = spread * Ramp(midOverLow, kEeFromDb, kEeFullDb);
        // else the middle band opens it wide (aa), and the low band rounds it (oh, then ou)
        const double wide = Ramp(midOverLow, kWideFromDb, kWideFullDb), oh = (1 - wide) * Ramp(midOverLow, kOhFromDb, kOhFullDb);
        out[bodyfile::Shape::kEe] = ee, out[bodyfile::Shape::kIh] = spread - ee;
        out[bodyfile::Shape::kAa] = (1 - spread) * wide;
        out[bodyfile::Shape::kOh] = (1 - spread) * oh, out[bodyfile::Shape::kOu] = (1 - spread) * (1 - wide - oh);
    }

    double floorDb() const { return floor_; }   // the room's noise as it is taken to be, dB of full scale
    double levelDb() const { return level_; }   // what was last heard

    static constexpr double kSlowestRate = 8000;
    static constexpr double kOverDb = 10, kRangeDb = 18;         // this far over the room's noise the mouth starts to open; this much further it is open
    static constexpr double kQuietestDb = -72, kLoudestFloorDb = -36;   // the noise is taken to be within these (dB of full scale)
    static constexpr double kFloorS = 3;                         // the noise is the quietest it has been in this long
    static constexpr double kOpensS = 0.035, kShutsS = 0.08, kDeafS = 0.25;
    static constexpr double kBandHz[3] = {350, 850, 2300}, kBandQ = 1.4;
    // the middle band over the low one, and the high one over the middle, in dB: where each vowel begins and is whole
    static constexpr double kSpreadFromDb = -15, kSpreadFullDb = -8, kEeFromDb = -13, kEeFullDb = -6;
    static constexpr double kWideFromDb = 4, kWideFullDb = 9, kOhFromDb = -8, kOhFullDb = -1;

private:
    struct Band {
        double b0 = 0, a1 = 0, a2 = 0;   // a band-pass (b1 is 0, b2 is -b0), its peak at 1
        double z[4] = {}, heard = 0;         // two of them, one after the other
    };
    static constexpr int kSlots = 12;

    static double Ramp(double x, double from, double full) { return std::min(1.0, std::max(0.0, (x - from) / (full - from))); }

    void Tune(double rate) {
        rate_ = rate;
        for (int i = 0; i < 3; ++i) {
            const double w = 2 * 3.14159265358979323846 * kBandHz[i] / rate, alpha = std::sin(w) / (2 * kBandQ);
            band_[i] = Band{};
            band_[i].b0 = alpha / (1 + alpha), band_[i].a1 = -2 * std::cos(w) / (1 + alpha), band_[i].a2 = (1 - alpha) / (1 + alpha);
        }
    }

    // What was heard since the last call into want_, and forgotten.
    void Tell(double dt) {
        const double level = level_ = std::max(kQuietestDb, 10 * std::log10(all_ / double(heard_) + 1e-30));
        // the room's noise: the quietest it has been lately (speech has gaps; a fan doesn't)
        slotS_ += dt;
        if (slotS_ >= kFloorS / kSlots) slotS_ = 0, slot_ = (slot_ + 1) % kSlots, quiet_[slot_] = level;
        quiet_[slot_] = std::min(quiet_[slot_], level);
        floor_ = std::min(*std::min_element(quiet_, quiet_ + kSlots), kLoudestFloorDb);
        const double open = Ramp(level, floor_ + kOverDb, floor_ + kOverDb + kRangeDb);
        Vowels(band_[0].heard, band_[1].heard, band_[2].heard, want_);
        for (double &w : want_) w *= open;
        all_ = 0, heard_ = 0;
        for (Band &b : band_) b.heard = 0;
    }

    double rate_ = 0, all_ = 0;
    size_t heard_ = 0;
    Band band_[3];
    double quiet_[kSlots] = {-50, -50, -50, -50, -50, -50, -50, -50, -50, -50, -50, -50};
    double slotS_ = 0, floor_ = -50, level_ = kQuietestDb, deaf_ = 0;
    int slot_ = 0;
    double want_[kWeights] = {}, now_[kWeights] = {};
};

}  // namespace face
