// Flagging tracking failures: see flagbuf.h.
#include "flagbuf.h"

#include "pinch.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace {

void appendf(std::string &s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void appendf(std::string &s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, size_t(std::min(n, int(sizeof buf) - 1)));
}

void points_json(std::string &s, const V3 *p, int n) {
    s += '[';
    for (int i = 0; i < n; ++i) appendf(s, "%s[%.4f, %.4f, %.4f]", i ? ", " : "", p[i][0], p[i][1], p[i][2]);
    s += ']';
}

double wall_now() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

std::string json_string(const std::string &s, bool null_if_empty) {
    if (null_if_empty && s.empty()) return "null";
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += char(c);
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) appendf(o, "\\u%04x", c);
        else o += char(c);
    }
    return o + "\"";
}

CropGeom crop_square(const Image &img, V2 center, double side, int max_px, uint8_t *out) {
    CropGeom g;
    side = std::max(side, 1.0);
    const int n = std::clamp(int(std::ceil(side - 1e-6)), 1, std::max(1, max_px));
    g.w = g.h = n;
    g.scale = side / n;
    g.x0 = center[0] - side / 2 + 0.5;
    g.y0 = center[1] - side / 2 + 0.5;
    // the taps: per output column and row, the image column and row (or -1 outside)
    const bool area = g.scale > 1.25;
    const int taps = area ? 2 : 1;
    const double off[2] = {area ? -g.scale / 4 : 0, area ? g.scale / 4 : 0};
    thread_local std::vector<int> xs, ys;
    xs.resize(size_t(n) * taps), ys.resize(size_t(n) * taps);
    for (int u = 0; u < n; ++u)
        for (int t = 0; t < taps; ++t) {
            const double c = (u + 0.5) * g.scale - 0.5 + off[t];
            const long x = std::lround(g.x0 + c), y = std::lround(g.y0 + c);
            xs[size_t(u) * taps + t] = x >= 0 && x < img.width ? int(x) : -1;
            ys[size_t(u) * taps + t] = y >= 0 && y < img.height ? int(y) : -1;
        }
    for (int v = 0; v < n; ++v) {
        uint8_t *o = out + size_t(v) * n;
        if (!area) {
            const int y = ys[size_t(v)];
            if (y < 0) {
                std::fill(o, o + n, 0);
                continue;
            }
            const uint8_t *row = img.data + size_t(y) * img.stride;
            for (int u = 0; u < n; ++u) o[u] = xs[size_t(u)] < 0 ? 0 : row[xs[size_t(u)]];
            continue;
        }
        const int y0 = ys[size_t(v) * 2], y1 = ys[size_t(v) * 2 + 1];
        const uint8_t *r0 = y0 < 0 ? nullptr : img.data + size_t(y0) * img.stride;
        const uint8_t *r1 = y1 < 0 ? nullptr : img.data + size_t(y1) * img.stride;
        for (int u = 0; u < n; ++u) {
            const int x0 = xs[size_t(u) * 2], x1 = xs[size_t(u) * 2 + 1];
            unsigned sum = 0;
            if (r0) sum += (x0 < 0 ? 0 : r0[x0]) + (x1 < 0 ? 0 : r0[x1]);
            if (r1) sum += (x0 < 0 ? 0 : r1[x0]) + (x1 < 0 ? 0 : r1[x1]);
            o[u] = uint8_t((sum + 2) / 4);
        }
    }
    return g;
}

FlagBuffer::FlagBuffer(const FlagConfig &cfg) : cfg_(cfg) {
    cfg_.crop_px = std::clamp(cfg_.crop_px, 16, 1024);
    slot_bytes_ = size_t(cfg_.crop_px) * cfg_.crop_px;
    nslots_ = int(std::max<size_t>(1, cfg_.max_bytes / slot_bytes_));
    stats_.slots = size_t(nslots_);
}

FlagBuffer::~FlagBuffer() { wait(); }

void FlagBuffer::wait() {
    if (writer_.joinable()) writer_.join();
}

int FlagBuffer::take_slot() {
    std::lock_guard<std::mutex> l(mu_);
    if (!free_.empty()) {
        const int s = free_.back();
        free_.pop_back();
        return s;
    }
    return -1;
}

void FlagBuffer::give_back(const std::vector<Crop> &crops) {
    std::lock_guard<std::mutex> l(mu_);
    for (const Crop &c : crops)
        if (c.slot >= 0) free_.push_back(c.slot);
}

void FlagBuffer::evict_front() {
    give_back(sets_.front().crops);
    sets_.pop_front();
}

void FlagBuffer::add(int64_t t_raw, int64_t t_mono, int rec_set, const std::map<std::string, Image> &images,
                     const std::vector<Seen> &views, const std::string &hands_json) {
    if (!arena_) {
        arena_.reset(new uint8_t[size_t(nslots_) * slot_bytes_]);   // not zeroed: untouched pages stay unmapped
        std::lock_guard<std::mutex> l(mu_);
        free_.clear();
        for (int s = nslots_ - 1; s >= 0; --s) free_.push_back(s);
    }
    const int64_t keep_ns = int64_t(cfg_.before_s * 1e9);
    while (!sets_.empty() && !sets_.front().held && sets_.front().t_raw < t_raw - keep_ns) evict_front();

    Set set;
    set.t_raw = t_raw, set.t_mono = t_mono, set.rec_set = rec_set, set.out = hands_json;
    set.held = pending_ && t_raw >= flag_t_ - keep_ns;
    auto crop = [&](const std::string &cam, int hand, const Roi &roi, bool ghost) -> int {
        const auto im = images.find(cam);
        if (im == images.end() || !im->second.data) return -1;
        int slot = take_slot();
        while (slot < 0 && !sets_.empty() && !sets_.front().held) evict_front(), slot = take_slot();
        if (slot < 0) return ++stats_.dropped, -1;
        const auto t0 = std::chrono::steady_clock::now();
        Crop c;
        c.cam = cam, c.hand = hand, c.ghost = ghost, c.slot = slot;
        c.g = crop_square(im->second, roi.center, cfg_.scale * roi.size, cfg_.crop_px, slot_ptr(slot));
        stats_.crop_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ++stats_.crop_calls, ++stats_.crops;
        stats_.ghosts += ghost;
        set.crops.push_back(std::move(c));
        return int(set.crops.size()) - 1;
    };
    for (const Seen &s : views) {
        View v;
        v.cam = s.cam, v.hand = s.hand;
        v.roi[0] = s.roi.center[0], v.roi[1] = s.roi.center[1], v.roi[2] = s.roi.size, v.roi[3] = s.roi.rotation;
        v.presence = s.lm.presence, v.right = s.lm.right;
        for (int k = 0; k < 21; ++k) v.lm[2 * k] = float(s.lm.pts[k][0]), v.lm[2 * k + 1] = float(s.lm.pts[k][1]);
        v.crop = crop(s.cam, s.hand, s.roi, false);
        set.views.push_back(v);
        last_[{s.cam, s.hand}] = {s.roi, t_raw};
    }
    // ghosts: views lost within ghost_s, cropped where they last were
    const int64_t ghost_ns = int64_t(cfg_.ghost_s * 1e9);
    int ghosts = 0;
    for (auto it = last_.begin(); it != last_.end();) {
        if (t_raw - it->second.t > ghost_ns) {
            it = last_.erase(it);
            continue;
        }
        if (it->second.t != t_raw && it->first.second != 0 && ghosts < cfg_.max_ghosts &&
            crop(it->first.first, it->first.second, it->second.roi, true) >= 0)
            ++ghosts;
        ++it;
    }
    sets_.push_back(std::move(set));
    ++stats_.sets;
    if (pending_ && t_raw >= flag_t_ + int64_t(cfg_.after_s * 1e9)) complete();
}

bool FlagBuffer::flag(const FlagRequest &req, int64_t t_raw, const std::string &extra_note) {
    if (pending_) {
        if (!extra_note.empty()) extra_notes_ += (extra_notes_.empty() ? "" : ", ") + json_string(extra_note);
        return false;
    }
    if (req.dir.empty()) return false;
    pending_ = true, req_ = req, flag_t_ = t_raw, extra_notes_.clear();
    dropped_at_flag_ = stats_.dropped;
    const int64_t keep_ns = int64_t(cfg_.before_s * 1e9);
    for (Set &s : sets_)
        if (s.t_raw >= flag_t_ - keep_ns) s.held = true;
    return true;
}

void FlagBuffer::tick(int64_t t_raw_now) {
    if (pending_ && t_raw_now >= flag_t_ + int64_t((cfg_.after_s + 0.5) * 1e9)) complete();
}

void FlagBuffer::finish() {
    if (pending_) complete();
}

void FlagBuffer::complete() {
    Dump d;
    d.req = req_, d.flag_t = flag_t_, d.first_unix = wall_now();
    d.dropped = stats_.dropped - dropped_at_flag_;
    if (!extra_notes_.empty()) d.req.fields += std::string(d.req.fields.empty() ? "" : ", ") + "\"more_notes\": [" + extra_notes_ + "]";
    auto first = std::find_if(sets_.begin(), sets_.end(), [](const Set &s) { return s.held; });
    for (auto it = first; it != sets_.end(); ++it) d.sets.push_back(std::move(*it));
    sets_.erase(first, sets_.end());
    pending_ = false;
    wait();   // one writer at a time (a flag takes 14 s; writing one well under that)
    writer_ = std::thread(&FlagBuffer::write, this, std::move(d));
}

void FlagBuffer::write(Dump d) {
    // off the tracking cores, at the lowest priority: it's only files
    if (!cfg_.writer_cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : cfg_.writer_cpus) CPU_SET(c, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 19);
    std::string err;
    const std::string &dir = d.req.dir;
    mkdir(dir.c_str(), 0700);
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) err = "can't use " + dir;
    FILE *bin = err.empty() ? std::fopen((dir + "/crops.bin").c_str(), "wb") : nullptr;
    FILE *cj = err.empty() ? std::fopen((dir + "/crops.jsonl").c_str(), "w") : nullptr;
    FILE *sj = err.empty() ? std::fopen((dir + "/sets.jsonl").c_str(), "w") : nullptr;
    if (err.empty() && (!bin || !cj || !sj)) err = "can't write in " + dir;
    size_t k = 0, offset = 0, ghosts = 0;
    std::string line;
    for (size_t i = 0; i < d.sets.size(); ++i) {
        Set &s = d.sets[i];
        if (err.empty()) {
            std::vector<size_t> kk(s.crops.size());
            for (size_t j = 0; j < s.crops.size(); ++j) {
                const Crop &c = s.crops[j];
                const size_t bytes = size_t(c.g.w) * c.g.h;
                if (std::fwrite(slot_ptr(c.slot), 1, bytes, bin) != bytes) err = "write failed in " + dir;
                line.clear();
                appendf(line, "{\"k\": %zu, \"set\": %zu, \"cam\": %s, \"hand\": %d, \"ghost\": %s, \"x0\": %.3f, \"y0\": %.3f, "
                              "\"scale\": %.5f, \"w\": %d, \"h\": %d, \"offset\": %zu, \"bytes\": %zu}\n",
                        k, i, json_string(c.cam).c_str(), c.hand, c.ghost ? "true" : "false", c.g.x0, c.g.y0, c.g.scale, c.g.w,
                        c.g.h, offset, bytes);
                std::fputs(line.c_str(), cj);
                kk[j] = k++, offset += bytes, ghosts += c.ghost;
            }
            line.clear();
            appendf(line, "{\"i\": %zu, \"t_raw_ns\": %lld, \"t_ns\": %lld", i, (long long)s.t_raw, (long long)s.t_mono);
            if (s.rec_set >= 0) appendf(line, ", \"rec_set\": %d", s.rec_set);
            if (!s.out.empty()) line += ", " + s.out;
            line += ", \"views\": [";
            for (size_t j = 0; j < s.views.size(); ++j) {
                const View &v = s.views[j];
                appendf(line, "%s{\"cam\": %s, \"hand\": %d, \"roi\": [%.2f, %.2f, %.2f, %.4f], \"presence\": %.3f, \"right\": %.3f, \"lm\": [",
                        j ? ", " : "", json_string(v.cam).c_str(), v.hand, v.roi[0], v.roi[1], v.roi[2], v.roi[3], v.presence, v.right);
                for (int p = 0; p < 21; ++p) appendf(line, "%s[%.2f, %.2f]", p ? ", " : "", v.lm[2 * p], v.lm[2 * p + 1]);
                appendf(line, "], \"crop\": %lld}", v.crop >= 0 ? (long long)kk[size_t(v.crop)] : -1LL);
            }
            line += "], \"ghosts\": [";
            bool first = true;
            for (size_t j = 0; j < s.crops.size(); ++j)
                if (s.crops[j].ghost) {
                    appendf(line, "%s{\"cam\": %s, \"hand\": %d, \"crop\": %zu}", first ? "" : ", ", json_string(s.crops[j].cam).c_str(),
                            s.crops[j].hand, kk[j]);
                    first = false;
                }
            line += "]}\n";
            std::fputs(line.c_str(), sj);
        }
        give_back(s.crops);   // as it goes: the buffer gets its slots back
        s.crops.clear();
    }
    for (FILE *f : {bin, cj, sj})
        if (f && std::fclose(f) != 0 && err.empty()) err = "write failed in " + dir;
    if (err.empty()) {
        std::string m = "{\"version\": 1";
        if (!d.req.fields.empty()) m += ", " + d.req.fields;
        appendf(m, ", \"flag_t_raw_ns\": %lld, \"window\": {\"before_s\": %.2f, \"after_s\": %.2f, \"first_t_raw_ns\": %lld, "
                   "\"last_t_raw_ns\": %lld}",
                (long long)d.flag_t, cfg_.before_s, cfg_.after_s, (long long)(d.sets.empty() ? 0 : d.sets.front().t_raw),
                (long long)(d.sets.empty() ? 0 : d.sets.back().t_raw));
        appendf(m, ", \"crop\": {\"scale\": %.3f, \"px\": %d, \"ghost_s\": %.2f}", cfg_.scale, cfg_.crop_px, cfg_.ghost_s);
        m += ", \"settings\": " + settings_json_ + ", \"cameras\": " + cameras_json_;
        appendf(m, ", \"counts\": {\"sets\": %zu, \"crops\": %zu, \"ghosts\": %zu, \"crops_dropped\": %zu, \"bytes\": %zu}", d.sets.size(), k,
                ghosts, d.dropped, offset);
        appendf(m, ", \"written_unix\": %.3f}\n", wall_now());
        const std::string tmp = dir + "/manifest.json.tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        const bool ok = f && std::fputs(m.c_str(), f) >= 0;
        if (!f || std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), (dir + "/manifest.json").c_str()) != 0)
            err = "can't write " + dir + "/manifest.json";
    }
    std::lock_guard<std::mutex> l(mu_);
    if (err.empty()) ++stats_.flags, stats_.last_dir = dir, stats_.last_error.clear();
    else stats_.last_error = err;
}

FlagBuffer::Stats FlagBuffer::stats() const {
    std::lock_guard<std::mutex> l(mu_);
    Stats s = stats_;
    s.slots_used = arena_ ? size_t(nslots_) - free_.size() : 0;
    return s;
}

size_t FlagBuffer::bytes() const {
    size_t b = stats().slots_used * slot_bytes_;
    for (const Set &s : sets_) b += sizeof(Set) + s.out.capacity() + s.views.capacity() * sizeof(View) + s.crops.capacity() * sizeof(Crop);
    return b;
}

std::string cameras_json(const std::map<std::string, Camera> &cams) {
    std::string s = "{";
    for (auto &[name, c] : cams) {
        appendf(s, "%s%s: {\"width\": %d, \"height\": %d, \"fx\": %.6f, \"fy\": %.6f, \"cx\": %.6f, \"cy\": %.6f, ", s.size() > 1 ? ", " : "",
                json_string(name).c_str(), c.width, c.height, c.fx, c.fy, c.cx, c.cy);
        appendf(s, "\"k\": [%.9g, %.9g, %.9g, %.9g], \"R\": [", c.k[0], c.k[1], c.k[2], c.k[3]);
        for (int r = 0; r < 3; ++r) appendf(s, "%s[%.9f, %.9f, %.9f]", r ? ", " : "", c.R[r][0], c.R[r][1], c.R[r][2]);
        appendf(s, "], \"origin\": [%.6f, %.6f, %.6f]}", c.origin[0], c.origin[1], c.origin[2]);
    }
    return s + "}";
}

std::string output_json(const std::vector<const Hand *> &hands, const Pinch *pinch, const Pinch *middle, const Grip *grip) {
    std::string s = "\"hands\": [";
    for (size_t i = 0; i < hands.size(); ++i) {
        const Hand &h = *hands[i];
        appendf(s, "%s{\"id\": %d, \"side\": \"%s\", \"right_score\": %.3f, \"nviews\": %d, \"residual\": %.4f, \"scale\": %.3f, "
                   "\"speed\": %.3f, \"fit\": %s, \"pts\": ",
                i ? ", " : "", h.id, h.right() ? "R" : "L", h.right_score, h.nviews, h.residual, h.scale, h.speed,
                h.fit.valid ? "true" : "false");
        points_json(s, h.pts, 21);
        s += ", \"pub\": ";
        points_json(s, h.fit.valid ? h.fit.pts : h.smooth, 21);
        s += '}';
    }
    s += ']';
    if (!pinch && !middle && !grip) return s;
    s += ", \"gestures\": {";
    bool first = true;
    auto sides = [&](const char *name, const fh_pinch_t &l, const fh_pinch_t &r) {
        appendf(s, "%s\"%s\": [{\"flags\": %u, \"d\": %.4f}, {\"flags\": %u, \"d\": %.4f}]", first ? "" : ", ", name, l.flags, l.distance,
                r.flags, r.distance);
        first = false;
    };
    if (pinch) sides("pinch", pinch->side(0), pinch->side(1));
    if (middle) sides("middle", middle->side(0), middle->side(1));
    if (grip) sides("grip", grip->side(0), grip->side(1));
    return s + "}";
}
