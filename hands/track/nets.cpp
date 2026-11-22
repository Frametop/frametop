#include "nets.h"

#include <mat.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>

namespace {

constexpr int kPalmSize = 192, kHandSize = Nets::kHandCrop;
const int kRoiLandmarks[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};

// 2x3 affine taking crop pixels (0..out) to image pixels.
void crop_matrix(V2 center, double size, double rotation, int out, float tm[6]) {
    const double c = std::cos(rotation), s = std::sin(rotation), k = size / out;
    tm[0] = float(c * k), tm[1] = float(-s * k), tm[3] = float(s * k), tm[4] = float(c * k);
    tm[2] = float(center[0] - (tm[0] + tm[1]) * out / 2.0);
    tm[5] = float(center[1] - (tm[3] + tm[4]) * out / 2.0);
}

V2 to_image(const float tm[6], double x, double y) {
    return {tm[0] * x + tm[1] * y + tm[2], tm[3] * x + tm[4] * y + tm[5]};
}

// OpenCV's CLAHE (4x4 tiles) on a square crop, in place.
void clahe(uint8_t *img, int n, double clip_limit) {
    constexpr int kTiles = 4;
    const int ts = n / kTiles, area = ts * ts;
    const int clip = std::max(1, int(clip_limit * area / 256));
    uint8_t lut[kTiles][kTiles][256];
    for (int ty = 0; ty < kTiles; ++ty)
        for (int tx = 0; tx < kTiles; ++tx) {
            int hist[256] = {};
            for (int y = ty * ts; y < (ty + 1) * ts; ++y)
                for (int x = tx * ts; x < (tx + 1) * ts; ++x) ++hist[img[y * n + x]];
            int excess = 0;
            for (int &h : hist)
                if (h > clip) excess += h - clip, h = clip;
            const int add = excess / 256, residual = excess - add * 256;
            for (int i = 0; i < 256; ++i) hist[i] += add + (i < residual ? 1 : 0);
            int sum = 0;
            const float scale = 255.f / area;
            for (int i = 0; i < 256; ++i) {
                sum += hist[i];
                lut[ty][tx][i] = uint8_t(std::min(255, int(sum * scale + 0.5f)));
            }
        }
    std::vector<uint8_t> out(size_t(n) * n);
    for (int y = 0; y < n; ++y) {
        const float fy = (y + 0.5f) / ts - 0.5f;
        const int y0 = std::clamp(int(std::floor(fy)), 0, kTiles - 1), y1 = std::min(y0 + 1, kTiles - 1);
        const float wy = std::clamp(fy - y0, 0.f, 1.f);
        for (int x = 0; x < n; ++x) {
            const float fx = (x + 0.5f) / ts - 0.5f;
            const int x0 = std::clamp(int(std::floor(fx)), 0, kTiles - 1), x1 = std::min(x0 + 1, kTiles - 1);
            const float wx = std::clamp(fx - x0, 0.f, 1.f);
            const uint8_t v = img[y * n + x];
            const float top = lut[y0][x0][v] * (1 - wx) + lut[y0][x1][v] * wx;
            const float bot = lut[y1][x0][v] * (1 - wx) + lut[y1][x1][v] * wx;
            out[size_t(y) * n + x] = uint8_t(top * (1 - wy) + bot * wy + 0.5f);
        }
    }
    std::copy(out.begin(), out.end(), img);
}

// Linear stretch of the 1st..99th percentile to 0..255, in place.
void stretch(uint8_t *img, int n) {
    int hist[256] = {};
    const int total = n * n;
    for (int i = 0; i < total; ++i) ++hist[img[i]];
    int lo = 0, hi = 255, acc = 0;
    for (int v = 0; v < 256; ++v)
        if ((acc += hist[v]) > total / 100) { lo = v; break; }
    acc = 0;
    for (int v = 255; v >= 0; --v)
        if ((acc += hist[v]) > total / 100) { hi = v; break; }
    if (hi <= lo) return;
    for (int i = 0; i < total; ++i) img[i] = uint8_t(std::clamp((img[i] - lo) * 255 / (hi - lo), 0, 255));
}

// A crop as gray bytes: warped, then equalized.
void gray_crop(const Image &img, const float tm[6], int n, const Contrast &contrast, uint8_t *patch) {
    ncnn::warpaffine_bilinear_c1(img.data, img.width, img.height, img.stride, patch, n, n, n, tm, 0, 0);
    if (contrast.mode == Contrast::Clahe) clahe(patch, n, contrast.clip);
    else if (contrast.mode == Contrast::Stretch) stretch(patch, n);
}

// A crop as the models' input: RGB (the mono plane three times), 0..1.
ncnn::Mat crop(const Image &img, const float tm[6], int n, const Contrast &contrast) {
    std::vector<uint8_t> patch(size_t(n) * n);
    gray_crop(img, tm, n, contrast, patch.data());
    ncnn::Mat m = ncnn::Mat::from_pixels(patch.data(), ncnn::Mat::PIXEL_GRAY2RGB, n, n);
    const float norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
    m.substract_mean_normalize(nullptr, norm);
    return m;
}

}  // namespace

bool Contrast::parse(const std::string &s, Contrast &out) {
    if (s == "none") return out.mode = None, true;
    if (s == "stretch") return out.mode = Stretch, true;
    if (s.rfind("clahe", 0) == 0) {
        out.mode = Clahe;
        out.clip = s.size() > 6 && s[5] == ':' ? std::atof(s.c_str() + 6) : 2.0;
        return out.clip > 0;
    }
    return false;
}

bool Contrast::parse_pair(const std::string &s, Contrast &palm, Contrast &hand) {
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return parse(s, palm) && parse(s, hand);
    return parse(s.substr(0, slash), palm) && parse(s.substr(slash + 1), hand);
}

namespace {

bool load_net(ncnn::Net &net, const std::string &base, std::string &err) {
    net.opt.num_threads = 1;
    net.opt.use_vulkan_compute = false;
    net.opt.use_fp16_packed = net.opt.use_fp16_storage = net.opt.use_fp16_arithmetic = true;
    if (net.load_param((base + ".param").c_str()) || net.load_model((base + ".bin").c_str())) {
        err = "can't load " + base + ".param/.bin";
        return false;
    }
    return true;
}

}  // namespace

Roi Palm::roi() const {
    const V2 a = kp[0], b = kp[2];
    const double rot = wrap_angle(M_PI / 2 - std::atan2(-(b[1] - a[1]), b[0] - a[0]));
    const double h = size[1];
    const V2 shift{-h * -0.5 * std::sin(rot), h * -0.5 * std::cos(rot)};
    return {center + shift, std::max(size[0], size[1]) * 2.6, rot};
}

Roi roi_from_points(const V2 *p) {
    const V2 w = p[0];
    V2 m = (p[5] + p[13]) * 0.5;
    m = (m + p[9]) * 0.5;
    const double rot = wrap_angle(M_PI / 2 - std::atan2(-(m[1] - w[1]), m[0] - w[0]));
    V2 lo{1e9, 1e9}, hi{-1e9, -1e9};
    for (int i : kRoiLandmarks)
        for (int k = 0; k < 2; ++k) lo[k] = std::min(lo[k], p[i][k]), hi[k] = std::max(hi[k], p[i][k]);
    V2 center = (lo + hi) * 0.5;
    const double c = std::cos(-rot), s = std::sin(-rot);
    V2 qlo{1e9, 1e9}, qhi{-1e9, -1e9};
    for (int i : kRoiLandmarks) {
        const V2 d = p[i] - center;
        const V2 q{d[0] * c - d[1] * s, d[0] * s + d[1] * c};
        for (int k = 0; k < 2; ++k) qlo[k] = std::min(qlo[k], q[k]), qhi[k] = std::max(qhi[k], q[k]);
    }
    const V2 mid = (qlo + qhi) * 0.5;
    const double c2 = std::cos(rot), s2 = std::sin(rot);
    center = center + V2{mid[0] * c2 - mid[1] * s2, mid[0] * s2 + mid[1] * c2};
    const double w2 = qhi[0] - qlo[0], h2 = qhi[1] - qlo[1];
    center = center + V2{-h2 * -0.1 * s2, h2 * -0.1 * c2};
    return {center, std::max(w2, h2) * 2.0, rot};
}

Roi Landmarks::next_roi() const { return roi_from_points(pts); }

bool Nets::load(const std::string &dir, bool int8, std::string &err) {
    const std::string suffix = int8 ? "-int8.ncnn" : ".ncnn";
    if (!load_net(palm_, dir + "/palm" + suffix, err) || !load_net(hand_, dir + "/hand" + suffix, err)) return false;
    // SSD anchors of palm_detection_full: strides 8 (2 per cell) and 16 (6 per cell)
    for (auto [stride, per] : {std::pair{8, 2}, std::pair{16, 6}}) {
        const int n = kPalmSize / stride;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                for (int k = 0; k < per; ++k) anchors_.push_back({(x + 0.5) / n * kPalmSize, (y + 0.5) / n * kPalmSize});
    }
    return true;
}

std::vector<Palm> Nets::palms(const Image &img, V2 center, double size, double rotation) const {
    float tm[6];
    crop_matrix(center, size, rotation, kPalmSize, tm);
    ncnn::Extractor ex = palm_.create_extractor();
    ex.input("in0", crop(img, tm, kPalmSize, palm_contrast_));
    ncnn::Mat boxes, scores;
    ex.extract("out0", boxes);
    ex.extract("out1", scores);
    const float *raw = boxes, *logit = scores;
    const int n = int(anchors_.size());
    const float min_logit = std::log(0.5f / 0.5f);  // score 0.5
    struct Cand { V2 c, s; V2 kp[7]; double score; };
    std::vector<Cand> cand;
    for (int i = 0; i < n; ++i) {
        if (logit[i] <= min_logit) continue;
        const float *r = raw + i * 18;
        Cand c;
        c.c = {r[0] + anchors_[i][0], r[1] + anchors_[i][1]};
        c.s = {r[2], r[3]};
        for (int k = 0; k < 7; ++k) c.kp[k] = {r[4 + 2 * k] + anchors_[i][0], r[5 + 2 * k] + anchors_[i][1]};
        c.score = 1 / (1 + std::exp(-std::clamp(double(logit[i]), -100.0, 100.0)));
        cand.push_back(c);
    }
    // MediaPipe's weighted NMS: overlapping boxes are averaged, weighted by score
    std::sort(cand.begin(), cand.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
    std::vector<bool> used(cand.size());
    std::vector<Palm> out;
    for (size_t i = 0; i < cand.size(); ++i) {
        if (used[i]) continue;
        double wsum = 0;
        Cand acc{};
        for (size_t j = i; j < cand.size(); ++j) {
            if (used[j]) continue;
            const double ix = std::max(0.0, std::min(cand[i].c[0] + cand[i].s[0] / 2, cand[j].c[0] + cand[j].s[0] / 2) -
                                                std::max(cand[i].c[0] - cand[i].s[0] / 2, cand[j].c[0] - cand[j].s[0] / 2));
            const double iy = std::max(0.0, std::min(cand[i].c[1] + cand[i].s[1] / 2, cand[j].c[1] + cand[j].s[1] / 2) -
                                                std::max(cand[i].c[1] - cand[i].s[1] / 2, cand[j].c[1] - cand[j].s[1] / 2));
            const double inter = ix * iy;
            const double uni = cand[i].s[0] * cand[i].s[1] + cand[j].s[0] * cand[j].s[1] - inter;
            if (j != i && inter / (uni + 1e-9) <= 0.3) continue;
            used[j] = true;
            const double w = cand[j].score;
            wsum += w;
            acc.c = acc.c + cand[j].c * w;
            acc.s = acc.s + cand[j].s * w;
            for (int k = 0; k < 7; ++k) acc.kp[k] = acc.kp[k] + cand[j].kp[k] * w;
        }
        Palm p;
        const V2 c = acc.c * (1 / wsum);
        p.center = to_image(tm, c[0], c[1]);
        p.size = acc.s * (1 / wsum * size / kPalmSize);
        for (int k = 0; k < 7; ++k) {
            const V2 q = acc.kp[k] * (1 / wsum);
            p.kp[k] = to_image(tm, q[0], q[1]);
        }
        p.score = cand[i].score;
        out.push_back(p);
    }
    return out;
}

void Nets::hand_crop(const Image &img, const Roi &roi, uint8_t *out, float tm[6]) const {
    crop_matrix(roi.center, roi.size, roi.rotation, kHandSize, tm);
    gray_crop(img, tm, kHandSize, hand_contrast_, out);
}

void Nets::palm_crop(const Image &img, const Roi &roi, uint8_t *out, float tm[6]) const {
    crop_matrix(roi.center, roi.size, roi.rotation, kHandSize, tm);
    gray_crop(img, tm, kHandSize, palm_contrast_, out);
}

V2 Nets::crop_to_image(const float tm[6], double x, double y) { return to_image(tm, x, y); }

Landmarks Nets::landmarks(const Image &img, const Roi &roi) const {
    float tm[6];
    crop_matrix(roi.center, roi.size, roi.rotation, kHandSize, tm);
    ncnn::Extractor ex = hand_.create_extractor();
    ex.input("in0", crop(img, tm, kHandSize, hand_contrast_));
    ncnn::Mat screen, presence, right, world;
    ex.extract("out0", screen);
    ex.extract("out1", presence);
    ex.extract("out2", right);
    ex.extract("out3", world);
    Landmarks lm;
    const float *s = screen, *w = world;
    for (int i = 0; i < 21; ++i) {
        lm.pts[i] = to_image(tm, s[3 * i], s[3 * i + 1]);
        for (int k = 0; k < 3; ++k) lm.world[i][k] = w[3 * i + k];
    }
    lm.presence = presence[0];
    lm.right = right[0];
    return lm;
}
