// An offscreen picture (canvas.h).
#include "canvas.h"

#include "rendernode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace canvas {

bool Canvas::Open(int width, int height) {
    gbm_device *gbm = rendernode::Acquire("avatars");
    node = gbm != nullptr;
    if (!gbm || !gpuctx::Open("avatars", gbm, gpuctx::Load(), 0, &dpy, &ctx)) return false;
    GLint most = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &most);
    if (width > most || height > most) return std::fprintf(stderr, "avatars: a picture %d by %d is more than the GPU draws\n", width, height), false;
    w = width, h = height;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return std::fprintf(stderr, "avatars: no offscreen buffer\n"), false;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

std::vector<float> Canvas::Read(bool topFirst) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const int ow = w / kFine, oh = h / kFine;
    std::vector<float> out(size_t(ow) * oh * 4);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x)
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int dy = 0; dy < kFine; ++dy)
                    for (int dx = 0; dx < kFine; ++dx) {
                        const int row = y * kFine + dy;
                        sum += px[(size_t(topFirst ? row : h - 1 - row) * w + x * kFine + dx) * 4 + c];
                    }
                out[(size_t(y) * ow + x) * 4 + c] = float(sum) / float(kFine * kFine * 255);
            }
    return out;
}

void Canvas::Close() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (depth) glDeleteRenderbuffers(1, &depth);
    if (tex) glDeleteTextures(1, &tex);
    gpuctx::Close(dpy, ctx);
    if (node) rendernode::Release();
}

bool Save(const std::string &path, const std::vector<float> &px, int w, int h) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float a = px[i * 4 + 3];
        for (int c = 0; c < 3; ++c) img[i * 4 + c] = uint8_t(std::lround(std::min(1.0f, a > 0 ? px[i * 4 + c] / a : 0) * 255));
        img[i * 4 + 3] = uint8_t(std::lround(a * 255));
    }
    return stbi_write_png(path.c_str(), w, h, 4, img.data(), w * 4) != 0;
}

}  // namespace canvas
