// CPU-only resolution experiment. Original weights, strict FP32 options and
// grayscale affine/normalization match ncnn_reference.cpp. No DSP access.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include "net.h"

extern "C" {
void *fnpu_resolution_create(const char *param, const char *weights) {
    try {
        std::unique_ptr<ncnn::Net> net(new ncnn::Net);
        net->opt.num_threads=1;
        net->opt.use_vulkan_compute=false;
        net->opt.use_packing_layout=false;
        net->opt.use_fp16_packed=false;
        net->opt.use_fp16_storage=false;
        net->opt.use_fp16_arithmetic=false;
        net->opt.use_bf16_storage=false;
        net->opt.use_winograd_convolution=false;
        net->opt.use_sgemm_convolution=false;
        if (net->load_param(param) || net->load_model(weights)) return nullptr;
        return net.release();
    } catch (...) { return nullptr; }
}
void fnpu_resolution_destroy(void *net) { delete static_cast<ncnn::Net *>(net); }

int fnpu_resolution_crop(const unsigned char *frame, int w, int h,
        const double *roi, int crop_n, int input_n, float *gray) {
    if (!frame || !roi || !gray || w<=0 || h<=0 || w>8192 || h>8192 ||
            crop_n<32 || crop_n>512 || input_n<32 || input_n>512) return -1;
    for (int i=0;i<4;i++) if (!std::isfinite(roi[i])) return -1;
    if (roi[2]<=0) return -1;
    try {
        float tm[6];
        double c=std::cos(roi[3]),s=std::sin(roi[3]),k=roi[2]/crop_n;
        tm[0]=float(c*k);tm[1]=float(-s*k);tm[3]=float(s*k);tm[4]=float(c*k);
        tm[2]=float(roi[0]-(tm[0]+tm[1])*crop_n/2.0);
        tm[5]=float(roi[1]-(tm[3]+tm[4])*crop_n/2.0);
        std::vector<unsigned char> crop(crop_n*crop_n),resized;
        ncnn::warpaffine_bilinear_c1(frame,w,h,w,crop.data(),crop_n,crop_n,crop_n,tm,0,0);
        const unsigned char *p=crop.data();
        if (crop_n!=input_n) {
            resized.resize(input_n*input_n);
            ncnn::resize_bilinear_c1(p,crop_n,crop_n,resized.data(),input_n,input_n);
            p=resized.data();
        }
        const float norm=1/255.f;
        for (int i=0;i<input_n*input_n;i++) gray[i]=float(p[i])*norm;
        return 0;
    } catch (...) { return -2; }
}

int fnpu_resolution_run(void *handle, const float *gray, int n, float *heads, int *shape) {
    if (!handle || !gray || !heads || !shape || n<32 || n>512) return -1;
    try {
        ncnn::Mat input(n,n,3);
        if (input.empty()) return -2;
        for (int ch=0;ch<3;ch++) std::memcpy(input.channel(ch),gray,n*n*sizeof(float));
        ncnn::Extractor ex=static_cast<ncnn::Net *>(handle)->create_extractor();
        ex.set_light_mode(false);
        if (ex.input("in0",input)) return -3;
        ncnn::Mat last;
        if (ex.extract("79",last) || last.dims!=3 || last.c!=672) return -4;
        shape[0]=last.c;shape[1]=last.h;shape[2]=last.w;
        const char *names[]={"out0","out3","out2","out1"};
        const int sizes[]={63,63,1,1};
        for (int i=0,at=0;i<4;at+=sizes[i++]) {
            ncnn::Mat output;
            if (ex.extract(names[i],output) || output.d!=1 || output.w*output.h*output.c!=sizes[i] ||
                    output.elempack!=1 || output.elemsize!=4u) return -5;
            // InnerProduct may retain 1x1xC: cstep includes alignment padding.
            for (int ch=0;ch<output.c;ch++)
                std::memcpy(heads+at+ch*output.w*output.h,output.channel(ch),output.w*output.h*sizeof(float));
        }
        return 0;
    } catch (...) { return -6; }
}
}
