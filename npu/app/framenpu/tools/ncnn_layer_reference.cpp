/* Strict FP32 ncnn oracle for one isolated convolution with supplied CHW input. */
#include <net.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        if(argc!=8) throw std::runtime_error("usage: layer_reference param bin input c h w output");
        int c=std::stoi(argv[4]),h=std::stoi(argv[5]),w=std::stoi(argv[6]);
        if(c<=0||h<=0||w<=0||c>8192||h>8192||w>8192) throw std::runtime_error("invalid dimensions");
        ncnn::Mat m(w,h,c);
        FILE* input=fopen(argv[3],"rb");if(!input) throw std::runtime_error("input open failed");
        for(int q=0;q<c;q++) if(fread(m.channel(q),4,size_t(h)*w,input)!=size_t(h)*w) throw std::runtime_error("input truncated");
        if(fgetc(input)!=EOF) throw std::runtime_error("input trailing bytes");fclose(input);
        ncnn::Net net;
        net.opt.num_threads=1;net.opt.use_vulkan_compute=false;net.opt.use_packing_layout=false;
        net.opt.use_fp16_packed=net.opt.use_fp16_storage=net.opt.use_fp16_arithmetic=false;
        net.opt.use_bf16_storage=false;net.opt.use_winograd_convolution=false;net.opt.use_sgemm_convolution=false;
        if(net.load_param(argv[1])||net.load_model(argv[2])) throw std::runtime_error("model load failed");
        ncnn::Extractor ex=net.create_extractor();
        ncnn::Mat result;
        if(ex.input("input",m)||ex.extract("output",result)||result.elempack!=1||result.elemsize!=4u) throw std::runtime_error("extraction failed");
        FILE* output=fopen(argv[7],"wb");if(!output) throw std::runtime_error("output open failed");
        for(int q=0;q<result.c;q++) if(fwrite(result.channel(q),4,size_t(result.h)*result.w,output)!=size_t(result.h)*result.w) throw std::runtime_error("output write failed");
        fclose(output);return 0;
    } catch(const std::exception& error) {fprintf(stderr,"layer reference: %s\n",error.what());return 2;}
}
