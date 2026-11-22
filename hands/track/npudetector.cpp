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

bool NpuDetector::run(const std::vector<const Image *> &images, std::vector<std::vector<Palm>> &out) {
    if (images.empty() || !s_.ensure() || !s_.client().has_detector()) return false;
    NpuClient &cl = s_.client();
    struct Job {
        size_t image;
        Roi roi;
    };
    std::vector<Job> jobs;
    for (size_t i = 0; i < images.size(); ++i)
        for (const Roi &r : det_tiles(images[i]->width, images[i]->height, grid_, frac_)) jobs.push_back({i, r});
    std::vector<std::vector<Palm>> found(images.size());
    float tm[NpuClient::kMaxCrops][6];
    const float s_obj = cl.det_scale(0), s_reg = cl.det_scale(1);
    for (size_t at = 0; at < jobs.size(); at += NpuClient::kMaxCrops) {
        const int n = int(std::min<size_t>(NpuClient::kMaxCrops, jobs.size() - at));
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) nets_.palm_crop(*images[jobs[at + i].image], jobs[at + i].roi, cl.crop(i), tm[i]);
        const auto t1 = std::chrono::steady_clock::now();
        std::string e;
        if (!cl.run_det(n, NpuSession::kTimeoutMs, e)) return s_.fail(e), false;
        const auto t2 = std::chrono::steady_clock::now();
        times.add(std::chrono::duration<double, std::milli>(t1 - t0).count(),
                  std::chrono::duration<double, std::milli>(t2 - t1).count(), cl);
        for (int i = 0; i < n; ++i) {
            const int8_t *o = cl.det_output(i);
            for (DetPoint d : decode_det(o, o + G * G, s_obj, s_reg, thr_)) {
                d.wrist = Nets::crop_to_image(tm[i], d.wrist[0], d.wrist[1]);
                d.middle = Nets::crop_to_image(tm[i], d.middle[0], d.middle[1]);
                found[jobs[at + i].image].push_back(palm_from(d));
            }
        }
    }
    for (auto &f : found) f = merge_palms(std::move(f));
    out = std::move(found);
    return true;
}
