// Pictures of a body offscreen (picture.h).
#include "picture.h"

#include "../pose/bodypose.h"
#include "bodygpu.h"
#include "demo.h"
#include "gpuctx.h"
#include "handfit.h"
#include "rendernode.h"
#include "viewmath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace picture {

using avatar::Vec;
using namespace bodyfile;

namespace {

constexpr int kFine = 2;   // drawn this many times the size each way, then averaged down

// The offscreen context and what is drawn into: colour to read back, and depth.
struct Canvas {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    bool node = false;
    GLuint tex = 0, depth = 0, fbo = 0;
    int w = 0, h = 0;

    bool Open(int width, int height) {
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
    // What was drawn, averaged down by kFine: colour times alpha, and alpha, the top row first.
    std::vector<float> Read() {
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
                        for (int dx = 0; dx < kFine; ++dx) sum += px[(size_t(h - 1 - (y * kFine + dy)) * w + x * kFine + dx) * 4 + c];
                    out[(size_t(y) * ow + x) * 4 + c] = float(sum) / float(kFine * kFine * 255);
                }
        return out;
    }
    void Close() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (depth) glDeleteRenderbuffers(1, &depth);
        if (tex) glDeleteTextures(1, &tex);
        gpuctx::Close(dpy, ctx);
        if (node) rendernode::Release();
    }
};

// The body's matrices `at` seconds into the pose, the wearer's moves solved 90 times a second
// up to then (what trails the head, trails it).
bool Posed(const Body &body, const Ask &ask, float bones[kRoles][12]) {
    bodypose::Solver solver(body);
    bodypose::Pose pose;
    if (ask.pose == "rest") {
        for (int r = 0; r < kRoles; ++r) pose.at[r] = {body.at[r][0], body.at[r][1], body.at[r][2]};
        return solver.Matrices(pose, bones), true;
    }
    const handfit::Fit fit(body);
    const double step = 1.0 / 90, until = std::min(std::max(ask.at, 0.0), 30.0);
    for (double t = 0;; t += step) {
        bodypose::Input in;
        if (!demo::Pose(ask.pose.c_str(), std::min(t, until), body, fit, in)) return std::fprintf(stderr, "avatars: no pose called %s (rest %s)\n", ask.pose.c_str(), demo::kNames), false;
        in.dt = t > 0 ? step : 0;
        if (!solver.Solve(in, pose)) return std::fprintf(stderr, "avatars: the pose can't be solved\n"), false;
        if (t >= until) break;
    }
    return solver.Matrices(pose, bones), true;
}

struct Loaded {
    modelfile::Model model;
    Body body;
    bool Read(const std::string &path) {
        std::string err;
        bool has = false;
        if (!bodyfile::ReadFile(path, model, body, has, err)) return std::fprintf(stderr, "avatars: %s\n", err.c_str()), false;
        if (!has) return std::fprintf(stderr, "avatars: %s has hands and no body\n", path.c_str()), false;
        return true;
    }
};

// The body drawn into a part of the canvas (pixels, from the bottom left) as a camera sees it
// from `turn` radians round it (0: from in front) and `rise` up, the box (lo to hi) filling the part.
void DrawView(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[kRoles][12], const float lo[3], const float hi[3],
              double turn, double rise, int x, int y, int w, int h) {
    const Vec mid = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    const double tall = (hi[1] - lo[1]) / 2, wide = Vec{hi[0] - lo[0], 0, hi[2] - lo[2]}.norm() / 2;
    const double lens = 24 * bodypose::kPi / 180, aspect = double(w) / h, half = std::tan(lens / 2);
    const double away = std::max(tall / half, wide / (half * aspect)) * 1.08 + wide;
    const Vec from = bodypose::Turn::About({0, 1, 0}, turn) * (bodypose::Turn::About({1, 0, 0}, rise) * Vec{0, 0, -1});
    const Vec eye = mid + from * away, right = from.cross(Vec{0, 1, 0}).unit();
    const viewmath::Mat4 clip = viewmath::Mul(viewmath::Perspective(lens, aspect, std::max(0.05, away - 2 * wide - tall), away + 2 * wide + tall),
                                              viewmath::LookAt(eye, mid, {0, 1, 0}));
    const Vec light = (from * 0.6 + Vec{0, 0.55, 0} + right * 0.45).unit();   // from over the camera's shoulder
    bodygpu::View view;
    std::copy(clip.m, clip.m + 16, view.clip);
    view.light[0] = float(light.x), view.light[1] = float(light.y), view.light[2] = float(light.z);
    glViewport(x, y, w, h);
    avatar.Draw(painter, bones, view);
}

int Draw(const Ask &ask, bool thumb) {
    Loaded file;
    if (!file.Read(ask.model)) return 1;
    float bones[kRoles][12], lo[3], hi[3];
    if (!Posed(file.body, ask, bones)) return 1;
    const int size = std::min(std::max(ask.size, 64), 1024), views = thumb ? 1 : 4;
    const int cw = thumb ? size : int(size * 0.6), ch = size;
    Canvas canvas;
    bodygpu::Painter painter;
    bodygpu::Avatar avatar;
    int status = 1;
    if (canvas.Open(cw * views * kFine, ch * kFine) && painter.Init("avatars")) {
        avatar.Upload(painter, file.body);
        avatar.Shape(ask.shapes);
        bodymesh::Bounds(file.body, avatar.pos(), bones, 1, lo, hi);
        // a sheet: from in front, from a side of the front, from the side, from behind
        static const double kTurns[4] = {0, -0.7, -bodypose::kPi / 2, bodypose::kPi};
        for (int v = 0; v < views; ++v) DrawView(painter, avatar, bones, lo, hi, thumb ? -0.35 : kTurns[v], thumb ? 0.08 : 0.05, v * cw * kFine, 0, cw * kFine, ch * kFine);
        const std::vector<float> px = canvas.Read();
        const int W = cw * views, H = ch, channels = thumb ? 4 : 3;
        std::vector<uint8_t> img(size_t(W) * H * channels);
        for (size_t i = 0; i < size_t(W) * H; ++i) {
            const float *p = &px[i * 4], a = p[3];
            for (int c = 0; c < 3; ++c) {
                // a thumb keeps its alpha (the colour as it is, not times alpha); a sheet goes over grey
                const float v = thumb ? (a > 0 ? p[c] / a : 0) : p[c] + 0.32f * (1 - a);
                img[i * channels + c] = uint8_t(std::lround(std::min(1.0f, v) * 255));
            }
            if (thumb) img[i * 4 + 3] = uint8_t(std::lround(a * 255));
        }
        if (stbi_write_png(ask.out.c_str(), W, H, channels, img.data(), W * channels)) {
            status = 0;
            std::printf("wrote %s: %s, %s, %d vertices, %d triangles in %zu parts, %zu textures, %.2f m tall as posed\n", ask.out.c_str(),
                        file.model.info.name.c_str(), ask.pose.c_str(), file.body.verts(), file.body.tris(), file.body.parts.size(), file.body.textures.size(), hi[1] - lo[1]);
        } else {
            std::fprintf(stderr, "avatars: can't write %s\n", ask.out.c_str());
        }
    }
    avatar.Free(), painter.Done(), canvas.Close();
    return status;
}

}  // namespace

int Sheet(const Ask &ask) { return Draw(ask, false); }
int Thumb(const Ask &ask) { return Draw(ask, true); }

}  // namespace picture
