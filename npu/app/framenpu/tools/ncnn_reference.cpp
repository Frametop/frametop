/* Reference exporter linked to pinned external ncnn, never to the DSP backend.
 * Input is a raw grayscale camera frame plus an explicit ROI affine. Crop math
 * follows the pinned FrameTop nets.cpp; no camera/model data is distributed.
 */
#include <net.h>
#include <mat.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static void save(const std::string& path, const ncnn::Mat& m) {
    if (m.empty() || m.elempack!=1 || m.elemsize!=4u) throw std::runtime_error("reference is not unpacked FP32");
    FILE* f=fopen(path.c_str(),"wb"); if(!f) throw std::runtime_error("output open failed");
    for(int q=0;q<m.c;q++) {
        const float* p=m.channel(q);
        if(fwrite(p,4,size_t(m.w)*m.h,f)!=size_t(m.w)*m.h) throw std::runtime_error("output write failed");
    }
    fclose(f);
}

int main(int argc,char** argv) {
    try {
        if(argc!=12 && argc!=13) throw std::runtime_error("usage: reference param bin gray width height cx cy size rotation output_dir contrast_none [all]");
        if(argc==13 && std::string(argv[12])!="all") throw std::runtime_error("unknown export mode");
        int w=std::stoi(argv[4]),h=std::stoi(argv[5]);
        double cx=std::stod(argv[6]),cy=std::stod(argv[7]),size=std::stod(argv[8]),rotation=std::stod(argv[9]);
        if(w<=0||h<=0||w>8192||h>8192||size<=0||!std::isfinite(size)||std::string(argv[11])!="none") throw std::runtime_error("invalid frame/ROI/contrast");
        std::ifstream input(argv[3],std::ios::binary);
        std::vector<unsigned char> frame(size_t(w)*h);
        if(!input.read((char*)frame.data(),frame.size())||input.peek()!=EOF) throw std::runtime_error("gray frame length differs");
        float tm[6];
        double c=cos(rotation),s=sin(rotation),k=size/224;
        tm[0]=float(c*k);tm[1]=float(-s*k);tm[3]=float(s*k);tm[4]=float(c*k);
        tm[2]=float(cx-(tm[0]+tm[1])*224/2.0);tm[5]=float(cy-(tm[3]+tm[4])*224/2.0);
        std::vector<unsigned char> crop(224*224);
        ncnn::warpaffine_bilinear_c1(frame.data(),w,h,w,crop.data(),224,224,224,tm,0,0);
        std::filesystem::create_directories(argv[10]);
        std::string out=argv[10];
        FILE* pgm=fopen((out+"/crop.pgm").c_str(),"wb");
        fprintf(pgm,"P5\n224 224\n255\n");fwrite(crop.data(),1,crop.size(),pgm);fclose(pgm);
        ncnn::Mat m=ncnn::Mat::from_pixels(crop.data(),ncnn::Mat::PIXEL_GRAY2RGB,224,224);
        float norm[3]={1/255.f,1/255.f,1/255.f};m.substract_mean_normalize(nullptr,norm);
        save(out+"/in0.f32",m);
        ncnn::Net net;
        net.opt.num_threads=1;net.opt.use_vulkan_compute=false;
        net.opt.use_packing_layout=false;
        net.opt.use_fp16_packed=net.opt.use_fp16_storage=net.opt.use_fp16_arithmetic=false;
        net.opt.use_bf16_storage=false;
        net.opt.use_winograd_convolution=false;net.opt.use_sgemm_convolution=false;
        if(net.load_param(argv[1])||net.load_model(argv[2])) throw std::runtime_error("ncnn model load failed");
        ncnn::Extractor ex=net.create_extractor();ex.set_light_mode(false);
        if(ex.input("in0",m)) throw std::runtime_error("reference input failed");
        std::vector<std::string> names={"2","3","4","8","9"};
        if(argc==13) {
            names.clear();
            for(const auto& blob:net.blobs()) if(blob.name!="in0") names.push_back(blob.name);
        }
        for(const auto& name:names) {
            const char* blob=name.c_str();
            ncnn::Mat result;
            if(ex.extract(blob,result)) throw std::runtime_error("reference extraction failed");
            save(out+"/blob_"+blob+".f32",result);
            printf("%s %d %d %d\n",blob,result.c,result.h,result.w);
        }
        return 0;
    } catch(const std::exception& error) {
        fprintf(stderr,"reference: %s\n",error.what());return 2;
    }
}
