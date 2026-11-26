// In-process CPU prefix for the RGB -> heads comparison. Link against the
// pinned optimized ncnn build used by ncnn_cpu_bench.cpp. No file I/O per run.
#include <cstring>
#include <memory>
#include "net.h"

extern "C" {
void *fnpu_prefix_create(const char *param, const char *weights, int fp16) {
    try {
        std::unique_ptr<ncnn::Net> net(new ncnn::Net);
        net->opt.num_threads = 1;
        net->opt.use_vulkan_compute = false;
        if (!fp16) {
            net->opt.use_fp16_packed = false;
            net->opt.use_fp16_storage = false;
            net->opt.use_fp16_arithmetic = false;
            net->opt.use_bf16_storage = false;
        }
        if (net->load_param(param) || net->load_model(weights)) return nullptr;
        return net.release();
    } catch (...) { return nullptr; }
}

void fnpu_prefix_destroy(void *handle) { delete static_cast<ncnn::Net *>(handle); }

int fnpu_prefix_run(void *handle, const float *rgb, float *out8, float *out10) {
    if (!handle || !rgb || !out8 || !out10) return -1;
    try {
        ncnn::Mat input(224,224,3);
        if (input.empty()) return -2;
        for (int ch=0;ch<3;ch++)
            std::memcpy(input.channel(ch),rgb+ch*224*224,224*224*sizeof(float));
        ncnn::Extractor ex = static_cast<ncnn::Net *>(handle)->create_extractor();
        if (ex.input("in0",input)) return -3;
        ncnn::Mat a,b;
        if (ex.extract("8",a) || ex.extract("10",b)) return -4;
        // extract(type=0) unpacks and converts storage to FP32; channel cstep
        // may contain alignment padding, so copy each channel separately.
        if (a.dims!=3 || a.w!=56 || a.h!=56 || a.c!=64 || a.elempack!=1 || a.elemsize!=4 ||
            b.dims!=3 || b.w!=56 || b.h!=56 || b.c!=16 || b.elempack!=1 || b.elemsize!=4) return -5;
        for (int ch=0;ch<64;ch++)
            std::memcpy(out8+ch*56*56,a.channel(ch),56*56*sizeof(float));
        for (int ch=0;ch<16;ch++)
            std::memcpy(out10+ch*56*56,b.channel(ch),56*56*sizeof(float));
        return 0;
    } catch (...) { return -6; }
}
}
