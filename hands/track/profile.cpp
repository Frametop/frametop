#include "profile.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <numeric>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace profile {

namespace {
const char *const kSideName[2] = {"left", "right"};
constexpr long kMaxSamples = 1'000'000'000;   // a count is only a weight: it needn't grow past this

int side_of(const std::string &word) { return word == "left" ? 0 : word == "right" ? 1 : -1; }
}  // namespace

void Running::add(double x, double step) {
    if (!std::isfinite(x)) return;
    if (n < kSettle) value += (x - value) / double(n + 1);
    else value += std::clamp(x - value, -step, step);
    n = std::min(n + 1, kMaxSamples);
}

int Side::settled() const {
    int count = 0;
    for (int k = 0; k < skel::kQuantities; ++k) count += n[k] >= skel::Calibration::kSettle;
    return count;
}

bool prior_shape(const Side &s, skel::Shape &out) {
    if (s.settled() < skel::kQuantities) return false;
    out = skel::Shape{};
    out.from_quantities(s.q);
    return true;
}

void merge(const Side &before, const Side &run, Side &out) {
    Side m = before;
    for (int k = 0; k < skel::kQuantities; ++k) {
        if (run.n[k] < skel::Calibration::kSettle || !plausible(k, run.q[k])) continue;
        m.q[k] = before.n[k] >= skel::Calibration::kSettle ? 0.5 * (before.q[k] + run.q[k]) : run.q[k];
        m.n[k] = int(std::min(long(before.n[k]) + run.n[k], kMaxSamples));
    }
    if (run.scale.settled() && run.scale.value >= kScaleMin && run.scale.value <= kScaleMax) {
        m.scale.value = before.scale.settled() ? 0.5 * (before.scale.value + run.scale.value) : run.scale.value;
        m.scale.n = std::min(before.scale.n + run.scale.n, kMaxSamples);
    }
    out = m;
}

bool wearer_mismatch(const Profile &saved, const Side run[2], double *ratio_out) {
    std::vector<double> bone_ratios;
    for (int s = 0; s < 2; ++s) {
        if (saved.side[s].settled() < skel::kQuantities || run[s].settled() < skel::kQuantities) continue;
        for (int k = 15; k < skel::kQuantities; ++k) {
            if (saved.side[s].q[k] > 0.005 && run[s].q[k] > 0.005)
                bone_ratios.push_back(run[s].q[k] / saved.side[s].q[k]);
        }
    }
    if (!bone_ratios.empty()) {
        std::sort(bone_ratios.begin(), bone_ratios.end());
        const size_t mid = bone_ratios.size() / 2;
        const double med = bone_ratios.size() % 2 ? bone_ratios[mid] : 0.5 * (bone_ratios[mid - 1] + bone_ratios[mid]);
        if (ratio_out) *ratio_out = med;
        return std::fabs(med - 1.0) > kGuestBoneDiff;
    }
    if (saved.side[0].scale.settled() && saved.side[1].scale.settled() &&
        run[0].scale.n >= kGuestScaleMinSamples && run[1].scale.n >= kGuestScaleMinSamples) {
        const double saved_mean = 0.5 * (saved.side[0].scale.value + saved.side[1].scale.value);
        const double run_mean = 0.5 * (run[0].scale.value + run[1].scale.value);
        if (saved_mean > 0) {
            const double ratio = run_mean / saved_mean;
            if (ratio_out) *ratio_out = ratio;
            return std::fabs(ratio - 1.0) > kGuestScaleDiff;
        }
    }
    return false;
}

bool Profile::empty() const {
    for (const Side &s : side)
        if (s.scale.n > 0 || s.settled() > 0) return false;
    for (const auto &[cam, stats] : tiles)
        for (const TileStat &t : stats)
            if (t.tries > 0 || t.finds > 0) return false;
    for (const PinchStat &s : pinch)
        if (s.tap_n() > 0 || s.idle_n() > 0) return false;
    return true;
}

double PinchStat::tap_n() const { return std::accumulate(taps, taps + kPinchBins, 0.0); }
double PinchStat::idle_n() const { return std::accumulate(idle, idle + kPinchBins, 0.0); }
void PinchStat::add(const PinchStat &o) {
    for (int b = 0; b < kPinchBins; ++b) taps[b] += o.taps[b], idle[b] += o.idle[b];
}

double quantile(const double bins[kPinchBins], double q) {
    const double total = std::accumulate(bins, bins + kPinchBins, 0.0);
    if (!(total > 0)) return -1;
    double below = 0;
    for (int b = 0; b < kPinchBins; ++b) {
        below += bins[b];
        if (below >= q * total) return (b + 1) * 0.001;   // the bin's upper edge: everything in it is under that
    }
    return kPinchBins * 0.001;
}

PinchLearned learned_pinch(const PinchStat &s, double begin_m, double dip_max_m) {
    PinchLearned out{begin_m, dip_max_m};
    if (s.tap_n() < kPinchMinTaps) return out;
    out.dip_max_m = std::min(dip_max_m, std::max(kDipMaxFloor, quantile(s.taps, 0.97) + 0.003));
    if (s.idle_n() >= kPinchMinIdle) {
        const double wanted = quantile(s.taps, 0.80) + 0.002, safe = quantile(s.idle, 0.01) - 0.003;
        out.begin_m = std::clamp(std::min(wanted, safe), begin_m, begin_m + kBeginRise);
    }
    // the dip rule is for dips that don't reach begin_m: keep some room for it over begin_m
    out.dip_max_m = std::max(out.dip_max_m, std::min(dip_max_m, out.begin_m + 0.002));
    return out;
}

namespace {
int bin_of(double metres) { return std::clamp(int(metres * 1000), 0, kPinchBins - 1); }
void halve_past(double *bins, double limit) {
    if (std::accumulate(bins, bins + kPinchBins, 0.0) > limit)
        for (int b = 0; b < kPinchBins; ++b) bins[b] *= 0.5;
}
}  // namespace

void PinchObserver::update(int s, int64_t t_ns, double d, bool down, bool began, bool ended, bool lost) {
    if (s < 0 || s > 1) return;
    const int64_t clear = int64_t(kPinchClear * 1e9);
    if (began) {
        pending_[s].clear();   // those were the fingers closing, not a hand at rest
        low_[s] = d >= 0 ? d : 1;
        begin_ns_[s] = t_ns;
    }
    if (down && d >= 0) low_[s] = std::min(low_[s], d);
    if (ended || lost) {
        if (ended && !lost && low_[s] < 1) {
            if (confirm) {
                waiting_[s].push_back({begin_ns_[s], t_ns, low_[s]});
                while (waiting_[s].size() > 16) waiting_[s].pop_front(), ++unanswered;
            } else {
                stat[s].taps[bin_of(low_[s])] += 1;
                halve_past(stat[s].taps, kTapsForget);
            }
        }
        low_[s] = 1;
        quiet_until_[s] = t_ns + clear;   // nor the fingers opening again
        pending_[s].clear();
    }
    if (confirm) settle(s, t_ns);
    if (d < 0) {
        pending_[s].clear();   // the hand went: what it was doing just before isn't known
        return;
    }
    while (!pending_[s].empty() && t_ns - pending_[s].front().first >= clear) {
        stat[s].idle[bin_of(pending_[s].front().second)] += 1;
        pending_[s].pop_front();
        halve_past(stat[s].idle, kIdleForget);
    }
    // a little under kIdleEvery will do: at 30 sets a second, three sets are 99.99 ms
    if (!down && !began && !ended && !lost && t_ns >= quiet_until_[s] &&
        t_ns - last_idle_[s] >= int64_t(kIdleEvery * 1e9) - 5'000'000) {
        pending_[s].push_back({t_ns, d});
        last_idle_[s] = t_ns;
    }
}

void PinchObserver::verdict(int s, int64_t begin_ns, bool done, int64_t now_ns) {
    if (s < 0 || s > 1 || begin_ns <= 0) return;
    heard_[s].push_back({begin_ns, now_ns, done});
    while (heard_[s].size() > 64) heard_[s].pop_front();
    settle(s, now_ns);
}

void PinchObserver::settle(int s, int64_t now_ns) {
    for (auto w = waiting_[s].begin(); w != waiting_[s].end();) {
        const auto h = std::find_if(heard_[s].begin(), heard_[s].end(), [&](const Heard &x) { return x.begin_ns == w->begin_ns; });
        if (h != heard_[s].end()) {
            if (h->done) {
                stat[s].taps[bin_of(w->low)] += 1;
                halve_past(stat[s].taps, kTapsForget);
                ++confirmed;
            } else {
                ++refused;
            }
            heard_[s].erase(h);
            w = waiting_[s].erase(w);
        } else if (now_ns - w->ended_ns > int64_t(kVerdictWait * 1e9)) {
            ++unanswered;
            w = waiting_[s].erase(w);
        } else {
            ++w;
        }
    }
    // a verdict waits as long for its pinch to end: one held back as it began comes first
    const int64_t keep = int64_t(4 * kVerdictWait * 1e9);
    while (!heard_[s].empty() && now_ns - heard_[s].front().t_ns > keep) heard_[s].pop_front();
}

double mean_rate(const std::map<std::string, std::vector<TileStat>> &tiles) {
    double tries = 0, finds = 0;
    for (const auto &[cam, stats] : tiles)
        for (const TileStat &t : stats) tries += t.tries, finds += t.finds;
    return tries > 0 ? finds / tries : 0;
}

double tile_boost(const TileStat &t, double mean_rate) {
    if (!(mean_rate > 0)) return 1;   // nothing found anywhere yet: nothing to go by
    const double rate = (t.finds + kTilePrior * mean_rate) / (t.tries + kTilePrior);
    return std::clamp(rate / mean_rate, kTileBoostMin, kTileBoostMax);
}

void forget_old(std::map<std::string, std::vector<TileStat>> &tiles) {
    double tries = 0;
    for (const auto &[cam, stats] : tiles)
        for (const TileStat &t : stats) tries += t.tries;
    if (tries <= kTileForget) return;
    for (auto &[cam, stats] : tiles)
        for (TileStat &t : stats) t.tries *= 0.5, t.finds *= 0.5;
}

// The first 15 quantities are five palm points' coordinates (the knuckles and the thumb's base,
// from the wrist): within a hand's length of it. The other 15 are the fingers' bones.
bool plausible(int quantity, double metres) {
    if (!std::isfinite(metres)) return false;
    return quantity < 15 ? std::fabs(metres) < 0.15 : metres > 0.005 && metres < 0.08;
}

std::string default_path() {
    const char *state = std::getenv("XDG_STATE_HOME"), *home = std::getenv("HOME");
    std::string dir = state && *state ? state : std::string(home ? home : "") + "/.local/state";
    mkdir(dir.c_str(), 0700);
    for (const char *part : {"/frametop", "/hands"}) mkdir((dir += part).c_str(), 0700);
    return dir + "/profile";
}

std::string text(const Profile &p) {
    std::string out = "# Frametop hand profile (hands/track/profile.h): what ft-hands has learned about your hands.\n"
                      "# Numbers only. `ft-handsctl profile` shows it, `ft-handsctl profile forget` deletes it.\n"
                      "format 1\n";
    char buf[64];
    for (int s = 0; s < 2; ++s) {
        const Side &side = p.side[s];
        if (side.scale.n > 0) {
            std::snprintf(buf, sizeof buf, "scale %s %.4f %ld\n", kSideName[s], side.scale.value, side.scale.n);
            out += buf;
        }
        if (side.settled() == 0) continue;
        out += std::string("shape ") + kSideName[s];
        for (int k = 0; k < skel::kQuantities; ++k) {
            std::snprintf(buf, sizeof buf, " %.6f", side.q[k]);
            out += buf;
        }
        out += std::string("\ncount ") + kSideName[s];
        for (int k = 0; k < skel::kQuantities; ++k) out += " " + std::to_string(side.n[k]);
        out += "\n";
    }
    for (const auto &[cam, stats] : p.tiles) {
        bool any = false;
        for (const TileStat &t : stats) any = any || t.tries > 0 || t.finds > 0;
        if (!any || cam.empty() || cam.find_first_of(" \t\n#") != std::string::npos) continue;
        out += "tiles " + cam + " " + std::to_string(stats.size());
        for (const TileStat &t : stats) {
            std::snprintf(buf, sizeof buf, " %.6g %.6g", t.tries, t.finds);
            out += buf;
        }
        out += "\n";
    }
    for (int s = 0; s < 2; ++s)
        for (const bool taps : {true, false}) {
            const double *bins = taps ? p.pinch[s].taps : p.pinch[s].idle;
            if (!(std::accumulate(bins, bins + kPinchBins, 0.0) > 0)) continue;
            out += std::string(taps ? "taps " : "idle ") + kSideName[s];
            for (int b = 0; b < kPinchBins; ++b) {
                std::snprintf(buf, sizeof buf, " %.6g", bins[b]);
                out += buf;
            }
            out += "\n";
        }
    return out;
}

int parse(const std::string &text, Profile &out) {
    out = Profile{};
    int kept = 0, format = 0;
    bool have_shape[2] = {false, false}, have_count[2] = {false, false};
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string key, which;
        if (!(words >> key) || key[0] == '#') continue;
        if (key == "format") {
            if (!(words >> format) || format != 1) return 0;   // a newer file: not ours to guess at
            continue;
        }
        if (format != 1 || !(words >> which)) continue;
        if (key == "tiles") {   // `which` is the camera
            size_t count = 0;
            if (!(words >> count) || count == 0 || count > 256) continue;
            std::vector<TileStat> stats(count);
            bool ok = true;
            for (TileStat &t : stats)
                ok = ok && bool(words >> t.tries >> t.finds) && std::isfinite(t.tries) && std::isfinite(t.finds) &&
                     t.finds >= 0 && t.tries >= t.finds;   // a find is a try that found a hand
            if (!ok) continue;
            out.tiles[which] = stats;
            ++kept;
            continue;
        }
        const int s = side_of(which);
        if (s < 0) continue;
        if (key == "scale") {
            double v;
            long n;
            if (!(words >> v >> n) || !std::isfinite(v) || v < kScaleMin || v > kScaleMax || n <= 0) continue;
            out.side[s].scale.value = v, out.side[s].scale.n = std::min(n, kMaxSamples);
            ++kept;
        } else if (key == "shape") {
            double q[skel::kQuantities];
            bool ok = true;
            for (int k = 0; k < skel::kQuantities && ok; ++k) ok = bool(words >> q[k]) && plausible(k, q[k]);
            if (!ok) continue;
            std::copy(q, q + skel::kQuantities, out.side[s].q);
            have_shape[s] = true;
            ++kept;
        } else if (key == "taps" || key == "idle") {
            double bins[kPinchBins];
            bool ok = true;
            for (int b = 0; b < kPinchBins && ok; ++b) ok = bool(words >> bins[b]) && std::isfinite(bins[b]) && bins[b] >= 0;
            if (!ok) continue;
            std::copy(bins, bins + kPinchBins, key == "taps" ? out.side_pinch(s).taps : out.side_pinch(s).idle);
            ++kept;
        } else if (key == "count") {
            long n[skel::kQuantities];
            bool ok = true;
            for (int k = 0; k < skel::kQuantities && ok; ++k) ok = bool(words >> n[k]) && n[k] >= 0;
            if (!ok) continue;
            for (int k = 0; k < skel::kQuantities; ++k) out.side[s].n[k] = int(std::min(n[k], kMaxSamples));
            have_count[s] = true;
            ++kept;
        }
    }
    // a shape and its counts only mean something together
    for (int s = 0; s < 2; ++s)
        if (have_shape[s] != have_count[s]) {
            std::fill(out.side[s].q, out.side[s].q + skel::kQuantities, 0.0);
            std::fill(out.side[s].n, out.side[s].n + skel::kQuantities, 0);
            --kept;
        }
    return kept;
}

bool load(const std::string &path, Profile &out, std::string &err) {
    out = Profile{};
    std::ifstream f(path);
    if (!f) {
        if (errno == ENOENT) return true;
        err = path + ": " + std::strerror(errno);
        return false;
    }
    std::stringstream all;
    all << f.rdbuf();
    if (parse(all.str(), out) == 0 && !all.str().empty()) {
        err = path + ": nothing in it this ft-hands can use";
        return false;
    }
    return true;
}

bool save(const std::string &path, const Profile &p, std::string &err) {
    const std::string tmp = path + ".tmp", body = text(p);
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        err = tmp + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < body.size()) {
        const ssize_t w = write(fd, body.data() + done, body.size() - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += size_t(w);
    }
    const bool ok = done == body.size() && fsync(fd) == 0;
    if (close(fd) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = path + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace profile
