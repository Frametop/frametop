// CPU timing of the pinned v20a hand landmark model with a normal optimized
// ncnn build (ncnn 20260526 built on the device with its default Release
// options and NCNN_VULKAN=OFF; see evidence/ncnn-cpu-baseline.json).
//
// usage: ncnn_cpu_bench <param> <bin> <crop_dir> <mode> <threads> <fp16> <warmup> <iters>
//   mode "full": input in0 (3x224x224), extract out0..out3
//   mode "tail": inputs blobs 8 and 10 (from the crop's ncnn FP32 blobs),
//                extract out0..out3 (the slice the DSP resident run covers,
//                plus pooling and heads)
// Prints one JSON line: per-iteration wall times (ms) and max |out - oracle|
// per head against the crop's FP32 oracle blobs. No tensor data is printed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "net.h"

static std::vector<float> load(const std::string &path, size_t n) {
    std::vector<float> v(n);
    std::ifstream f(path, std::ios::binary);
    f.read(reinterpret_cast<char *>(v.data()), n * sizeof(float));
    if (!f || f.gcount() != static_cast<std::streamsize>(n * sizeof(float))) {
        std::fprintf(stderr, "short read %s\n", path.c_str());
        std::exit(2);
    }
    return v;
}

static ncnn::Mat as_mat(std::vector<float> &v, int w, int h, int c) {
    ncnn::Mat m(w, h, c);
    for (int q = 0; q < c; q++) std::memcpy(m.channel(q), v.data() + size_t(q) * w * h, size_t(w) * h * sizeof(float));
    return m;
}

int main(int argc, char **argv) {
    if (argc != 9) {
        std::fprintf(stderr, "usage: %s param bin crop_dir full|tail threads fp16 warmup iters\n", argv[0]);
        return 2;
    }
    std::string crop = argv[3], mode = argv[4];
    int threads = std::atoi(argv[5]), fp16 = std::atoi(argv[6]), warm = std::atoi(argv[7]), iters = std::atoi(argv[8]);
    ncnn::Net net;
    net.opt.num_threads = threads;
    net.opt.use_vulkan_compute = false;
    if (!fp16) {
        net.opt.use_fp16_packed = net.opt.use_fp16_storage = net.opt.use_fp16_arithmetic = false;
        net.opt.use_bf16_storage = false;
    }
    if (net.load_param(argv[1]) || net.load_model(argv[2])) return 3;

    std::vector<float> in0, b8, b10;
    if (mode == "full") in0 = load(crop + "/in0.f32", 3 * 224 * 224);
    else {
        b8 = load(crop + "/blob_8.f32", 64 * 56 * 56);
        b10 = load(crop + "/blob_10.f32", 16 * 56 * 56);
    }
    const char *outs[4] = {"out0", "out1", "out2", "out3"};
    const int sizes[4] = {63, 1, 1, 63};
    std::vector<std::vector<float>> ref(4);
    for (int i = 0; i < 4; i++) ref[i] = load(crop + "/blob_" + outs[i] + ".f32", sizes[i]);

    std::vector<double> ms;
    double err[4] = {0, 0, 0, 0};
    for (int it = 0; it < warm + iters; it++) {
        auto t0 = std::chrono::steady_clock::now();
        ncnn::Extractor ex = net.create_extractor();
        if (mode == "full") ex.input("in0", as_mat(in0, 224, 224, 3));
        else {
            ex.input("8", as_mat(b8, 56, 56, 64));
            ex.input("10", as_mat(b10, 56, 56, 16));
        }
        ncnn::Mat o[4];
        for (int i = 0; i < 4; i++) ex.extract(outs[i], o[i]);
        auto t1 = std::chrono::steady_clock::now();
        if (it >= warm) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (it == warm + iters - 1)
            for (int i = 0; i < 4; i++) {
                if (o[i].dims != 1 || o[i].w != sizes[i] || o[i].elempack != 1 || o[i].elemsize != 4) return 4;
                const float *f = o[i];
                for (int k = 0; k < sizes[i]; k++) err[i] = std::max(err[i], double(std::fabs(f[k] - ref[i][k])));
            }
    }
    std::sort(ms.begin(), ms.end());
    auto pct = [&](double p) { return ms[std::min(ms.size() - 1, size_t(p * ms.size()))]; };
    std::printf("{\"mode\":\"%s\",\"threads\":%d,\"fp16\":%d,\"iters\":%d,\"min_ms\":%.4f,\"p50_ms\":%.4f,"
                "\"p90_ms\":%.4f,\"max_ms\":%.4f,\"max_abs_err\":[%.3g,%.3g,%.3g,%.3g]}\n",
                mode.c_str(), threads, fp16, iters, ms.front(), pct(0.5), pct(0.9), ms.back(), err[0], err[1], err[2],
                err[3]);
    return 0;
}
