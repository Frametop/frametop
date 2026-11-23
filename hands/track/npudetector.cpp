#include "npudetector.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int G = NpuClient::kDetGrid, kStride = NpuClient::kCrop / G;
constexpr double kAlong = 0.374, kBoxSize = 1.02;   // palm.py BOX

double cell(int i) { return (i + 0.5) * kStride - 0.5; }

}  // namespace

std::vector<Roi> det_tiles(int width, int height, int grid, double frac) {
    const double size = frac * std::max(width, height);
    auto centres = [&](double ext) {
        std::vector<double> c;
        if (grid == 1) return std::vector<double>{ext / 2.0};
        for (int i = 0; i < grid; ++i) c.push_back(size / 2 + i * (ext - size) / (grid - 1));
        return c;
    };
    std::vector<Roi> out;
    for (double cy : centres(height))
        for (double cx : centres(width)) out.push_back(Roi{{cx, cy}, size, 0.0});
    return out;
}

std::vector<DetPoint> decode_det(const int8_t *obj, const int8_t *reg, float s_obj, float s_reg, float thr) {
    // sigmoid(q * s) > thr  <=>  q * s > logit(thr); local maxima compare the integers (sigmoid is monotonic)
    const double logit = std::log(thr / (1.0 - thr));
    std::vector<DetPoint> out;
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x) {
            const int q = obj[y * G + x];
            if (q * double(s_obj) <= logit) continue;
            bool peak = true;
            for (int dy = -1; dy <= 1 && peak; ++dy)
                for (int dx = -1; dx <= 1 && peak; ++dx) {
                    const int yy = y + dy, xx = x + dx;
                    if ((dy || dx) && yy >= 0 && yy < G && xx >= 0 && xx < G) peak = q >= obj[yy * G + xx];
                }
            if (!peak) continue;
            auto r = [&](int c) { return kStride * double(reg[c * G * G + y * G + x]) * s_reg; };
            DetPoint d;
            d.score = 1.0 / (1.0 + std::exp(-q * double(s_obj)));
            d.wrist = {cell(x) + r(0), cell(y) + r(1)};
            d.middle = {cell(x) + r(2), cell(y) + r(3)};
            out.push_back(d);
        }
    return out;
}

Palm palm_from(const DetPoint &d) {
    Palm p;
    const V2 u = d.middle - d.wrist;
    const double size = kBoxSize * norm(u);
    p.center = d.wrist + u * kAlong;
    p.size = {size, size};
    p.kp[0] = p.kp[1] = d.wrist;
    p.kp[2] = d.middle;
    p.score = d.score;
    return p;
}

std::vector<Palm> merge_palms(std::vector<Palm> palms) {
    std::stable_sort(palms.begin(), palms.end(), [](const Palm &a, const Palm &b) { return a.score > b.score; });
    std::vector<Palm> out;
    for (const Palm &p : palms) {
        bool keep = true;
        for (const Palm &o : out) keep = keep && norm(p.center - o.center) >= 0.5 * std::max(o.size[0], 1.0);
        if (keep) out.push_back(p);
    }
    return out;
}

bool NpuDetector::post(const Image &image, const std::string &camera) {
    if (pending_ || !s_.ensure() || !s_.client().has_detector()) return false;
    NpuClient &cl = s_.client();
    const std::vector<Roi> tiles = det_tiles(image.width, image.height, grid_, frac_);
    n_ = int(std::min<size_t>(NpuClient::kMaxCrops, tiles.size()));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n_; ++i) nets_.palm_crop(image, tiles[size_t(i)], cl.crop(i), tm_[i]);
    crop_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string e;
    if (!cl.post_det(n_, e)) return s_.fail(e), false;
    camera_ = camera, connection_ = s_.connection(), pending_ = true;
    return true;
}

bool NpuDetector::collect(std::string &camera, std::vector<Palm> &out) {
    if (!pending_) return false;
    pending_ = false;
    NpuClient &cl = s_.client();
    // lost with the connection (a request settled by another backend's ensure() kept its outputs: read on)
    if (!cl.connected() || s_.connection() != connection_) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const bool was_in_flight = cl.in_flight();
    std::string e;
    if (!cl.settle(NpuSession::kTimeoutMs, e)) return s_.fail(e), false;
    if (was_in_flight)   // the header holds this request's times only until the next request
        times.add(crop_ms_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), cl);
    const float s_obj = cl.det_scale(0), s_reg = cl.det_scale(1);
    std::vector<Palm> found;
    for (int i = 0; i < n_; ++i) {
        const int8_t *o = cl.det_output(i);
        for (DetPoint d : decode_det(o, o + G * G, s_obj, s_reg, thr_)) {
            d.wrist = Nets::crop_to_image(tm_[i], d.wrist[0], d.wrist[1]);
            d.middle = Nets::crop_to_image(tm_[i], d.middle[0], d.middle[1]);
            found.push_back(palm_from(d));
        }
    }
    out = merge_palms(std::move(found));
    camera = camera_;
    return true;
}
