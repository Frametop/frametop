// Unit tests for the flag buffer (track/flagbuf.h): the crops' geometry, the rolling window, the
// slots running out, a flag's window and files, ghosts, a second flag while one waits, and the
// clock completing a flag when no sets come. Made-up images and views; no models.
// Build and run: make check (in the dev container; hands/build.sh's environment).
#include "../track/flagbuf.h"

#include <json/json.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

namespace {

constexpr int64_t kSec = 1'000'000'000;

struct Img {
    int w, h;
    std::vector<uint8_t> px;
    Img(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) px[size_t(y) * w + x] = uint8_t((x + 2 * y) & 255);
    }
    Image image() const { return {px.data(), w, h, w}; }
    int at(int x, int y) const { return px[size_t(y) * w + x]; }
};

Seen view(const std::string &cam, int hand, double cx, double cy, double size) {
    Seen s;
    s.cam = cam, s.hand = hand;
    s.roi.center = {cx, cy}, s.roi.size = size, s.roi.rotation = 0.3;
    s.lm.presence = 0.9, s.lm.right = 0.8;
    for (int k = 0; k < 21; ++k) s.lm.pts[k] = {cx + k, cy - k};
    return s;
}

bool parse(const std::string &text, Json::Value &v) {
    Json::CharReaderBuilder b;
    std::string e;
    std::istringstream in(text);
    return Json::parseFromStream(b, in, &v, &e);
}

std::vector<Json::Value> lines(const std::string &path) {
    std::vector<Json::Value> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        Json::Value v;
        CHECK(parse(line, v), "%s: a line isn't JSON: %.80s", path.c_str(), line.c_str());
        out.push_back(v);
    }
    return out;
}

Json::Value file_json(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    Json::Value v;
    CHECK(parse(ss.str(), v), "%s isn't JSON", path.c_str());
    return v;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/flagbuf-test-XXXXXX";
    const char *d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

void test_crop_geometry() {
    const Img img(320, 240);
    std::vector<uint8_t> out(224 * 224);
    // native size: crop pixel (u, v) is image pixel (x0 + u, y0 + v) exactly
    CropGeom g = crop_square(img.image(), {100.5, 80.5}, 60, 224, out.data());
    CHECK(g.w == 60 && g.h == 60 && std::fabs(g.scale - 1) < 1e-9, "native crop %dx%d scale %f", g.w, g.h, g.scale);
    bool same = true;
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u) {
            const int x = int(std::lround(g.x0 + (u + 0.5) * g.scale - 0.5)), y = int(std::lround(g.y0 + (v + 0.5) * g.scale - 0.5));
            same = same && out[size_t(v) * g.w + u] == img.at(x, y);
        }
    CHECK(same, "native crop's pixels aren't the image's at the mapped place");
    // the crop's middle maps to the centre
    CHECK(std::fabs(g.x0 + (g.w / 2.0) * g.scale - 0.5 - 100.5) < 1e-9, "centre x %f", g.x0 + (g.w / 2.0) * g.scale - 0.5);
    // downscaled: at most max_px, the mapping still holds (the pattern is smooth: within a level or two)
    g = crop_square(img.image(), {160, 120}, 200, 64, out.data());
    CHECK(g.w == 64 && std::fabs(g.scale - 200.0 / 64) < 1e-9, "downscaled %d scale %f", g.w, g.scale);
    int worst = 0;
    for (int v = 0; v < g.h; ++v)
        for (int u = 0; u < g.w; ++u) {
            const double x = g.x0 + (u + 0.5) * g.scale - 0.5, y = g.y0 + (v + 0.5) * g.scale - 0.5;
            const int want = int(std::lround(x + 2 * y)) & 255, got = out[size_t(v) * g.w + u];
            if (want > 8 && want < 248) worst = std::max(worst, std::abs(want - got));
        }
    CHECK(worst <= 3, "downscaled crop off the mapping by %d levels", worst);
    // past the image's corner: 0 there, the image inside
    g = crop_square(img.image(), {2, 2}, 40, 224, out.data());
    CHECK(out[0] == 0 && out[size_t(g.h - 1) * g.w + g.w - 1] == img.at(22, 22), "corner crop: %d, %d (want 0, %d)", out[0],
          out[size_t(g.h - 1) * g.w + g.w - 1], img.at(22, 22));
    CHECK(g.x0 < 0 && g.y0 < 0, "corner crop's x0 %f y0 %f", g.x0, g.y0);
}

FlagConfig small_config() {
    FlagConfig c;
    c.before_s = 1.0, c.after_s = 0.5, c.ghost_s = 0.25, c.crop_px = 32, c.scale = 1.6;
    c.max_bytes = 32 * 32 * 20;   // 20 slots
    c.writer_cpus.clear();
    return c;
}

void test_window_and_slots() {
    const Img img(128, 128);
    const std::map<std::string, Image> images = {{"a", img.image()}, {"b", img.image()}};
    FlagBuffer fb(small_config());
    // 10 sets a second, two views each (20 slots: 10 sets fit)
    for (int i = 0; i < 30; ++i)
        fb.add(i * kSec / 10, i * kSec / 10, i, images, {view("a", 1, 60, 60, 20), view("b", 1, 64, 50, 20)}, "\"hands\": []");
    FlagBuffer::Stats s = fb.stats();
    CHECK(s.slots == 20 && s.slots_used <= 20, "slots %zu used %zu", s.slots, s.slots_used);
    CHECK(s.dropped == 0, "nothing held, so nothing dropped: %zu", s.dropped);
    CHECK(s.crops == 60, "crops %zu", s.crops);
    // one view a set: the window (1 s), not the slots, keeps it to about 11 sets
    for (int i = 30; i < 60; ++i)
        fb.add(i * kSec / 10, i * kSec / 10, i, images, {view("a", 1, 60, 60, 20)}, "\"hands\": []");
    s = fb.stats();
    CHECK(s.slots_used >= 10 && s.slots_used <= 12, "one view a set: %zu slots in use", s.slots_used);
}

void test_flag_files() {
    const Img img(128, 128);
    const std::map<std::string, Image> images = {{"a", img.image()}, {"b", img.image()}};
    FlagBuffer fb(small_config());
    fb.set_static("{\"a\": {\"width\": 128}}", "{\"models\": \"m\"}");
    const std::string root = temp_dir(), dir = root + "/flag-1";
    int i = 0;
    for (; i < 30; ++i) fb.add(i * kSec / 10, i * kSec / 10, i, images, {view("a", 1, 60, 60, 20)}, "\"hands\": []");
    CHECK(fb.flag({dir, "\"source\": \"test\", \"note\": \"fingers \\\"crossed\\\"\""}, 29 * kSec / 10), "flag refused");
    CHECK(!fb.flag({dir + "-2", ""}, 30 * kSec / 10, "again"), "a second flag while one waits was taken");
    CHECK(fb.waiting(), "not waiting");
    // two views a set now: the held sets fill the slots, so some crops drop
    for (; i < 34; ++i)
        fb.add(i * kSec / 10, i * kSec / 10, i, images, {view("a", 1, 60, 60, 20), view("b", 2, 30, 30, 20)}, "\"hands\": []");
    CHECK(fb.waiting(), "completed early (0.5 s after 2.9 s is 3.4 s; the last set is 3.3 s)");
    fb.add(i * kSec / 10, i * kSec / 10, i, images, {}, "\"hands\": []");
    ++i;
    CHECK(!fb.waiting(), "not completed");
    fb.wait();
    const FlagBuffer::Stats s = fb.stats();
    CHECK(s.flags == 1 && s.last_dir == dir && s.last_error.empty(), "flags %zu dir %s error %s", s.flags, s.last_dir.c_str(),
          s.last_error.c_str());
    CHECK(s.slots_used == 0, "the written sets' slots weren't given back: %zu in use", s.slots_used);
    const Json::Value m = file_json(dir + "/manifest.json");
    CHECK(m["version"].asInt() == 1 && m["source"].asString() == "test", "manifest version/source");
    CHECK(m["note"].asString() == "fingers \"crossed\"", "note %s", m["note"].asString().c_str());
    CHECK(m["more_notes"].size() == 1 && m["more_notes"][0].asString() == "again", "more_notes");
    CHECK(m["flag_t_raw_ns"].asInt64() == 29 * kSec / 10, "flag time");
    CHECK(m["settings"]["models"].asString() == "m" && m["cameras"]["a"]["width"].asInt() == 128, "static parts");
    const std::vector<Json::Value> sets = lines(dir + "/sets.jsonl"), crops = lines(dir + "/crops.jsonl");
    // held: from 1.9 s (1 s before) to the set at 3.4 s
    CHECK(!sets.empty() && sets.front()["t_raw_ns"].asInt64() >= 19 * kSec / 10 - 1 &&
              sets.back()["t_raw_ns"].asInt64() == 34 * kSec / 10,
          "window %lld..%lld", sets.empty() ? 0LL : (long long)sets.front()["t_raw_ns"].asInt64(),
          sets.empty() ? 0LL : (long long)sets.back()["t_raw_ns"].asInt64());
    CHECK(m["counts"]["sets"].asUInt() == sets.size() && m["counts"]["crops"].asUInt() == crops.size(), "counts");
    CHECK(m["counts"]["crops_dropped"].asUInt() > 0, "held sets filling the slots should drop crops");
    // every view's crop index points at a crop line of that set and camera; the bytes add up
    size_t bytes = 0;
    for (const Json::Value &c : crops) {
        CHECK(c["offset"].asUInt64() == bytes, "crop %u offset", c["k"].asUInt());
        bytes += c["bytes"].asUInt64();
        CHECK(c["bytes"].asUInt64() == c["w"].asUInt64() * c["h"].asUInt64(), "crop bytes");
    }
    struct stat st;
    CHECK(stat((dir + "/crops.bin").c_str(), &st) == 0 && size_t(st.st_size) == bytes, "crops.bin size");
    for (const Json::Value &set : sets)
        for (const Json::Value &v : set["views"]) {
            const int k = v["crop"].asInt();
            if (k < 0) continue;
            CHECK(k < int(crops.size()) && crops[size_t(k)]["set"].asInt() == set["i"].asInt() &&
                      crops[size_t(k)]["cam"].asString() == v["cam"].asString(),
                  "view crop %d", k);
            CHECK(v["lm"].size() == 21 && v["roi"].size() == 4, "view landmarks");
        }
    // the first crop's pixels are the image's where the mapping says
    if (!crops.empty()) {
        std::ifstream f(dir + "/crops.bin", std::ios::binary);
        std::vector<uint8_t> px(crops[0]["bytes"].asUInt64());
        f.read(reinterpret_cast<char *>(px.data()), std::streamsize(px.size()));
        const double x0 = crops[0]["x0"].asDouble(), y0 = crops[0]["y0"].asDouble(), sc = crops[0]["scale"].asDouble();
        const int w = crops[0]["w"].asInt(), u = w / 2, v = w / 3;
        const int x = int(std::lround(x0 + (u + 0.5) * sc - 0.5)), y = int(std::lround(y0 + (v + 0.5) * sc - 0.5));
        CHECK(std::abs(int(px[size_t(v) * w + u]) - img.at(x, y)) <= 3, "crop pixel %d vs image %d", px[size_t(v) * w + u], img.at(x, y));
    }
    std::system(("rm -rf " + root).c_str());
}

void test_ghosts() {
    const Img img(128, 128);
    const std::map<std::string, Image> images = {{"a", img.image()}};
    FlagConfig c = small_config();
    c.after_s = 0.05;
    FlagBuffer fb(c);
    const std::string root = temp_dir(), dir = root + "/flag-g";
    fb.add(0, 0, 0, images, {view("a", 7, 60, 60, 20)}, "");
    for (int i = 1; i <= 5; ++i) fb.add(i * kSec / 10, i * kSec / 10, i, images, {}, "");
    CHECK(fb.stats().ghosts == 2, "ghosts within 0.25 s: %zu (want 2: at 0.1 and 0.2 s)", fb.stats().ghosts);
    fb.flag({dir, ""}, 5 * kSec / 10);
    fb.add(6 * kSec / 10, 6 * kSec / 10, 6, images, {}, "");
    fb.wait();
    const std::vector<Json::Value> sets = lines(dir + "/sets.jsonl"), crops = lines(dir + "/crops.jsonl");
    int ghost_lines = 0;
    for (const Json::Value &s : sets) ghost_lines += int(s["ghosts"].size());
    CHECK(ghost_lines == 2 && crops.size() == 3, "ghost lines %d crops %zu", ghost_lines, crops.size());
    if (crops.size() == 3) CHECK(!crops[0]["ghost"].asBool() && crops[1]["ghost"].asBool() && crops[1]["hand"].asInt() == 7, "ghost crop");
    std::system(("rm -rf " + root).c_str());
}

void test_tick() {
    const Img img(64, 64);
    FlagBuffer fb(small_config());
    const std::string root = temp_dir(), dir = root + "/flag-t";
    fb.add(0, 0, 0, {{"a", img.image()}}, {view("a", 1, 30, 30, 10)}, "");
    fb.flag({dir, ""}, 0);
    fb.tick(kSec / 2);   // 0.5 s: not yet (after_s + 0.5)
    CHECK(fb.waiting(), "ticked too early");
    fb.tick(kSec + 1);
    CHECK(!fb.waiting(), "tick didn't complete it");
    fb.wait();
    CHECK(file_json(dir + "/manifest.json")["counts"]["sets"].asInt() == 1, "tick flag sets");
    // a folder it can't make: reported, the slots still come back
    FlagBuffer fb2(small_config());
    fb2.add(0, 0, 0, {{"a", img.image()}}, {view("a", 1, 30, 30, 10)}, "");
    fb2.flag({root + "/missing/deeper", ""}, 0);
    fb2.finish();
    fb2.wait();
    CHECK(!fb2.stats().last_error.empty() && fb2.stats().slots_used == 0, "bad folder: error '%s', %zu slots",
          fb2.stats().last_error.c_str(), fb2.stats().slots_used);
    std::system(("rm -rf " + root).c_str());
}

// Not a check: what a crop costs here, a hand at arm's length in a 1056 x 1024 camera (a 375 px crop,
// so a 600 px square) down to 224 and 288, and a small one copied as it is.
void bench() {
    const Img img(1056, 1024);
    std::vector<uint8_t> out(512 * 512);
    for (int px : {224, 288}) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 300; ++i) crop_square(img.image(), {500.0 + i % 7, 480}, 600, px, out.data());
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 300;
        std::printf("flagbuf_test: a 600 px square to %d px: %.0f us a crop\n", px, us);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 300; ++i) crop_square(img.image(), {500.0 + i % 7, 480}, 200, 224, out.data());
    std::printf("flagbuf_test: a 200 px square as it is: %.0f us a crop\n",
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 300);
}

void test_json_string() {
    CHECK(json_string("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"", "%s", json_string("a\"b\\c\nd").c_str());
    CHECK(json_string("", true) == "null" && json_string("") == "\"\"", "empty");
}

}  // namespace

int main() {
    test_crop_geometry();
    test_window_and_slots();
    test_flag_files();
    test_ghosts();
    test_tick();
    test_json_string();
    bench();
    std::printf("flagbuf_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
