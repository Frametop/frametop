// Hand cutouts, the GPU side (see handcut.h): Renderer draws a panel's client buffer with
// the hands cut out and the glows on top. And EyePositions, which asks SteamVR.
#include "handcut.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace handcut {

void EyePositions(const Mat &head, double out[2][3]) {
    const vr::EVREye eyes[2] = {vr::Eye_Left, vr::Eye_Right};
    for (int e = 0; e < 2; ++e) {
        const Mat t = vr::VRSystem()->GetEyeToHeadTransform(eyes[e]);
        for (int i = 0; i < 3; ++i)
            out[e][i] = head.m[i][0] * t.m[0][3] + head.m[i][1] * t.m[1][3] + head.m[i][2] * t.m[2][3] + head.m[i][3];
    }
}

// ---------------------------------------------------------------------------- renderer

namespace {

PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
PFNEGLCREATEIMAGEKHRPROC pCreateImage;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pImageTargetRenderbuffer;
PFNEGLCREATESYNCKHRPROC pCreateSync;
PFNEGLDESTROYSYNCKHRPROC pDestroySync;
PFNEGLCLIENTWAITSYNCKHRPROC pClientWaitSync;

const char *kVertex = R"(
attribute vec2 pos;          // the unit square
uniform vec4 rect;           // where it goes, in pixels of the eye's half: x0 y0 x1 y1
uniform vec2 size;           // the half's size in pixels
varying vec2 px;
varying vec2 uv;
void main() {
    px = mix(rect.xy, rect.zw, pos);
    uv = px / size;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})";

// Pixel positions as they are (a hull's polygon, handcut.h Hull2D::fan).
const char *kVertexPx = R"(
attribute vec2 pos;          // pixels of the eye's half
uniform vec2 size;
varying vec2 px;
void main() {
    px = pos;
    gl_Position = vec4(pos / size * 2.0 - 1.0, 0.0, 1.0);
})";

// The client's pixels, opaque (its alpha is ignored, as IgnoreTextureAlpha did).
const char *kCopy = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;   // mediump (16-bit on Adreno) steps 1.7 texels across a 3440-pixel screen
uniform samplerExternalOES tex;
varying vec2 uv;
void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); })";

// Coverage of one tapered capsule; blended to take that much alpha away.
const char *kCut = R"(
precision highp float;
uniform vec2 a, b, r;
uniform float feather;
varying vec2 px;
void main() {
    vec2 ab = b - a;
    float t = clamp(dot(px - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    float d = length(px - (a + t * ab));
    float rad = mix(r.x, r.y, t);
    if (d >= rad + feather) discard;   // outside it: nothing to blend
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0 - smoothstep(rad - feather, rad + feather, d));
})";

// Coverage of one convex polygon grown by r (handcut.h, Hull2D); blended like kCut.
const char *kHull = R"(
precision highp float;
uniform vec3 edges[16];
uniform int n;
uniform float r, feather;
varying vec2 px;
void main() {
    float d = -1e9;
    for (int i = 0; i < 16; ++i) {
        if (i >= n) break;
        d = max(d, dot(edges[i].xy, px) - edges[i].z);
        if (d >= r + feather) discard;   // outside: most of the box (and no blend to pay for)
    }
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0 - smoothstep(r - feather, r + feather, d));
})";

// A glow of hand input: a soft ring with a faint fill, or a filled dot; blended over the
// colour, the alpha (the cutouts) left as it is, so a glow under a finger stays hidden.
const char *kGlow = R"(
precision highp float;
uniform vec2 c;
uniform float r, alpha, ring;
uniform vec3 color;
varying vec2 px;
void main() {
    float d = length(px - c);
    float a;
    if (ring > 0.5) {
        float w = max(1.5, 0.22 * r);
        a = (1.0 - smoothstep(0.0, w, abs(d - r))) + 0.35 * (1.0 - smoothstep(0.0, r, d));
    } else {
        a = 1.0 - smoothstep(r - 1.0, r + 1.0, d);
    }
    a = clamp(a, 0.0, 1.0) * alpha;
    if (a <= 0.0) discard;
    gl_FragColor = vec4(color, a);
})";

// A probe dot: the colour inside, a dark ring around it so it reads on any background.
const char *kMark = R"(
precision highp float;
uniform vec2 c;
uniform float r;
uniform vec3 color;
varying vec2 px;
void main() {
    float d = length(px - c);
    float a = 1.0 - smoothstep(r - 0.75, r + 0.75, d);
    if (a <= 0.0) discard;
    gl_FragColor = vec4(d > r - 2.0 ? vec3(0.0) : color, a);
})";

unsigned Shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "handcut: shader: %s\n", log);
    }
    return s;
}

unsigned Program(const char *fs, const char *vs = kVertex) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, Shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, Shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "handcut: program: %s\n", log);
        return 0;
    }
    return p;
}

EGLImageKHR ImageFor(EGLDisplay dpy, const ft_dmabuf &b) {
    static const EGLint fd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                 EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint off[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                  EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint pitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                    EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint lo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                 EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint hi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                 EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    EGLint a[64];
    int n = 0;
    a[n++] = EGL_WIDTH, a[n++] = b.width, a[n++] = EGL_HEIGHT, a[n++] = b.height;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT, a[n++] = EGLint(b.format);
    for (int i = 0; i < b.n_planes && i < 4; ++i) {
        a[n++] = fd[i], a[n++] = b.fd[i], a[n++] = off[i], a[n++] = EGLint(b.offset[i]);
        a[n++] = pitch[i], a[n++] = EGLint(b.stride[i]);
        if (b.modifier != DRM_FORMAT_MOD_INVALID) {
            a[n++] = lo[i], a[n++] = EGLint(b.modifier & 0xffffffff);
            a[n++] = hi[i], a[n++] = EGLint(b.modifier >> 32);
        }
    }
    a[n++] = EGL_NONE;
    return pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
}

}  // namespace

Renderer::~Renderer() {
    for (auto &[k, r] : rings_)
        for (Output &o : r.out) FreeOutput(o);
    for (auto &[k, im] : imported_) {
        glDeleteTextures(1, &im.tex);
        pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(im.image));
    }
    if (ctx_) eglDestroyContext(EGLDisplay(dpy_), EGLContext(ctx_));
    if (dpy_) eglTerminate(EGLDisplay(dpy_));
    if (gbm_) gbm_device_destroy(static_cast<gbm_device *>(gbm_));
    if (drm_ >= 0) close(drm_);
}

bool Renderer::Init(const std::vector<uint64_t> &modifiers, std::function<void(const Output *)> released) {
    if (ready_) return true;
    modifiers_ = modifiers;
    released_ = std::move(released);
    drm_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_ < 0) return std::perror("handcut: /dev/dri/renderD128"), false;
    gbm_ = gbm_create_device(drm_);
    pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    pImageTargetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    pCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    pDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    pClientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    if (!gbm_ || !pGetPlatformDisplay || !pCreateImage || !pImageTargetTexture || !pImageTargetRenderbuffer ||
        !pCreateSync || !pDestroySync || !pClientWaitSync) {
        std::fprintf(stderr, "handcut: GBM or EGL extensions missing\n");
        return false;
    }
    EGLDisplay dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return std::fprintf(stderr, "handcut: no EGL display\n"), false;
    dpy_ = dpy;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return std::fprintf(stderr, "handcut: no surfaceless GLES context\n"), false;
    ctx_ = ctx;
    copyProg_ = Program(kCopy);
    cutProg_ = Program(kCut);
    hullProg_ = Program(kHull, kVertexPx);
    glowProg_ = Program(kGlow);
    markProg_ = Program(kMark);
    if (!copyProg_ || !cutProg_ || !hullProg_ || !glowProg_ || !markProg_) return false;
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    ready_ = true;
    return true;
}

unsigned Renderer::Texture(const void *key, const ft_dmabuf &src) {
    auto it = imported_.find(key);
    if (it != imported_.end()) return it->second.tex;
    EGLImageKHR image = ImageFor(EGLDisplay(dpy_), src);
    if (image == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr, "handcut: can't import a %dx%d client buffer (format 0x%x modifier 0x%llx)\n", src.width,
                     src.height, src.format, (unsigned long long)src.modifier);
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
    imported_[key] = {image, tex};
    return tex;
}

void Renderer::Forget(const void *key) {
    auto it = imported_.find(key);
    if (it == imported_.end()) return;
    glDeleteTextures(1, &it->second.tex);
    pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(it->second.image));
    imported_.erase(it);
    for (auto &[k, r] : rings_)   // a new buffer at the same address isn't this one
        for (Output &o : r.out)
            if (o.key == key) o.drawn = false;
}

bool Renderer::MakeOutput(Output &o, int w, int h) {
    auto *gbm = static_cast<gbm_device *>(gbm_);
    std::vector<uint64_t> mods;
    for (uint64_t m : modifiers_)
        if (m != DRM_FORMAT_MOD_INVALID) mods.push_back(m);
    gbm_bo *bo = mods.empty() ? gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
                              : gbm_bo_create_with_modifiers2(gbm, w, h, GBM_FORMAT_ABGR8888, mods.data(),
                                                             unsigned(mods.size()), GBM_BO_USE_RENDERING);
    if (!bo) return std::fprintf(stderr, "handcut: can't allocate a %dx%d output\n", w, h), false;
    o.bo = bo;
    o.buf = {};
    o.buf.width = w, o.buf.height = h;
    o.buf.format = DRM_FORMAT_ABGR8888;
    o.buf.modifier = mods.empty() ? DRM_FORMAT_MOD_LINEAR : gbm_bo_get_modifier(bo);
    o.buf.n_planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < o.buf.n_planes && i < 4; ++i) {
        o.buf.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
        o.buf.offset[i] = gbm_bo_get_offset(bo, i);
        o.buf.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
    }
    EGLImageKHR image = ImageFor(EGLDisplay(dpy_), o.buf);
    if (image == EGL_NO_IMAGE_KHR) return std::fprintf(stderr, "handcut: can't render to the output\n"), FreeOutput(o), false;
    o.image = image;
    glGenRenderbuffers(1, &o.rb);
    glBindRenderbuffer(GL_RENDERBUFFER, o.rb);
    pImageTargetRenderbuffer(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o.rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "handcut: output framebuffer incomplete\n"), FreeOutput(o), false;
    return true;
}

void Renderer::FreeOutput(Output &o) {
    if (o.bo && released_) released_(&o);
    if (o.fence) pDestroySync(EGLDisplay(dpy_), EGLSyncKHR(o.fence));
    if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
    if (o.rb) glDeleteRenderbuffers(1, &o.rb);
    if (o.image) pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(o.image));
    for (int i = 0; i < o.buf.n_planes && i < 4; ++i)
        if (o.buf.fd[i] >= 0) close(o.buf.fd[i]);
    if (o.bo) gbm_bo_destroy(static_cast<gbm_bo *>(o.bo));
    o = Output{};
}

void Renderer::DropPanel(int panel) {
    auto it = rings_.find(panel);
    if (it == rings_.end()) return;
    for (Output &o : it->second.out) FreeOutput(o);
    rings_.erase(it);
}

namespace {

// The pixels a cutout's quad covers (see Draw), as x0 y0 x1 y1 in the eye's half.
float CapsuleFeather(const Capsule2D &c) { return std::max(1.5f, 0.15f * std::min(c.ra, c.rb)); }
constexpr float kHullFeather = 2.0f;
void Bounds(const Capsule2D &c, float b[4]) {
    const float r = std::max(c.ra, c.rb) + CapsuleFeather(c);
    b[0] = std::min(c.ax, c.bx) - r, b[1] = std::min(c.ay, c.by) - r;
    b[2] = std::max(c.ax, c.bx) + r, b[3] = std::max(c.ay, c.by) + r;
}
void Bounds(const Hull2D &h, float b[4]) { std::copy(h.box, h.box + 4, b); }
void Bounds(const Glow &g, float b[4]) {
    const float r = g.r + std::max(1.5f, 0.22f * g.r) + 1;
    b[0] = g.x - r, b[1] = g.y - r, b[2] = g.x + r, b[3] = g.y + r;
}

// Everything drawn on an eye's half, as one box (x0 y0 x1 y1), grown into `box`.
void Grow(const Spots &s, float box[4]) {
    float b[4];
    auto add = [&] {
        box[0] = std::min(box[0], b[0]), box[1] = std::min(box[1], b[1]);
        box[2] = std::max(box[2], b[2]), box[3] = std::max(box[3], b[3]);
    };
    for (const Capsule2D &c : s.caps) Bounds(c, b), add();
    for (const Hull2D &h : s.hulls) Bounds(h, b), add();
    for (const Glow &g : s.glows) Bounds(g, b), add();
}

// Within a quarter pixel (a hundredth of opacity): the same picture.
bool Near(float a, float b) { return std::fabs(a - b) <= 0.25f; }
bool SameSpots(const Spots a[2], const Spots b[2]) {
    for (int e = 0; e < 2; ++e) {
        const Spots &x = a[e], &y = b[e];
        if (x.caps.size() != y.caps.size() || x.hulls.size() != y.hulls.size() || x.glows.size() != y.glows.size())
            return false;
        for (size_t i = 0; i < x.caps.size(); ++i) {
            const Capsule2D &p = x.caps[i], &q = y.caps[i];
            if (!Near(p.ax, q.ax) || !Near(p.ay, q.ay) || !Near(p.bx, q.bx) || !Near(p.by, q.by) || !Near(p.ra, q.ra) ||
                !Near(p.rb, q.rb))
                return false;
        }
        for (size_t i = 0; i < x.hulls.size(); ++i) {
            const Hull2D &p = x.hulls[i], &q = y.hulls[i];
            if (p.n != q.n || !Near(p.r, q.r)) return false;
            for (int k = 0; k < p.n; ++k)   // an edge's normal (a quarter pixel across 1000) and offset
                if (std::fabs(p.e[k][0] - q.e[k][0]) > 2.5e-4f || std::fabs(p.e[k][1] - q.e[k][1]) > 2.5e-4f ||
                    !Near(p.e[k][2], q.e[k][2]))
                    return false;
        }
        for (size_t i = 0; i < x.glows.size(); ++i) {
            const Glow &p = x.glows[i], &q = y.glows[i];
            if (!Near(p.x, q.x) || !Near(p.y, q.y) || !Near(p.r, q.r) || std::fabs(p.alpha - q.alpha) > 0.01f ||
                p.ring != q.ring || std::fabs(p.rgb[0] - q.rgb[0]) + std::fabs(p.rgb[1] - q.rgb[1]) + std::fabs(p.rgb[2] - q.rgb[2]) > 0.01f)
                return false;
        }
    }
    return true;
}

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool Renderer::Passed(Output &o, int64_t timeoutNs) {
    if (!o.fence) return true;
    const EGLint r = pClientWaitSync(EGLDisplay(dpy_), EGLSyncKHR(o.fence), 0, EGLTimeKHR(timeoutNs));
    if (r == EGL_TIMEOUT_EXPIRED_KHR) return false;
    pDestroySync(EGLDisplay(dpy_), EGLSyncKHR(o.fence));   // passed, or failed: don't wait on it again
    o.fence = nullptr;
    return true;
}

// Draws one buffer. Partial: the buffer holds this client frame already, with o.spots drawn,
// so each eye is drawn again only inside the box around those and the new spots.
void Renderer::Draw(Output &o, unsigned tex, int w, int h, const Spots eyes[2], bool partial) {
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        if (partial) {
            float box[4] = {1e9f, 1e9f, -1e9f, -1e9f};
            Grow(o.spots[e], box);
            Grow(eyes[e], box);
            // Window y is the buffer's row, the same way down as the cutouts' y (see kVertex).
            const int x0 = std::clamp(int(std::floor(box[0])) - 1, 0, w), y0 = std::clamp(int(std::floor(box[1])) - 1, 0, h);
            const int x1 = std::clamp(int(std::ceil(box[2])) + 1, 0, w), y1 = std::clamp(int(std::ceil(box[3])) + 1, 0, h);
            if (x1 <= x0 || y1 <= y0) continue;   // nothing in this eye, then or now
            glEnable(GL_SCISSOR_TEST);
            glScissor(e * w + x0, y0, x1 - x0, y1 - y0);
        }
        glViewport(e * w, 0, w, h);
        glDisable(GL_BLEND);
        glUseProgram(copyProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glUniform1i(glGetUniformLocation(copyProg_, "tex"), 0);
        glUniform4f(glGetUniformLocation(copyProg_, "rect"), 0, 0, float(w), float(h));
        glUniform2f(glGetUniformLocation(copyProg_, "size"), float(w), float(h));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // take alpha away where the hand is; the colour stays (straight alpha)
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        float b[4];
        if (!eyes[e].caps.empty()) {
            glUseProgram(cutProg_);
            glUniform2f(glGetUniformLocation(cutProg_, "size"), float(w), float(h));
            const GLint uRect = glGetUniformLocation(cutProg_, "rect"), uA = glGetUniformLocation(cutProg_, "a"),
                        uB = glGetUniformLocation(cutProg_, "b"), uR = glGetUniformLocation(cutProg_, "r"),
                        uF = glGetUniformLocation(cutProg_, "feather");
            for (const Capsule2D &c : eyes[e].caps) {
                Bounds(c, b);
                glUniform4f(uRect, b[0], b[1], b[2], b[3]);
                glUniform2f(uA, c.ax, c.ay);
                glUniform2f(uB, c.bx, c.by);
                glUniform2f(uR, c.ra, c.rb);
                glUniform1f(uF, CapsuleFeather(c));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }
        if (!eyes[e].hulls.empty()) {
            glUseProgram(hullProg_);
            glUniform2f(glGetUniformLocation(hullProg_, "size"), float(w), float(h));
            glUniform1f(glGetUniformLocation(hullProg_, "feather"), kHullFeather);
            const GLint uE = glGetUniformLocation(hullProg_, "edges"), uN = glGetUniformLocation(hullProg_, "n"),
                        uR = glGetUniformLocation(hullProg_, "r");
            // the grown polygon itself, not its box: a forearm across the panel covers a fraction of it
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            for (const Hull2D &hl : eyes[e].hulls) {
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, &hl.fan[0][0]);
                glUniform3fv(uE, hl.n, &hl.e[0][0]);
                glUniform1i(uN, hl.n);
                glUniform1f(uR, hl.r);
                glDrawArrays(GL_TRIANGLE_FAN, 0, hl.n);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
        if (!eyes[e].glows.empty()) {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            glUseProgram(glowProg_);
            glUniform2f(glGetUniformLocation(glowProg_, "size"), float(w), float(h));
            const GLint uRect = glGetUniformLocation(glowProg_, "rect"), uC = glGetUniformLocation(glowProg_, "c"),
                        uR = glGetUniformLocation(glowProg_, "r"), uA = glGetUniformLocation(glowProg_, "alpha"),
                        uRing = glGetUniformLocation(glowProg_, "ring"), uColor = glGetUniformLocation(glowProg_, "color");
            for (const Glow &g : eyes[e].glows) {
                Bounds(g, b);
                glUniform4f(uRect, b[0], b[1], b[2], b[3]);
                glUniform2f(uC, g.x, g.y);
                glUniform1f(uR, g.r);
                glUniform1f(uA, g.alpha);
                glUniform1f(uRing, g.ring ? 1.f : 0.f);
                glUniform3f(uColor, g.rgb[0], g.rgb[1], g.rgb[2]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }
        glDisable(GL_SCISSOR_TEST);
    }
    glDisable(GL_BLEND);
}

const Output *Renderer::Composite(int panel, const void *key, uint64_t serial, const ft_dmabuf &src,
                                  const Spots eyes[2]) {
    if (!ready_) return nullptr;
    const int64_t t0 = SteadyNs();
    const int w = src.width, h = src.height;
    Ring &ring = rings_[panel];
    if (ring.w != w || ring.h != h) {
        for (Output &old : ring.out) FreeOutput(old);
        ring.w = w, ring.h = h, ring.shown = ring.before = ring.drawing = -1;
    }
    // After a pause the panel showed its client buffer, so nothing of ours is on it.
    if (t0 - ring.lastCall > 30'000'000) ring.shown = ring.before = -1;
    ring.lastCall = t0;
    auto promote = [&ring] {
        ring.before = ring.shown, ring.shown = ring.drawing, ring.drawing = -1;
    };
    if (ring.drawing >= 0 && Passed(ring.out[ring.drawing], 0)) promote();

    const int newest = ring.drawing >= 0 ? ring.drawing : ring.shown;
    const Output *n = newest >= 0 ? &ring.out[newest] : nullptr;
    if (n && n->key == key && n->serial == serial && SameSpots(n->spots, eyes)) {
        ++stats_.same;
    } else if (ring.drawing >= 0) {
        ++stats_.busy;   // drawn on a later tick, from what's current then
    } else {
        int i = 0;
        while (i == ring.shown || i == ring.before) ++i;
        Output &o = ring.out[i];
        if (!o.bo && !MakeOutput(o, 2 * w, h)) return nullptr;
        const GLuint tex = Texture(key, src);
        if (!tex) return nullptr;
        const bool partial = o.drawn && o.key == key && o.serial == serial;
        Draw(o, tex, w, h, eyes, partial);
        o.fence = pCreateSync(EGLDisplay(dpy_), EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        if (!o.fence) glFinish();   // no fence: wait here, as before
        o.key = key, o.serial = serial, o.drawn = true;
        for (int e = 0; e < 2; ++e) o.spots[e] = eyes[e];
        ring.drawing = i;
        ++stats_.draws, stats_.partial += partial;
    }
    // Nothing of ours to show yet: wait for this one rather than show none.
    if (ring.shown < 0 && ring.drawing >= 0) {
        ++stats_.waits;
        if (Passed(ring.out[ring.drawing], 50'000'000)) promote();
    }
    lastMs_ = (SteadyNs() - t0) / 1e6;
    stats_.cpuMs += lastMs_, stats_.worstMs = std::max(stats_.worstMs, lastMs_);
    return ring.shown >= 0 ? &ring.out[ring.shown] : nullptr;
}

double Renderer::Finish() {
    if (!ready_) return 0;
    const int64_t t0 = SteadyNs();
    glFinish();
    return (SteadyNs() - t0) / 1e6;
}

const Output *Renderer::Marks(int panel, int w, int h, const std::vector<Mark> eyes[2]) {
    if (!ready_) return nullptr;
    Ring &ring = rings_[panel];
    if (ring.w != w || ring.h != h) {
        for (Output &old : ring.out) FreeOutput(old);
        ring.w = w, ring.h = h, ring.shown = ring.before = ring.drawing = -1;
    }
    auto promote = [&ring] {
        ring.before = ring.shown, ring.shown = ring.drawing, ring.drawing = -1;
    };
    if (ring.drawing >= 0 && Passed(ring.out[ring.drawing], 0)) promote();
    if (ring.drawing < 0) {
        int i = 0;
        while (i == ring.shown || i == ring.before) ++i;
        Output &o = ring.out[i];
        if (!o.bo && !MakeOutput(o, 2 * w, h)) return nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
        glViewport(0, 0, 2 * w, h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDisable(GL_BLEND);
        glUseProgram(markProg_);
        glUniform2f(glGetUniformLocation(markProg_, "size"), float(w), float(h));
        const GLint uRect = glGetUniformLocation(markProg_, "rect"), uC = glGetUniformLocation(markProg_, "c"),
                    uR = glGetUniformLocation(markProg_, "r"), uColor = glGetUniformLocation(markProg_, "color");
        for (int e = 0; e < 2; ++e) {
            glViewport(e * w, 0, w, h);
            for (const Mark &m : eyes[e]) {
                glUniform4f(uRect, m.x - m.r - 1, m.y - m.r - 1, m.x + m.r + 1, m.y + m.r + 1);
                glUniform2f(uC, m.x, m.y);
                glUniform1f(uR, m.r);
                glUniform3f(uColor, m.rgb[0], m.rgb[1], m.rgb[2]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }
        o.fence = pCreateSync(EGLDisplay(dpy_), EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        if (!o.fence) glFinish();
        o.key = nullptr, o.drawn = false;   // holds no client frame
        ring.drawing = i;
    }
    if (ring.shown < 0 && ring.drawing >= 0 && Passed(ring.out[ring.drawing], 50'000'000)) promote();
    return ring.shown >= 0 ? &ring.out[ring.shown] : nullptr;
}

}  // namespace handcut
