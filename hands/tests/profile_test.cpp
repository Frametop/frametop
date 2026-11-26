// Unit tests for the hand profile (track/profile.h): what's written is what's read, a damaged or
// implausible file does no harm, and a calibration seeded from one starts settled and goes on
// learning. No models, no recordings, no cameras. make check (in the dev container) builds and
// runs it.
#include "../include/fh_shm.hpp"
#include "../include/fh_verdicts.h"
#include "../track/profile.h"

#include <cstddef>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace skel;

namespace {

int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures;   \
    } while (0)

// A hand `scale` times the model's average, open and flat, in its own palm frame: what
// Calibration::add takes (Skeleton::update hands it each clean stereo frame so).
void flat_hand(double scale, V3 local[21]) {
    const double q[kParams] = {};
    Rot frames[21];
    Skeleton::pose(Skeleton::canonical(scale), 1, Rot{}, V3{0, 0, 0}, q, local, frames);
}

void feed(Calibration &c, double scale, int frames) {
    V3 local[21];
    flat_hand(scale, local);
    bool ok[21], finger_ok[5] = {true, true, true, true, true};
    for (bool &b : ok) b = true;
    for (int i = 0; i < frames; ++i) c.add(local, ok, finger_ok);
}

double biggest_difference(const Shape &a, const Shape &b) {
    double worst = 0;
    for (int k : {1, 5, 9, 13, 17}) worst = std::max(worst, norm(a.palm[k] - b.palm[k]));
    for (int b_ = 0; b_ < 20; ++b_) worst = std::max(worst, std::fabs(a.len[b_] - b.len[b_]));
    return worst;
}

profile::Side side_of(const Calibration &c, double scale, long scale_n) {
    profile::Side s;
    c.quantities(s.q, s.n);
    s.scale.value = scale, s.scale.n = scale_n;
    return s;
}

}  // namespace

int main() {
    // --- Running: the mean of the first 45, then bounded steps
    {
        profile::Running r;
        for (int i = 0; i < 45; ++i) r.add(i % 2 ? 1.1 : 1.3, 0.0005);
        CHECK(r.settled() && std::fabs(r.value - 1.2) < 0.01);
        const double before = r.value;
        r.add(1.6, 0.0005);   // one wild reading
        CHECK(std::fabs(r.value - before - 0.0005) < 1e-12);
        for (int i = 0; i < 1000; ++i) r.add(1.0, 0.0005);   // a lasting change gets there
        CHECK(std::fabs(r.value - 1.0) < 1e-9);
        r.add(NAN, 0.0005);
        CHECK(std::isfinite(r.value) && r.n == 45 + 1 + 1000);
        profile::Running few;
        for (int i = 0; i < 10; ++i) few.add(1.25, 0.0005);
        CHECK(!few.settled() && std::fabs(few.value - 1.25) < 1e-12);
    }

    // --- a calibration learned in one run
    Calibration learned[2];
    feed(learned[0], 1.18, 60);
    feed(learned[1], 1.22, 60);
    CHECK(learned[0].settled() == 1.0 && learned[1].settled() == 1.0);
    const Shape truth_l = Skeleton::canonical(1.18), truth_r = Skeleton::canonical(1.22);
    CHECK(biggest_difference(learned[0].shape(Skeleton::canonical(1.0)), truth_l) < 1e-6);

    // --- what's written is what's read
    profile::Profile p;
    p.side[0] = side_of(learned[0], 1.1812, 5321);
    p.side[1] = side_of(learned[1], 1.2204, 6120);
    CHECK(!p.empty() && p.side[0].settled() == kQuantities);
    const std::string body = profile::text(p);
    profile::Profile back;
    CHECK(profile::parse(body, back) == 6);   // a scale, a shape and its counts, per side
    CHECK(profile::text(back) == body);
    CHECK(std::fabs(back.side[1].scale.value - 1.2204) < 1e-9 && back.side[1].scale.n == 6120);
    for (int k = 0; k < kQuantities; ++k) {
        CHECK(std::fabs(back.side[0].q[k] - p.side[0].q[k]) < 1e-6);   // to the micrometre
        CHECK(back.side[0].n[k] == p.side[0].n[k]);
    }

    // --- the next run: the profile's shape is the fit's until this run's own settles
    {
        Skeleton sk;
        CHECK(biggest_difference(sk.shape(1, 1.0), Skeleton::canonical(1.0)) < 1e-12);   // no profile: the average hand
        skel::Shape prior;
        CHECK(profile::prior_shape(back.side[1], prior));
        sk.set_prior(1, prior);
        CHECK(biggest_difference(sk.shape(1, 1.0), truth_r) < 1e-5);   // the wearer's from frame 1
        CHECK(biggest_difference(sk.shape(-1, 1.0), Skeleton::canonical(1.0)) < 1e-12);   // only the side it was given
        feed(sk.calibration(1), 1.30, 10);    // a few samples of a bigger hand: still mostly the profile's
        const double early = biggest_difference(sk.shape(1, 1.0), truth_r);
        CHECK(early > 1e-4 && early < biggest_difference(Skeleton::canonical(1.30), truth_r) * 0.6);
        feed(sk.calibration(1), 1.30, 35);    // settled: this run's own, as without a profile
        CHECK(biggest_difference(sk.shape(1, 1.0), Skeleton::canonical(1.30)) < 1e-6);
        // a side that hadn't settled every quantity gives no prior
        profile::Side partial = back.side[0];
        partial.n[20] = 10;
        CHECK(!profile::prior_shape(partial, prior) && !profile::prior_shape(profile::Side{}, prior));
    }
    // --- what a run adds: halfway to it where both had settled, all of it where the profile had none
    {
        Calibration run_cal;
        feed(run_cal, 1.30, 60);
        profile::Side run = side_of(run_cal, 1.30, 900), merged;
        profile::merge(back.side[1], run, merged);
        const double want = 0.5 * (back.side[1].q[20] + run.q[20]);
        CHECK(std::fabs(merged.q[20] - want) < 1e-12 && merged.n[20] == back.side[1].n[20] + run.n[20]);
        CHECK(std::fabs(merged.scale.value - 0.5 * (1.2204 + 1.30)) < 1e-9 && merged.scale.n == 6120 + 900);
        profile::merge(profile::Side{}, run, merged);   // a first run: taken as it is
        CHECK(merged.q[20] == run.q[20] && merged.scale.value == 1.30);
        profile::Side unsettled = run;                  // a run that settled nothing changes nothing
        for (int k = 0; k < kQuantities; ++k) unsettled.n[k] = 20;
        unsettled.scale.n = 10;
        profile::merge(back.side[1], unsettled, merged);
        bool same = merged.scale.value == back.side[1].scale.value && merged.scale.n == back.side[1].scale.n;
        for (int k = 0; k < kQuantities; ++k) same = same && merged.q[k] == back.side[1].q[k] && merged.n[k] == back.side[1].n[k];
        CHECK(same);
        profile::Side wild = run;                       // nor one with a bone no hand has
        wild.q[20] = 0.5;
        profile::merge(back.side[1], wild, merged);
        CHECK(merged.q[20] == back.side[1].q[20] && merged.q[21] != back.side[1].q[21]);
        // repeated sessions with fresh per-run sample counts accumulate linearly, not exponentially
        profile::Side acc = back.side[1];
        for (int i = 0; i < 4; ++i) {
            profile::Side next;
            profile::merge(acc, run, next);
            acc = next;
        }
        CHECK(acc.scale.n == back.side[1].scale.n + 4 * run.scale.n);
        // clear_prior drops the loaded shape immediately
        Skeleton sk_clear;
        skel::Shape pr;
        CHECK(profile::prior_shape(back.side[1], pr));
        sk_clear.set_prior(1, pr);
        CHECK(biggest_difference(sk_clear.shape(1, 1.0), truth_r) < 1e-5);
        sk_clear.clear_prior();
        CHECK(biggest_difference(sk_clear.shape(1, 1.0), Skeleton::canonical(1.0)) < 1e-12);
    }

    // --- second-wearer guard: protects the saved profile when someone else wears the headset
    {
        profile::Side run[2];
        double ratio = 0;
        // empty saved profile never flags a mismatch
        CHECK(!profile::wearer_mismatch(profile::Profile{}, run, &ratio));
        // same wearer (within ±5% bone length): matches
        Calibration same_l, same_r;
        feed(same_l, 1.20, 60);
        feed(same_r, 1.24, 60);
        run[0] = side_of(same_l, 1.20, 200);
        run[1] = side_of(same_r, 1.24, 200);
        CHECK(!profile::wearer_mismatch(back, run, &ratio) && std::fabs(ratio - 1.017) < 0.02);
        // smaller-handed guest (~18% shorter finger bones): triggers on one settled side
        Calibration guest_small;
        feed(guest_small, 0.97, 60);
        profile::Side small_run[2];
        small_run[0] = side_of(guest_small, 0.97, 80);
        CHECK(profile::wearer_mismatch(back, small_run, &ratio) && ratio < 0.85);
        // larger-handed guest (~16% longer finger bones): triggers
        Calibration guest_big;
        feed(guest_big, 1.40, 60);
        profile::Side big_run[2];
        big_run[1] = side_of(guest_big, 1.40, 80);
        CHECK(profile::wearer_mismatch(back, big_run, &ratio) && ratio > 1.12);
        // scale-only fallback (no settled shapes): requires both sides >= kGuestScaleMinSamples and >14% diff
        profile::Side scale_only[2];
        scale_only[0].scale = {0.95, 100};
        scale_only[1].scale = {0.95, 100};
        CHECK(!profile::wearer_mismatch(back, scale_only, &ratio));   // under 150 frames: wait
        scale_only[0].scale.n = 160;
        CHECK(!profile::wearer_mismatch(back, scale_only, &ratio));   // only one side past 150: wait
        scale_only[1].scale.n = 160;
        CHECK(profile::wearer_mismatch(back, scale_only, &ratio) && ratio < 0.82);   // both past 150 & >14% off
        // single-side lighting bias with matching mean does not trigger scale-only mismatch
        scale_only[0].scale = {1.38, 200};
        scale_only[1].scale = {1.06, 200};
        CHECK(!profile::wearer_mismatch(back, scale_only, &ratio));
    }

    // --- the search's tiles: more where hands turn up, never none
    {
        std::map<std::string, std::vector<profile::TileStat>> tiles;
        CHECK(profile::mean_rate(tiles) == 0 && profile::tile_boost({}, 0) == 1);   // nothing found yet: as before
        tiles["slam_left"] = {{400, 80}, {400, 4}, {0, 0}, {10, 1}};
        tiles["upper_left"] = {{200, 16}};
        const double mean = profile::mean_rate(tiles);
        CHECK(std::fabs(mean - 101.0 / 1010.0) < 1e-12);
        const double hot = profile::tile_boost(tiles["slam_left"][0], mean), cold = profile::tile_boost(tiles["slam_left"][1], mean);
        CHECK(hot > 1.8 && hot < 2.0);                                       // 20% against 10%, pulled a little to the mean
        CHECK(cold == profile::kTileBoostMin);                               // 1%: half as often, no less
        CHECK(profile::tile_boost(tiles["slam_left"][2], mean) == 1.0);      // never tried: its fixed weight
        CHECK(std::fabs(profile::tile_boost(tiles["slam_left"][3], mean) - 1.0) < 0.05);   // hardly tried: nearly
        CHECK(profile::tile_boost({50, 50}, mean) == profile::kTileBoostMax);              // every try: 4 times, no more
        profile::forget_old(tiles);                                          // 1010 tries: nothing forgotten yet
        CHECK(tiles["slam_left"][0].tries == 400);
        tiles["upper_left"][0].tries = 30000;
        profile::forget_old(tiles);
        CHECK(tiles["slam_left"][0].tries == 200 && tiles["slam_left"][0].finds == 40 && tiles["upper_left"][0].tries == 15000);
        // written and read with the rest
        profile::Profile with = p, read;
        with.tiles = tiles;
        const std::string t = profile::text(with);
        CHECK(profile::parse(t, read) == 8 && profile::text(read) == t);
        CHECK(read.tiles["slam_left"].size() == 4 && read.tiles["slam_left"][0].finds == 40 && read.tiles.count("upper_left"));
        profile::Profile only;
        only.tiles["slam_right"] = {{3, 1}};
        CHECK(!only.empty());
        // a line that doesn't add up is left out: fewer numbers than it says, more finds than tries, no tiles
        CHECK(profile::parse("format 1\ntiles slam_left 3 10 1 10 2\ntiles slam_right 1 5 9\ntiles upper_left 0\n", read) == 0);
        CHECK(read.empty());
    }

    // --- the wearer's pinches: counted, and the detector's two distances moved to them
    {
        using profile::PinchStat;
        double bins[profile::kPinchBins] = {};
        CHECK(profile::quantile(bins, 0.5) == -1);
        bins[10] = 50, bins[20] = 30, bins[30] = 20;   // 10-11 mm, 20-21 mm, 30-31 mm
        CHECK(profile::quantile(bins, 0.5) == 0.011 && profile::quantile(bins, 0.8) == 0.021 && profile::quantile(bins, 0.97) == 0.031);
        const double begin = 0.020, dip = 0.032;
        auto same = [&](const PinchStat &s) {
            const profile::PinchLearned l = profile::learned_pinch(s, begin, dip);
            return l.begin_m == begin && l.dip_max_m == dip;
        };
        auto fill = [](double *b, int from_mm, int to_mm, double each) {
            for (int mm = from_mm; mm < to_mm; ++mm) b[mm] += each;
        };
        PinchStat firm;                          // taps that close well: 8 to 16 mm
        fill(firm.taps, 8, 16, 5);               // 40 taps: too few to go by
        CHECK(same(firm));
        fill(firm.taps, 8, 16, 20);              // 200
        fill(firm.idle, 40, 50, 500);            // an idle hand that stays open
        profile::PinchLearned l = profile::learned_pinch(firm, begin, dip);
        CHECK(l.begin_m == begin);               // nothing to gain: it stays
        CHECK(l.dip_max_m == profile::kDipMaxFloor);   // dips past 24 mm aren't this wearer's taps
        PinchStat shallow;                       // a wearer whose quick taps often read 20 to 26 mm
        fill(shallow.taps, 10, 20, 12);          // 120 reach under 20
        fill(shallow.taps, 20, 26, 20);          // 120 don't (the dip rule caught them)
        CHECK(profile::learned_pinch(shallow, begin, dip).begin_m == begin);   // no idle samples yet: no move up
        fill(shallow.idle, 40, 50, 500);
        l = profile::learned_pinch(shallow, begin, dip);
        CHECK(std::fabs(l.begin_m - 0.026) < 1e-12);     // 80% under 25 mm, plus 2: capped at 6 mm over
        CHECK(std::fabs(l.dip_max_m - 0.029) < 1e-12);   // 97% under 26 mm, plus 3
        PinchStat typist = shallow;              // the same taps, but resting fingers come within 2 cm
        fill(typist.idle, 15, 20, 30);           // 150 of 5150: 3%
        l = profile::learned_pinch(typist, begin, dip);
        CHECK(l.begin_m == begin && std::fabs(l.dip_max_m - 0.029) < 1e-12);
        PinchStat close_idle = shallow;          // idle fingers down to 26 mm: begin may reach 23, not 26
        fill(close_idle.idle, 26, 28, 100);
        l = profile::learned_pinch(close_idle, begin, dip);
        CHECK(std::fabs(l.begin_m - 0.024) < 1e-12);     // 1% of the idle under 27 mm, less 3
        // never below the detector's own begin, never above its dip limit, and room for the dip rule
        for (const PinchStat &s : {firm, shallow, typist, close_idle}) {
            const profile::PinchLearned any = profile::learned_pinch(s, begin, dip);
            CHECK(any.begin_m >= begin && any.begin_m <= begin + profile::kBeginRise && any.dip_max_m <= dip &&
                  any.dip_max_m >= any.begin_m + 0.002 - 1e-12);
        }

        // the observer, at 30 frames a second: an open hand, a tap, an open hand, a pinch lost, a hand gone
        profile::PinchObserver seen;
        int64_t t = 0;
        const int64_t frame = 33'333'333;
        auto run = [&](int frames, double d, bool down, bool began = false, bool ended = false, bool lost = false) {
            for (int i = 0; i < frames; ++i, t += frame) seen.update(1, t, d, down, began && i == 0, ended && i == 0, lost && i == 0);
        };
        run(60, 0.060, false);                    // 2 s open at 60 mm
        run(4, 0.030, false);                     // closing
        run(1, 0.018, true, true);                // begins
        run(5, 0.012, true);                      // down, to 12 mm
        run(3, 0.040, false, false, true);        // ends, the fingers still opening
        run(60, 0.060, false);                    // 2 s open again
        CHECK(seen.stat[1].tap_n() == 1 && seen.stat[1].taps[12] == 1);
        CHECK(seen.stat[1].idle[30] == 0 && seen.stat[1].idle[40] == 0);   // the closing and the opening aren't idle
        const double idle_before_lost = seen.stat[1].idle_n();
        // 60 mm is past the last bin, which takes it: 2 s at 10 a second, less 0.3 s before the pinch;
        // then 2 s less the 0.3 s after it, and less the 0.3 s still waiting at the end
        CHECK(idle_before_lost >= 28 && idle_before_lost <= 34 && seen.stat[1].idle[49] == idle_before_lost);
        if (!(idle_before_lost >= 28 && idle_before_lost <= 34)) std::printf("idle samples: %.0f\n", idle_before_lost);
        run(1, 0.015, true, true);
        run(3, 0.015, true);
        run(1, -1, false, false, false, true);    // the hand went: no tap
        CHECK(seen.stat[1].tap_n() == 1);
        run(30, -1, false);                       // no hand: nothing counted
        CHECK(seen.stat[1].idle_n() <= idle_before_lost + 3 && seen.stat[0].idle_n() == 0);
        seen.update(2, t, 0.05, false, false, false, false);   // not a side: ignored

        // in the file with the rest
        profile::Profile with = p, read;
        with.pinch[0] = shallow, with.pinch[1] = seen.stat[1];
        const std::string text = profile::text(with);
        CHECK(profile::parse(text, read) == 6 + 4 && profile::text(read) == text);
        CHECK(read.pinch[0].tap_n() == shallow.tap_n() && read.pinch[1].taps[12] == 1);
        CHECK(profile::parse("format 1\ntaps left 1 2 3\nidle right " + std::string(100, 'x') + "\n", read) == 0 && read.empty());
        std::string negative = "format 1\ntaps left";
        for (int b = 0; b < profile::kPinchBins; ++b) negative += b == 7 ? " -1" : " 0";
        CHECK(profile::parse(negative + "\n", read) == 0);
    }

    // --- with confirm, a pinch counts only once the pointer helper says it did something
    {
        profile::PinchObserver seen;
        seen.confirm = true;
        const int64_t ms = 1'000'000;
        int64_t t = 10'000 * ms;
        auto tap = [&](double low, int64_t at) {   // a quick tap at `at`: begins, bottoms out, ends 60 ms later
            seen.update(1, at, 0.019, true, true, false, false);
            seen.update(1, at + 30 * ms, low, true, false, false, false);
            seen.update(1, at + 60 * ms, 0.050, false, false, true, false);
            return at;
        };
        const int64_t a = tap(0.012, t);
        CHECK(seen.stat[1].tap_n() == 0);                     // ended, but no verdict yet
        seen.verdict(1, a, true, a + 80 * ms);                // the helper clicked: it counts
        CHECK(seen.stat[1].tap_n() == 1 && seen.stat[1].taps[12] == 1 && seen.confirmed == 1);
        const int64_t b = tap(0.025, t + 1000 * ms);
        seen.verdict(1, b, false, b + 80 * ms);               // held back (typing, say): it doesn't
        CHECK(seen.stat[1].tap_n() == 1 && seen.refused == 1);
        const int64_t c = t + 2000 * ms;
        seen.verdict(1, c, false, c + 5 * ms);                // ignored as it began, before it ended
        tap(0.022, c);
        CHECK(seen.stat[1].tap_n() == 1 && seen.refused == 2);
        const int64_t d = tap(0.015, t + 3000 * ms);
        seen.verdict(1, d + 1, true, d + 80 * ms);            // a verdict for some other pinch: no match
        seen.verdict(0, d, true, d + 80 * ms);                // nor one for the other side
        CHECK(seen.stat[1].tap_n() == 1);
        seen.update(1, d + 3100 * ms, 0.06, false, false, false, false);   // no answer within 3 s: dropped
        CHECK(seen.stat[1].tap_n() == 1 && seen.unanswered == 1);
        seen.verdict(1, d, true, d + 3200 * ms);              // too late: it was dropped
        CHECK(seen.stat[1].tap_n() == 1);
        // the pinch a grip or a lost hand ended waits for nothing
        seen.update(1, t + 9000 * ms, 0.015, true, true, false, false);
        seen.update(1, t + 9030 * ms, -1, false, false, false, true);
        seen.update(1, t + 13000 * ms, 0.06, false, false, false, false);
        CHECK(seen.unanswered == 1 && seen.confirmed == 1 && seen.refused == 2);
    }

    // --- the verdicts file: written by the helper's side, read by ft-hands' (fh_shm.hpp)
    {
        char dir[] = "/tmp/ft-verdicts-test-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        fh::ShmFile file(FH_VERDICTS_FILE, FH_VERDICTS_MAGIC, offsetof(fh_verdicts_t, ring), dir,
                         std::chrono::seconds(0));   // look again every read (live: once a second)
        fh_verdicts_t v;
        CHECK(!file.Read(&v, sizeof v, offsetof(fh_verdicts_t, seq)));   // none yet
        for (int i = 1; i <= 40; ++i) fh_verdict(dir, i % 2, 0, i % 3 ? FH_VERDICT_DONE : FH_VERDICT_REFUSED, uint64_t(i) * 1000);
        fh_verdict(dir, 2, 0, FH_VERDICT_DONE, 5);   // not a side: not written
        fh_verdict(dir, 0, 0, FH_VERDICT_DONE, 0);   // no gesture: not written
        CHECK(file.Read(&v, sizeof v, offsetof(fh_verdicts_t, seq)));
        CHECK(v.count == 40 && v.version == FH_VERDICTS_VERSION && v.seq % 2 == 0);
        const fh_verdict_t &newest = v.ring[(v.count - 1) % FH_VERDICTS_RING], &oldest = v.ring[v.count % FH_VERDICTS_RING];
        CHECK(newest.begin_ns == 40000 && newest.side == 0 && newest.outcome == FH_VERDICT_DONE);
        CHECK(oldest.begin_ns == 9000 && oldest.outcome == FH_VERDICT_REFUSED);   // the 9th: the 8 before are gone
        struct stat st;
        CHECK(stat((std::string(dir) + "/" FH_VERDICTS_FILE).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        unlink((std::string(dir) + "/" FH_VERDICTS_FILE).c_str());
        rmdir(dir);
    }

    // --- damage does no harm
    {
        profile::Profile q;
        CHECK(profile::parse("", q) == 0 && q.empty());
        CHECK(profile::parse("format 2\n" + body.substr(body.find("scale")), q) == 0 && q.empty());   // a newer file
        CHECK(profile::parse("scale left 1.2 100\n", q) == 0 && q.empty());                           // no format line
        // unknown lines and unknown sides are skipped; the rest is kept
        CHECK(profile::parse("format 1\ntiles slam_left 3 1 2 3\nscale middle 1.2 9\nscale left 1.25 90\n", q) == 1);
        CHECK(std::fabs(q.side[0].scale.value - 1.25) < 1e-9 && q.side[1].scale.n == 0);
        // a scale no hand has, or with no samples behind it
        CHECK(profile::parse("format 1\nscale left 2.5 100\nscale right 1.2 0\nscale right nan 5\n", q) == 0 && q.empty());
        // a shape cut short, one with a bone a metre long, and one without its counts
        const size_t at = body.find("shape left"), end = body.find('\n', at);
        const std::string shape_line = body.substr(at, end - at);
        CHECK(profile::parse("format 1\n" + shape_line.substr(0, shape_line.size() / 2) + "\n", q) == 0 && q.empty());
        std::string long_bone = shape_line;
        long_bone.replace(long_bone.rfind(' '), std::string::npos, " 1.000000");
        CHECK(profile::parse("format 1\n" + long_bone + "\n", q) == 0 && q.empty());
        CHECK(profile::parse("format 1\n" + shape_line + "\n", q) == 0 && q.side[0].settled() == 0);
        CHECK(!profile::plausible(0, 0.2) && profile::plausible(0, -0.03) && !profile::plausible(20, 0.001) &&
              !profile::plausible(20, NAN) && profile::plausible(20, 0.03));
    }

    // --- the file: none is fine, written whole and private, read back
    {
        char dir[] = "/tmp/ft-profile-test-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        const std::string path = std::string(dir) + "/profile";
        profile::Profile q;
        std::string err;
        CHECK(profile::load(path, q, err) && q.empty() && err.empty());   // no file yet: not an error
        CHECK(profile::save(path, p, err) && err.empty());
        struct stat st;
        CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
        CHECK(profile::load(path, q, err) && profile::text(q) == body);
        FILE *f = std::fopen(path.c_str(), "w");
        std::fputs("this is not a profile\n", f);
        std::fclose(f);
        CHECK(!profile::load(path, q, err) && q.empty() && !err.empty());
        CHECK(!profile::save(std::string(dir) + "/missing/profile", p, err));
        unlink(path.c_str());
        rmdir(dir);
    }

    if (failures) return std::printf("%d failure(s)\n", failures), 1;
    std::printf("profile: all checks passed\n");
    return 0;
}
