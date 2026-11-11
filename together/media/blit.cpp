#include "blit.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <cstdio>

namespace media {

namespace {

PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
PFNEGLCREATEIMAGEKHRPROC pCreateImage;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pImageTargetRenderbuffer;

// Memory row 0 is y = 0 on both sides, so nothing is flipped.
const char *kVertex = R"(
attribute vec2 pos;
varying vec2 uv;
void main() {
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";

// A screen is opaque whatever its buffer's alpha says (KWin's are XRGB or ARGB).
const char *kFragment = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
uniform samplerExternalOES tex;
varying vec2 uv;
void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); })";

GLuint Shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok) return s;
    glDeleteShader(s);
    return 0;
}

EGLImageKHR Image(EGLDisplay dpy, const Dmabuf &d) {
    static const EGLint kFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint kOffset[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint kPitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint kLo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint kHi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    EGLint a[64];
    int n = 0;
    a[n++] = EGL_WIDTH, a[n++] = EGLint(d.width), a[n++] = EGL_HEIGHT, a[n++] = EGLint(d.height);
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT, a[n++] = EGLint(d.format);
    for (int i = 0; i < d.planes && i < 4; ++i) {
        a[n++] = kFd[i], a[n++] = d.fd[i], a[n++] = kOffset[i], a[n++] = EGLint(d.offset[i]);
        a[n++] = kPitch[i], a[n++] = EGLint(d.stride[i]);
        if (d.modifier != DRM_FORMAT_MOD_INVALID) {
            a[n++] = kLo[i], a[n++] = EGLint(d.modifier & 0xffffffff);
            a[n++] = kHi[i], a[n++] = EGLint(d.modifier >> 32);
        }
    }
    a[n++] = EGL_NONE;
    return pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
}

}  // namespace

bool Blit::Open(const Pictures &pictures, std::string &err) {
    Close();
    pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    pImageTargetRenderbuffer =
        reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!pictures.device() || !pGetPlatformDisplay || !pCreateImage || !pDestroyImage || !pImageTargetTexture || !pImageTargetRenderbuffer)
        return err = "GBM or EGL extensions missing", false;
    EGLDisplay dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, pictures.device(), nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return err = "no EGL display", false;
    dpy_ = dpy;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return err = "no surfaceless GLES context", Close(), false;
    ctx_ = ctx;

    const GLuint vs = Shader(GL_VERTEX_SHADER, kVertex), fs = Shader(GL_FRAGMENT_SHADER, kFragment);
    program_ = glCreateProgram();
    glAttachShader(program_, vs), glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "pos");
    glLinkProgram(program_);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs), glDeleteShader(fs);
    if (!vs || !fs || !ok) return err = "the blit's shader doesn't build", Close(), false;
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    width_ = pictures.width(), height_ = pictures.height();
    for (int i = 0; i < Pictures::kCount; ++i) {
        Dmabuf d;
        d.width = uint32_t(width_), d.height = uint32_t(height_), d.format = DRM_FORMAT_ABGR8888, d.modifier = DRM_FORMAT_MOD_LINEAR;
        d.planes = 1, d.fd[0] = pictures.fd(i), d.stride[0] = pictures.stride();
        EGLImageKHR image = Image(dpy, d);
        if (image == EGL_NO_IMAGE_KHR) return err = "EGL can't import the encoder's pictures", Close(), false;
        target_[i] = image;
        glGenRenderbuffers(1, &rb_[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_[i]);
        pImageTargetRenderbuffer(GL_RENDERBUFFER, image);
        glGenFramebuffers(1, &fbo_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_[i]);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return err = "the GPU can't draw into the encoder's pictures", Close(), false;
    }
    return true;
}

void Blit::Drop(From &f) {
    if (f.texture) glDeleteTextures(1, &f.texture);
    if (f.image) pDestroyImage(dpy_, f.image);
    f = From{};
}

void Blit::Close() {
    if (ctx_) {
        for (auto &[id, f] : from_) Drop(f);
        for (int i = 0; i < Pictures::kCount; ++i) {
            if (fbo_[i]) glDeleteFramebuffers(1, &fbo_[i]);
            if (rb_[i]) glDeleteRenderbuffers(1, &rb_[i]);
            if (target_[i]) pDestroyImage(dpy_, target_[i]);
            fbo_[i] = rb_[i] = 0, target_[i] = nullptr;
        }
        if (quad_) glDeleteBuffers(1, &quad_);
        if (program_) glDeleteProgram(program_);
        eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy_, ctx_);
    }
    from_.clear();
    if (dpy_) eglTerminate(dpy_);
    dpy_ = ctx_ = nullptr, program_ = quad_ = 0, width_ = height_ = 0;
}

bool Blit::Import(uint32_t id, const Dmabuf &from, std::string &err) {
    if (!ctx_) return err = "not open", false;
    if (from.planes < 1 || from.planes > 4 || from.width < 1 || from.height < 1 || from.width > 16384 || from.height > 16384)
        return err = "not a buffer", false;
    Forget(id);
    if (from_.size() >= kMostBuffers) return err = "too many buffers", false;
    From f;
    f.image = Image(dpy_, from);
    if (f.image == EGL_NO_IMAGE_KHR) {
        char why[64];
        std::snprintf(why, sizeof why, "EGL can't import it (0x%x)", unsigned(eglGetError()));
        return err = why, false;
    }
    glGenTextures(1, &f.texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, f.texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, f.image);
    if (glGetError() != GL_NO_ERROR) return Drop(f), err = "the GPU can't sample it", false;
    from_[id] = f;
    return true;
}

void Blit::Forget(uint32_t id) {
    const auto it = from_.find(id);
    if (it == from_.end()) return;
    Drop(it->second);
    from_.erase(it);
}

bool Blit::Draw(uint32_t id, int into) {
    const auto it = from_.find(id);
    if (!ctx_ || it == from_.end() || into < 0 || into >= Pictures::kCount) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[into]);
    glViewport(0, 0, width_, height_);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, it->second.texture);
    glUniform1i(glGetUniformLocation(program_, "tex"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    return glGetError() == GL_NO_ERROR;
}

}  // namespace media
