// ft-handview: your hands, drawn, when Room View is off. In a VR game, SteamVR Home, the
// dashboard, or Frametop's panels over the void, it draws each tracked hand like the Quest
// does: a light, see-through hand from the tracked skeleton with a brighter rim, fading out
// toward the forearm, over everything. With Room View on, it draws nothing: you see your hands,
// and ft-screens cuts them out of the panels (handcut.h).
//
//   ft-handview [--status S] [--res PX] [--demo] [--seconds S]
//
//   --status S   every S seconds a line to the log: Room View, each hand, renders and their cost
//   --res PX     each eye's half of a hand's buffer, PX square (default 320)
//   --demo       two still, slowly turning hands 45 cm ahead of where your head is at the start,
//                whatever Room View, the tracking or the helper say (trying the look, the cost)
//
// The hands come from ft-hands' shared files, as ft-screens reads them (handcut::Hands, in the
// standing universe, moved ahead to when the frame is on the displays). A hand holding a
// controller isn't drawn: the pointer helper's word ("hands <L> <R> <allowed>" on @ft_handview,
// as on @ft_screens), and while the helper is silent, a hand within 15 cm of its side's controller.
// The hands draw in games too (it's only a picture; hand input has its own rules).
//
// Each hand has an overlay of its own (handmesh.h): a square at the hand, facing the head, side by
// side, one half per eye. A hand is drawn again for each new hands frame, and in between (at most
// every 20 ms) while it moves, with the GPU (EGL on GBM, GLES 2) into buffers SteamVR imports as
// they are (DMA-BUF, IVRIPCResourceManagerClient). Two passes: the body into a scratch buffer
// (shading, the fade, and the depth in its colour), then the rim from it into the overlay's buffer:
// bright where the hand meets the background or a part of it further back (a finger over the palm).
//
// Room View: roomview.h. HANDS_ROOMVIEW=auto|on|off in ~/.config/frametop.conf forces it.
// Commands (datagrams on @ft_handview, replies to the sender):
//   hands <L> <R> <allowed>   the pointer helper's word: each side hand|controller|off
//   roomview auto|on|off      force Room View's state (auto: detect it) -> "ok <on|off> (<why>)"
//   state                     -> "ok <the status line>"
#include "handcut.h"
#include "handmesh.h"
#include "roomview.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using handcut::Mat;

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

double CpuSeconds() {
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

// ------------------------------------------------------------------------------------ GPU

PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
PFNEGLCREATEIMAGEKHRPROC pCreateImage;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pImageTargetRenderbuffer;
PFNEGLCREATESYNCKHRPROC pCreateSync;
PFNEGLDESTROYSYNCKHRPROC pDestroySync;
PFNEGLCLIENTWAITSYNCKHRPROC pClientWaitSync;

// The body: shading (a little darker where the surface turns away), the fade toward the
// forearm, and how far from the eye (from `depth0`, half a metre in 0..1), into the scratch buffer.
const char *kBodyVs = R"(
attribute vec3 pos;
attribute vec3 nrm;
attribute float back;
uniform mat4 mvp;
uniform vec3 eye;
varying vec3 vn;
varying vec3 vv;
varying float vback;
varying float vdist;
void main() {
    gl_Position = mvp * vec4(pos, 1.0);
    vn = nrm;
    vv = eye - pos;
    vback = back;
    vdist = gl_Position.w;
})";
const char *kBodyFs = R"(
precision highp float;
uniform float depth0;
varying vec3 vn;
varying vec3 vv;
varying float vback;
varying float vdist;
void main() {
    float facing = abs(dot(normalize(vn), normalize(vv)));
    float fade = 1.0 - smoothstep(-0.01, 0.10, vback);   // gone by handmesh.cpp kFaded
    gl_FragColor = vec4(0.72 + 0.28 * facing, fade, clamp((vdist - depth0) / 0.5, 0.0, 1.0), 1.0);
})";

// The rim, from the scratch buffer: how close a pixel of the hand is to its outline, or to a
// part of the hand further back (1.5 cm or more), at two radii. Outside the hand: nothing.
const char *kRimVs = R"(
attribute vec2 pos;
uniform vec4 box;   // this eye's half, in texture coordinates: x0 y0 x1 y1
varying vec2 uv;
void main() {
    uv = mix(box.xy, box.zw, pos);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";
const char *kRimFs = R"(
precision highp float;
uniform sampler2D tex;
uniform vec4 box;
uniform vec2 texel;
uniform float rim;        // the rim's width, texels
uniform float body;       // the body's opacity
varying vec2 uv;
vec4 at(vec2 d) { return texture2D(tex, clamp(uv + d * texel, box.xy, box.zw)); }
void main() {
    vec4 c = texture2D(tex, uv);
    if (c.a < 0.5) discard;
    float cov = 2.0, outer = 0.0, inner = 0.0;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        vec2 d = vec2(cos(a), sin(a));
        cov += at(d * 0.7).a;
        vec4 s = at(d * rim);
        outer = max(outer, max(1.0 - s.a, s.a * step(0.03, s.b - c.b)));
        s = at(d * rim * 0.45);
        inner = max(inner, max(1.0 - s.a, s.a * step(0.03, s.b - c.b)));
    }
    float edge = 0.4 * outer + 0.6 * inner;
    float alpha = (cov / 10.0) * c.g * mix(body, 0.95, edge);
    vec3 rgb = mix(vec3(0.86, 0.89, 0.94) * c.r, vec3(1.0), edge);
    gl_FragColor = vec4(rgb, alpha);
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
        std::fprintf(stderr, "handview: shader: %s\n", log);
    }
    return s;
}

unsigned Program(const char *vs, const char *fs, std::initializer_list<const char *> attribs) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, Shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, Shader(GL_FRAGMENT_SHADER, fs));
    GLuint i = 0;
    for (const char *a : attribs) glBindAttribLocation(p, i++, a);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "handview: program: %s\n", log);
        return 0;
    }
    return p;
}

// One buffer SteamVR shows: a GBM buffer object, drawn to through an EGL image.
struct Output {
    gbm_bo *bo = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint rb = 0, fbo = 0;
    vr::SharedTextureHandle_t handle = 0;
    EGLSyncKHR fence = EGL_NO_SYNC_KHR;
    handmesh::Quad quad;   // what it was drawn for
};

class Gpu {
public:
    bool Init(const std::vector<uint64_t> &modifiers, int eyePx);
    bool MakeOutput(Output &o);
    void FreeOutput(Output &o);
    // Draws the mesh for both eyes into o and fences it; returns at once.
    void Draw(Output &o, const handmesh::Mesh &m, const handmesh::Quad &q, const double eyes[2][3]);
    bool Passed(Output &o);
    void Done();

private:
    int drm_ = -1, eye_ = 0;
    gbm_device *gbm_ = nullptr;
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    std::vector<uint64_t> mods_;
    GLuint body_ = 0, rim_ = 0, scratchTex_ = 0, scratchDepth_ = 0, scratch_ = 0, vbo_ = 0, ibo_ = 0, quad_ = 0;
};

bool Gpu::Init(const std::vector<uint64_t> &modifiers, int eyePx) {
    eye_ = eyePx;
    for (uint64_t m : modifiers)
        if (m != DRM_FORMAT_MOD_INVALID) mods_.push_back(m);
    drm_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_ < 0) return std::perror("handview: /dev/dri/renderD128"), false;
    gbm_ = gbm_create_device(drm_);
    pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    pCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    pDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    pClientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    if (!gbm_ || !pGetPlatformDisplay || !pCreateImage || !pDestroyImage || !pImageTargetRenderbuffer || !pCreateSync ||
        !pDestroySync || !pClientWaitSync)
        return std::fprintf(stderr, "handview: GBM or EGL extensions missing\n"), false;
    dpy_ = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
    if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, nullptr, nullptr)) return std::fprintf(stderr, "handview: no EGL display\n"), false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    ctx_ = eglCreateContext(dpy_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx_ == EGL_NO_CONTEXT || !eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_))
        return std::fprintf(stderr, "handview: no surfaceless GLES context\n"), false;
    body_ = Program(kBodyVs, kBodyFs, {"pos", "nrm", "back"});
    rim_ = Program(kRimVs, kRimFs, {"pos"});
    if (!body_ || !rim_) return false;
    // the scratch buffer: both eyes side by side, with depth
    glGenTextures(1, &scratchTex_);
    glBindTexture(GL_TEXTURE_2D, scratchTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2 * eye_, eye_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &scratchDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, scratchDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 2 * eye_, eye_);
    glGenFramebuffers(1, &scratch_);
    glBindFramebuffer(GL_FRAMEBUFFER, scratch_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scratchDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "handview: scratch framebuffer incomplete\n"), false;
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    return true;
}

bool Gpu::MakeOutput(Output &o) {
    const int w = 2 * eye_, h = eye_;
    o.bo = mods_.empty() ? gbm_bo_create(gbm_, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
                         : gbm_bo_create_with_modifiers2(gbm_, w, h, GBM_FORMAT_ABGR8888, mods_.data(), unsigned(mods_.size()),
                                                         GBM_BO_USE_RENDERING);
    if (!o.bo) return std::fprintf(stderr, "handview: can't allocate a %dx%d buffer\n", w, h), false;
    const int planes = gbm_bo_get_plane_count(o.bo);
    const uint64_t mod = mods_.empty() ? DRM_FORMAT_MOD_LINEAR : gbm_bo_get_modifier(o.bo);
    int fds[4] = {-1, -1, -1, -1};
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(w), a.unHeight = uint32_t(h);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = DRM_FORMAT_ABGR8888, a.ulModifier = mod, a.unPlaneCount = uint32_t(planes);
    EGLint at[64];
    int n = 0;
    at[n++] = EGL_WIDTH, at[n++] = w, at[n++] = EGL_HEIGHT, at[n++] = h;
    at[n++] = EGL_LINUX_DRM_FOURCC_EXT, at[n++] = DRM_FORMAT_ABGR8888;
    static const EGLint kFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                  EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint kOff[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                   EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint kPitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                     EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint kLo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint kHi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                  EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (int i = 0; i < planes && i < 4; ++i) {
        fds[i] = gbm_bo_get_fd_for_plane(o.bo, i);
        const uint32_t off = gbm_bo_get_offset(o.bo, i), stride = gbm_bo_get_stride_for_plane(o.bo, i);
        a.plane[i].unOffset = off, a.plane[i].unStride = stride, a.plane[i].nFd = fds[i];
        at[n++] = kFd[i], at[n++] = fds[i], at[n++] = kOff[i], at[n++] = EGLint(off), at[n++] = kPitch[i], at[n++] = EGLint(stride);
        at[n++] = kLo[i], at[n++] = EGLint(mod & 0xffffffff), at[n++] = kHi[i], at[n++] = EGLint(mod >> 32);
    }
    at[n++] = EGL_NONE;
    o.image = pCreateImage(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &o.handle)) o.handle = 0;
    for (int fd : fds)
        if (fd >= 0) close(fd);   // the EGL image and SteamVR hold their own references
    if (o.image == EGL_NO_IMAGE_KHR || !o.handle)
        return std::fprintf(stderr, "handview: the buffer can't be %s\n", o.handle ? "drawn to" : "imported by SteamVR"),
               FreeOutput(o), false;
    glGenRenderbuffers(1, &o.rb);
    glBindRenderbuffer(GL_RENDERBUFFER, o.rb);
    pImageTargetRenderbuffer(GL_RENDERBUFFER, o.image);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o.rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "handview: output framebuffer incomplete\n"), FreeOutput(o), false;
    return true;
}

void Gpu::FreeOutput(Output &o) {
    if (o.handle) vr::VRIPCResourceManager()->UnrefResource(o.handle);
    if (o.fence != EGL_NO_SYNC_KHR) pDestroySync(dpy_, o.fence);
    if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
    if (o.rb) glDeleteRenderbuffers(1, &o.rb);
    if (o.image != EGL_NO_IMAGE_KHR) pDestroyImage(dpy_, o.image);
    if (o.bo) gbm_bo_destroy(o.bo);
    o = Output{};
}

void Gpu::Draw(Output &o, const handmesh::Mesh &m, const handmesh::Quad &q, const double eyes[2][3]) {
    // the body, both eyes
    glBindFramebuffer(GL_FRAMEBUFFER, scratch_);
    glViewport(0, 0, 2 * eye_, eye_);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(body_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.v.size() * sizeof(handmesh::Vertex)), m.v.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(m.idx.size() * sizeof(uint16_t)), m.idx.data(), GL_STREAM_DRAW);
    const GLsizei stride = sizeof(handmesh::Vertex);
    glEnableVertexAttribArray(0), glEnableVertexAttribArray(1), glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, p)));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, n)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, back)));
    const double depth0 = std::max(0.0, q.distance - 0.25);
    glUniform1f(glGetUniformLocation(body_, "depth0"), float(depth0));
    for (int e = 0; e < 2; ++e) {
        float mvp[16];
        handmesh::EyeMatrix(q, eyes[e], 0.05, q.distance + 1.0, mvp);
        glViewport(e * eye_, 0, eye_, eye_);
        glUniformMatrix4fv(glGetUniformLocation(body_, "mvp"), 1, GL_FALSE, mvp);
        glUniform3f(glGetUniformLocation(body_, "eye"), float(eyes[e][0]), float(eyes[e][1]), float(eyes[e][2]));
        glDrawElements(GL_TRIANGLES, GLsizei(m.idx.size()), GL_UNSIGNED_SHORT, nullptr);
    }
    glDisableVertexAttribArray(1), glDisableVertexAttribArray(2);
    glDisable(GL_DEPTH_TEST);
    // the rim, into the output
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glViewport(0, 0, 2 * eye_, eye_);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(rim_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scratchTex_);
    glUniform1i(glGetUniformLocation(rim_, "tex"), 0);
    glUniform2f(glGetUniformLocation(rim_, "texel"), 1.0f / (2 * eye_), 1.0f / eye_);
    glUniform1f(glGetUniformLocation(rim_, "rim"), float(std::clamp(0.004 * eye_ / q.size, 1.5, 8.0)));   // 4 mm
    glUniform1f(glGetUniformLocation(rim_, "body"), 0.32f);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        glViewport(e * eye_, 0, eye_, eye_);
        const float half = 0.5f / (2 * eye_);   // texel centres: the taps stay in this eye's half
        glUniform4f(glGetUniformLocation(rim_, "box"), e * 0.5f + half, 0.5f / eye_, e * 0.5f + 0.5f - half, 1 - 0.5f / eye_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (o.fence != EGL_NO_SYNC_KHR) pDestroySync(dpy_, o.fence);
    o.fence = pCreateSync(dpy_, EGL_SYNC_FENCE_KHR, nullptr);
    glFlush();
    o.quad = q;
}

bool Gpu::Passed(Output &o) {
    if (o.fence == EGL_NO_SYNC_KHR) return true;
    if (pClientWaitSync(dpy_, o.fence, 0, 0) == EGL_TIMEOUT_EXPIRED_KHR) return false;
    pDestroySync(dpy_, o.fence);
    o.fence = EGL_NO_SYNC_KHR;
    return true;
}

void Gpu::Done() {
    if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
    if (dpy_ != EGL_NO_DISPLAY) eglTerminate(dpy_);
    if (gbm_) gbm_device_destroy(gbm_);
    if (drm_ >= 0) close(drm_);
}

// ------------------------------------------------------------------------------- the hands

// Each side's overlay and its three buffers: the one shown, the one shown before it (SteamVR
// may still be reading it), and one being drawn.
struct Side {
    const char *name;
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    Output out[3];
    int shown = -1, before = -1, drawing = -1;
    bool visible = false;
    int64_t lastDraw = 0;
    uint64_t lastFrame = 0;
    double lastPalm[3] = {};
    const char *why = "not tracked";   // why it's hidden
    // since the last status line
    int draws = 0, busy = 0;
    double cpuMs = 0, worstMs = 0, meshMs = 0, gpuMs = 0;
    int64_t drawnAt = 0;
};

// The pointer helper's word (see the top); older than 3 s it lapses.
struct Word {
    int side[2] = {0, 0};   // 0 off, 1 hand, 2 controller
    int64_t atNs = INT64_MIN / 2;
} g_word;

int WordSide(const char *s) { return !std::strcmp(s, "hand") ? 1 : !std::strcmp(s, "controller") ? 2 : 0; }

// Each side's hand controllers (not the pointer helper's own device), looked up once a second.
std::vector<vr::TrackedDeviceIndex_t> g_controllers[2];
int64_t g_controllersAt = INT64_MIN / 2;

// A controller of that side tracked within 15 cm of the palm.
bool ControllerAt(int side, const double palm[3], const vr::TrackedDevicePose_t *poses, int64_t now) {
    if (now - g_controllersAt >= 1'000'000'000) {
        g_controllersAt = now;
        g_controllers[0].clear(), g_controllers[1].clear();
        for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
            const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);
            if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) continue;
            char type[64] = "";
            vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
            if (!std::strcmp(type, "ft_pointer")) continue;
            g_controllers[role == vr::TrackedControllerRole_RightHand].push_back(i);
        }
    }
    for (vr::TrackedDeviceIndex_t i : g_controllers[side]) {
        const auto &p = poses[i];
        if (!p.bPoseIsValid || p.eTrackingResult != vr::TrackingResult_Running_OK) continue;
        const auto &m = p.mDeviceToAbsoluteTracking.m;
        const double dx = m[0][3] - palm[0], dy = m[1][3] - palm[1], dz = m[2][3] - palm[2];
        if (dx * dx + dy * dy + dz * dz < 0.15 * 0.15) return true;
    }
    return false;
}

// --demo: an open hand, fingers up and the palm toward you, 45 cm ahead of `head` and 12 cm to
// its side, turning slowly about its arm.
handcut::Hand DemoHand(const Mat &head, bool right, double t) {
    static const float kP[21][3] = {{0, 0, 0},           {-0.025f, 0.02f, 0},  {-0.045f, 0.04f, 0}, {-0.058f, 0.06f, 0},
                                    {-0.068f, 0.08f, 0}, {-0.022f, 0.085f, 0}, {-0.025f, 0.125f, 0}, {-0.027f, 0.15f, 0},
                                    {-0.028f, 0.17f, 0}, {0, 0.088f, 0},       {0, 0.132f, 0},       {0, 0.16f, 0},
                                    {0, 0.182f, 0},      {0.02f, 0.083f, 0},   {0.023f, 0.122f, 0},  {0.025f, 0.148f, 0},
                                    {0.026f, 0.168f, 0}, {0.038f, 0.075f, 0},  {0.043f, 0.103f, 0},  {0.046f, 0.12f, 0},
                                    {0.048f, 0.136f, 0}};
    handcut::Hand h;
    h.id = right ? 2 : 1, h.right = right, h.stereo = true, h.fit = true, h.confidence = 1;
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]);   // the head's heading, level
    const double fx = -std::sin(yaw), fz = -std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    const double side = right ? 1 : -1, turn = 0.5 * std::sin(t * 0.8), curl = 0.5 + 0.5 * std::sin(t * 1.3);
    const double c[3] = {head.m[0][3] + 0.45 * fx + 0.12 * side * rx, head.m[1][3] - 0.25, head.m[2][3] + 0.45 * fz + 0.12 * side * rz};
    auto place = [&](double x, double y, double z, float out[3]) {
        // the hand's frame: x to its side (mirrored for the left), y up its fingers, z toward you;
        // turned by `turn` about y
        const double lx = side * (x * std::cos(turn) + z * std::sin(turn)), lz = -x * std::sin(turn) + z * std::cos(turn);
        out[0] = float(c[0] + lx * rx - lz * fx), out[1] = float(c[1] + y), out[2] = float(c[2] + lx * rz - lz * fz);
    };
    for (int j = 0; j < 21; ++j) {
        double y = kP[j][1], z = kP[j][2];
        if (j > 5 && j % 4 != 1) {   // the fingers' outer joints curl toward you
            const int base = (j - 1) / 4 * 4 + 1;
            const double over = kP[j][1] - kP[base][1];
            y = kP[base][1] + over * std::cos(curl * 0.8), z = over * std::sin(curl * 0.8);
        }
        place(kP[j][0], y, z, h.p[j]);
        h.radius[j] = 0.008f;
    }
    for (int b = 0; b < 20; ++b) h.bone[b] = 0.03f;
    place(0, -0.26, 0.04, h.elbow);
    h.hasElbow = true, h.elbowConf = 1;
    std::memcpy(h.pinchPoint, h.p[9], sizeof h.pinchPoint);
    std::memcpy(h.contact, h.p[8], sizeof h.contact);
    h.pinchD[0] = h.pinchD[1] = 0.08f;
    h.v[0] = h.v[1] = h.v[2] = 0;
    return h;
}

}  // namespace

int main(int argc, char **argv) {
    double statusS = 0, seconds = 0;
    int res = 320;
    bool demo = false;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--status") && more) statusS = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--res") && more) res = std::clamp(std::atoi(argv[++i]), 64, 1024);
        else if (!std::strcmp(argv[i], "--seconds") && more) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--demo")) demo = true;
        else return std::fprintf(stderr, "usage: ft-handview [--status S] [--res PX] [--demo] [--seconds S]\n"), 2;
    }
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const char *name = "ft_handview";
        std::memcpy(addr.sun_path + 1, name, std::strlen(name));
        if (bind(sock, reinterpret_cast<sockaddr *>(&addr), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name))) != 0)
            return std::perror("bind @ft_handview (already running?)"), 1;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) return std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err)), 1;
    if (!vr::VRIPCResourceManager()) return std::fprintf(stderr, "openvr: no IVRIPCResourceManagerClient\n"), 1;
    uint64_t mods[64];
    uint32_t nmods = 64;
    if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, DRM_FORMAT_ABGR8888, &nmods, mods)) nmods = 0;
    Gpu gpu;
    if (!gpu.Init(std::vector<uint64_t>(mods, mods + nmods), res)) return 1;

    Side sides[2] = {{"left"}, {"right"}};
    for (Side &s : sides) {
        const std::string key = std::string("frametop.handview.") + s.name;
        if (vr::VROverlay()->CreateOverlay(key.c_str(), (std::string("Hand (") + s.name + ")").c_str(), &s.overlay) !=
            vr::VROverlayError_None)
            return std::fprintf(stderr, "can't create overlay %s (already running?)\n", key.c_str()), 1;
        vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SideBySide_Parallel, true);
        vr::VROverlay()->SetOverlaySortOrder(s.overlay, 100);   // over the panels (5-21) and their controls
        for (Output &o : s.out)
            if (!gpu.MakeOutput(o)) return 1;
    }

    roomview::Detector room;
    room.SetLog(true);
    roomview::Force force = roomview::Force::Auto;
    if (!roomview::ParseForce(handcut::Setting("HANDS_ROOMVIEW", "auto"), &force))
        std::fprintf(stderr, "HANDS_ROOMVIEW: auto, on or off; taking auto\n");
    room.SetForce(force);
    handcut::Hands hands;
    handmesh::Mesh mesh;
    std::printf("ft-handview: %dx%d per eye per hand%s; Room View %s\n", res, res, demo ? ", demo hands" : "",
                force == roomview::Force::Auto ? "detected" : roomview::Name(force));

    bool scene = false, constructRoomView = true;
    int64_t nextScene = 0, nextSettings = 0;
    const int64_t start = MonoNs();
    int64_t lastStatus = start;
    double cpu0 = CpuSeconds();
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    Mat demoHead{};
    bool haveDemoHead = false;
    std::string statusLine = "starting";

    while (!g_stop && (seconds <= 0 || (MonoNs() - start) / 1e9 < seconds)) {
        const auto tick = std::chrono::steady_clock::now();
        const int64_t now = MonoNs();
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
            if (ev.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                g_stop = 1;
            }
            room.Event(ev.eventType);
        }
        // commands
        char buf[256];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
            char l[16], r[16], word[16];
            int allowed;
            std::string reply;
            if (std::sscanf(buf, "hands %15s %15s %d", l, r, &allowed) == 3) {
                g_word.side[0] = WordSide(l), g_word.side[1] = WordSide(r), g_word.atNs = now;
            } else if (std::sscanf(buf, "roomview %15s", word) == 1) {
                roomview::Force f;
                if (!roomview::ParseForce(word, &f)) reply = "error roomview auto|on|off";
                else {
                    room.SetForce(f);
                    room.Update(now, scene, constructRoomView);
                    reply = std::string("ok ") + (room.on() ? "on" : "off") + " (" + room.why() + ")";
                }
            } else if (!std::strcmp(buf, "state")) {
                reply = "ok " + statusLine;
            } else reply = "error hands <L> <R> <allowed>|roomview auto|on|off|state";
            if (!reply.empty() && fromLen > offsetof(sockaddr_un, sun_path))
                sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
        // Room View
        if (now >= nextScene) nextScene = now + 500'000'000, scene = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
        if (now >= nextSettings) {
            nextSettings = now + 5'000'000'000;
            vr::EVRSettingsError se;
            const bool v = vr::VRSettings()->GetBool("camera", "enableConstructRoomView", &se);
            constructRoomView = se == vr::VRSettingsError_None ? v : true;
        }
        const bool roomOn = room.Update(now, scene, constructRoomView);

        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const Mat &head = hp.mDeviceToAbsoluteTracking;
        std::vector<handcut::Hand> shown;
        uint64_t frame = 0;
        if (demo) {
            if (!haveDemoHead && hp.bPoseIsValid) demoHead = head, haveDemoHead = true;
            if (haveDemoHead) {
                const double t = (now - start) / 1e9;
                shown = {DemoHand(demoHead, false, t), DemoHand(demoHead, true, t)};
                frame = uint64_t(t * 30);   // a "hands frame" 30 times a second
            }
        } else if (hp.bPoseIsValid && !roomOn && hands.Update(head, now)) {
            shown = hands.shown();
            frame = hands.frame();
        }
        double eyes[2][3];
        if (hp.bPoseIsValid) handcut::EyePositions(head, eyes);
        const double headPos[3] = {head.m[0][3], head.m[1][3], head.m[2][3]};
        const bool heard = now - g_word.atNs < 3'000'000'000;

        for (int si = 0; si < 2; ++si) {
            Side &s = sides[si];
            const handcut::Hand *h = nullptr;
            for (const handcut::Hand &c : shown)
                if (c.right == bool(si)) h = &c;
            double palm[3] = {};
            if (h) h->Palm(palm);
            const char *why = nullptr;
            if (!hp.bPoseIsValid) why = "no head pose";
            else if (roomOn && !demo) why = "Room View on";
            else if (!h) why = "not tracked";
            else if (demo) why = nullptr;
            else if (heard ? g_word.side[si] == 2 : ControllerAt(si, palm, poses, now)) why = heard ? "controller (helper)" : "controller near";
            // a finished buffer goes up, with the square it was drawn for (shown unless the hand went)
            if (s.drawing >= 0 && gpu.Passed(s.out[s.drawing])) {
                Output &o = s.out[s.drawing];
                vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(o.quad.size));
                vr::VROverlay()->SetOverlayTransformAbsolute(s.overlay, vr::TrackingUniverseStanding, &o.quad.pose);
                vr::Texture_t tex = {&o.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(s.overlay, &tex);
                s.before = s.shown, s.shown = s.drawing, s.drawing = -1;
                s.gpuMs += (MonoNs() - s.drawnAt) / 1e6;
                if (!s.visible && !why) vr::VROverlay()->ShowOverlay(s.overlay), s.visible = true;
            }
            if (why) {
                s.why = why;
                if (s.visible) vr::VROverlay()->HideOverlay(s.overlay), s.visible = false;
                continue;
            }
            s.why = "shown";
            // draw on a new hands frame, or every 20 ms while it moves (3 mm or more)
            const double moved = std::sqrt((palm[0] - s.lastPalm[0]) * (palm[0] - s.lastPalm[0]) +
                                           (palm[1] - s.lastPalm[1]) * (palm[1] - s.lastPalm[1]) +
                                           (palm[2] - s.lastPalm[2]) * (palm[2] - s.lastPalm[2]));
            const bool want = frame != s.lastFrame || (moved > 0.003 && now - s.lastDraw >= 20'000'000) || !s.visible;
            if (!want) continue;
            if (s.drawing >= 0) {   // the GPU is still on the last one
                s.busy += s.visible;
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            handmesh::Build(*h, headPos, mesh);
            handmesh::Quad q;
            if (!handmesh::Place(mesh, eyes, 0.04, q)) continue;
            s.meshMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            int pick = 0;
            while (pick == s.shown || pick == s.before) ++pick;
            gpu.Draw(s.out[pick], mesh, q, eyes);
            s.drawing = pick, s.drawnAt = MonoNs();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++s.draws, s.cpuMs += ms, s.worstMs = std::max(s.worstMs, ms);
            s.lastFrame = frame, s.lastDraw = now;
            std::memcpy(s.lastPalm, palm, sizeof palm);
        }

        if (now - lastStatus >= int64_t((statusS > 0 ? statusS : 2) * 1e9)) {
            const double dt = (now - lastStatus) / 1e9, cpu1 = CpuSeconds();
            char line[640];
            int k = std::snprintf(line, sizeof line, "room view %s (%s), helper %s; CPU %.1f%%", roomOn ? "on" : "off",
                                  room.why(), heard ? "heard" : "silent", 100 * (cpu1 - cpu0) / dt);
            for (Side &s : sides)
                k += std::snprintf(line + k, sizeof line - k, "; %s %s, %.1f draws/s, CPU %.2f ms avg (mesh %.2f) %.2f worst, up after %.1f ms, %d busy",
                                   s.name, s.why, s.draws / dt, s.draws ? s.cpuMs / s.draws : 0.0,
                                   s.draws ? s.meshMs / s.draws : 0.0, s.worstMs,
                                   s.draws ? s.gpuMs / s.draws : 0.0, s.busy);
            statusLine = line;
            if (statusS > 0) std::printf("%s\n", line);
            for (Side &s : sides) s.draws = s.busy = 0, s.cpuMs = s.worstMs = s.meshMs = s.gpuMs = 0;
            lastStatus = now, cpu0 = cpu1;
        }
        // about the displays' rate while a hand shows; else 20 times a second, and 10 with Room View
        // on (nothing to draw: a hand shows up a little later, for less waking)
        const bool busy = sides[0].visible || sides[1].visible || sides[0].drawing >= 0 || sides[1].drawing >= 0;
        std::this_thread::sleep_until(tick + std::chrono::microseconds(busy ? 11111 : roomOn && !demo ? 100000 : 50000));
    }
    for (Side &s : sides) {
        vr::VROverlay()->DestroyOverlay(s.overlay);
        for (Output &o : s.out) gpu.FreeOutput(o);
    }
    gpu.Done();
    vr::VR_Shutdown();
    close(sock);
    return 0;
}
