// ft-handview: your hands, drawn. In a VR game, SteamVR Home, the dashboard, Frametop's panels
// over the void, and (HANDS_VIEW_ROOMVIEW=1, the default) with Room View on too, it draws each
// tracked hand over everything: a 3D hand model posed by the tracked skeleton (handskin.h; which
// model, HANDS_VIEW_MODEL: handasset.h's), in one of two looks (HANDS_VIEW_STYLE): ghost, light and
// see-through, glowing toward its edges like the Quest's, or solid, a matte light hand lit from
// above with a soft rim light. With Room View on, ft-screens also cuts a hole for each hand in the
// panels (handcut.h), and the drawn hand floats over the real one. HANDS_VIEW_MODEL=robot draws a
// robot hand built in code from the skeleton (handrobot.h), capsules the older hand of tubes from
// the matte's pieces (handmesh.h).
//
//   ft-handview [--status S] [--res PX] [--demo] [--seconds S]
//   ft-handview --snapshot FILE.png [--res PX] [--seconds S]
//
//   --status S   every S seconds a line to the log: Room View, each hand, renders and their cost
//   --res PX     each eye's half of a hand's buffer, PX square (default 320)
//   --demo       two slowly opening and turning hands 45 cm ahead of where your head is at the start,
//                whatever Room View, the tracking or the helper say (trying the look, the cost)
//   --snapshot   no SteamVR: draws demo poses (open, palm away, palm up, fist, point, pinch, spread)
//                of both hands offscreen, each eye's half, into a PNG (left hands over dark, right
//                over light), then draws both hands 30 times a second for S seconds (default 5) and
//                prints the CPU it took, per draw and of one core, and the wait for the GPU after
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
// (with depth, so only the surface nearest the eye: how squarely it faces the eye, the fade, the
// light from above, the fingertips), the model's wrist end capped there (kCapFs), then the colour
// from it into the overlay's buffer: a soft fresnel rim where the surface turns away, the
// fingertips' glow, and the outline smoothed.
//
// Room View: roomview.h. HANDS_ROOMVIEW=auto|on|off in ~/.config/frametop.conf forces it.
// Settings (~/.config/frametop.conf): HANDS_VIEW_MODEL asset|babylon|mrtk3|godot|mrtk2|robot|capsules
// (screens/assets/hands/README.md), HANDS_VIEW_STYLE ghost|solid,
// HANDS_VIEW_ROOMVIEW 1|0 (draw with Room View on too).
// Commands (datagrams on @ft_handview, replies to the sender):
//   hands <L> <R> <allowed>   the pointer helper's word: each side hand|controller|off
//   roomview auto|on|off      force Room View's state (auto: detect it) -> "ok <on|off> (<why>)"
//   model <name>|robot|capsules  the hand model, style ghost|solid its look, inroom 1|0 drawing with
//                             Room View on (live, until a restart) -> "ok"
//   state                     -> "ok <the status line>"
//   flash                     both drawn hands pulse cyan for half a second: a flag was taken
//                             (`ft-handsctl flag`, hands/README.md) -> "ok <hands shown>"
//   poses <S>                 the head's poses over the last S seconds (at most 20; kept 30 times a
//                             second) -> "ok" and a line each: "<CLOCK_MONOTONIC ns> <3x4 row-major,
//                             standing universe>" (ft-handsctl saves them with a flag)
#include "handcut.h"
#include "handmesh.h"
#include "handskin.h"
#include "handasset.h"
#include "handrobot.h"
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

// The body: how squarely the surface faces the eye (the second pass's fresnel), the fade toward
// the forearm, the light from above, and how near a fingertip, into the scratch buffer. Only the
// surface nearest the eye lands there (depth), so a see-through hand shows its front and nothing
// inside it. The capsules' mesh comes posed (handmesh.h); the model is skinned here (kSkinVs).
const char *kBodyVs = R"(
attribute vec3 pos;
attribute vec3 nrm;
attribute float back;
uniform mat4 mvp;
uniform vec3 eye;
uniform vec3 tips[5];
varying vec3 vn;
varying vec3 vv;
varying float vback;
varying float vtip;
float tipness(vec3 p) {
    float d = 1.0;
    for (int i = 0; i < 5; ++i) d = min(d, distance(p, tips[i]));
    return 1.0 - smoothstep(0.003, 0.016, d);
}
void main() {
    gl_Position = mvp * vec4(pos, 1.0);
    vn = nrm;
    vv = eye - pos;
    vback = back;
    vtip = tipness(pos);
})";
// The model (handasset.h), skinned: each vertex moved by up to four of its joints' matrices
// (handskin::Pose, model -> room), blended by its weights. `back` is worked out here: how far
// behind the tracked wrist, along the hand.
const char *kSkinVs = R"(
attribute vec3 pos;       // the model's units (kPosStep)
attribute vec3 nrm;
attribute vec4 joints;
attribute vec4 weights;
uniform vec4 bones[75];   // 25 joints' 3x4 matrices, a row each
uniform float posStep;
uniform mat4 mvp;
uniform vec3 eye;
uniform vec3 wrist;
uniform vec3 fwd;
uniform vec3 tips[5];
varying vec3 vn;
varying vec3 vv;
varying float vback;
varying float vtip;
vec3 apply(float j, vec4 p) {
    int k = int(j + 0.5) * 3;
    return vec3(dot(bones[k], p), dot(bones[k + 1], p), dot(bones[k + 2], p));
}
float tipness(vec3 p) {
    float d = 1.0;
    for (int i = 0; i < 5; ++i) d = min(d, distance(p, tips[i]));
    return 1.0 - smoothstep(0.003, 0.016, d);
}
void main() {
    vec4 p = vec4(pos * posStep, 1.0), n = vec4(nrm, 0.0);
    vec3 sp = weights.x * apply(joints.x, p) + weights.y * apply(joints.y, p) + weights.z * apply(joints.z, p) +
              weights.w * apply(joints.w, p);
    vec3 sn = weights.x * apply(joints.x, n) + weights.y * apply(joints.y, n) + weights.z * apply(joints.z, n) +
              weights.w * apply(joints.w, n);
    gl_Position = mvp * vec4(sp, 1.0);
    vn = sn;
    vv = eye - sp;
    vback = dot(wrist - sp, fwd);
    vtip = tipness(sp);
})";
// The robot (handrobot.h): each vertex moved by its segment's matrix, its end-fixed ones along
// the bone by its length (the shell's middle takes it up), the x and y scales on the palm.
const char *kRobotVs = R"(
attribute vec3 pos;    // in its segment's frame (handrobot::Vertex)
attribute vec3 nrm;
attribute vec3 info;   // its segment, 1 if fixed to the bone's end, its material
uniform vec4 segs[64]; // 16 segments: a 3x4 matrix (a row each), then (length, x scale, y scale, 0)
uniform mat4 mvp;
uniform vec3 eye;
varying vec3 vn;
varying vec3 vv;
varying float vmat;
void main() {
    int k = int(info.x + 0.5) * 4;
    vec4 g = segs[k + 3];
    vec4 l = vec4(pos.x * g.y, pos.y * g.z, pos.z + info.y * g.x, 1.0);
    vec3 p = vec3(dot(segs[k], l), dot(segs[k + 1], l), dot(segs[k + 2], l));
    vec4 n = vec4(nrm.x / g.y, nrm.y / g.z, nrm.z, 0.0);
    vn = vec3(dot(segs[k], n), dot(segs[k + 1], n), dot(segs[k + 2], n));
    vv = eye - p;
    vmat = info.z;
    gl_Position = mvp * vec4(p, 1.0);
})";
// Its body: r how squarely it faces the eye, g a highlight (a light above, a little to the eye's
// side), b the light from above (wrapped), a its material (0.5 shell, 0.625 joint, 0.75 accent,
// 0.875 line). Nothing fades: the cuff is closed.
const char *kRobotFs = R"(
precision highp float;
varying vec3 vn;
varying vec3 vv;
varying float vmat;
void main() {
    vec3 n = normalize(vn), v = normalize(vv);
    vec3 h = normalize(normalize(vec3(0.25, 1.0, 0.35)) + v);
    float spec = pow(max(dot(n, h), 0.0), 28.0);
    gl_FragColor = vec4(abs(dot(n, v)), spec, 0.5 + 0.5 * n.y, 0.5 + 0.125 * floor(vmat + 0.5));
})";
// Out: r how squarely it faces the eye, g the fade (1 drawn, 0 gone), b the light from above,
// a 0.5 and up where there's hand, up to 1 at a fingertip.
const char *kBodyFs = R"(
precision highp float;
uniform vec2 fade;   // vback where the fade starts and where it's gone (nothing past that)
uniform float open;  // 1: the mesh is open at the wrist (the model)
varying vec3 vn;
varying vec3 vv;
varying float vback;
varying float vtip;
void main() {
    if (vback > fade.y) discard;
    vec3 n = normalize(vn), v = normalize(vv);
    float facing = abs(dot(n, v));
    // above (the room's up), wrapped round so the underside is dimmer, not dark
    float light = 0.5 + 0.5 * (gl_FrontFacing ? n.y : -n.y);
    float f = 1.0 - smoothstep(fade.x, fade.y, vback);
    // the model's inside, seen through its open wrist end: clear (the cap pass covers it)
    if (!gl_FrontFacing) f *= 1.0 - open;
    gl_FragColor = vec4(facing, f, light, 0.5 + 0.5 * vtip);
})";

// The model's open wrist end, capped: where the eye looks into the hand through the cut the
// fade makes (the stencil counts the hand's surfaces along each view: +1 each front face, -1 each
// back face, so a view in through the cut comes out below 0), the cut plane's disc, its middle
// softly there and its edge gone as the wrist around it. Without it a hand pointing away shows
// its inside: a hollow tube, and the fingertips of a fist through the palm.
const char *kCapVs = R"(
attribute vec2 pos;
varying vec2 ndc;
void main() {
    ndc = pos * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
})";
const char *kCapFs = R"(
precision highp float;
uniform mat4 inv;      // clip space -> the room (this eye's)
uniform vec3 centre;   // the cut's middle, its normal (along the hand)
uniform vec3 axis;
uniform vec3 u;        // and its ellipse's axes, over their radii
uniform vec3 v;
uniform vec2 look;     // r: how squarely it faces the eye, g: its opacity in the middle
varying vec2 ndc;
void main() {
    vec4 a = inv * vec4(ndc, -1.0, 1.0), b = inv * vec4(ndc, 1.0, 1.0);
    vec3 pa = a.xyz / a.w, d = b.xyz / b.w - pa;
    vec3 x = pa + d * (dot(centre - pa, axis) / dot(d, axis)) - centre;
    float r = length(vec2(dot(x, u), dot(x, v)));
    gl_FragColor = vec4(look.x, look.y * (1.0 - smoothstep(0.6, 1.1, r)), 0.75, 0.5);
})";

// The second pass, from the scratch buffer: the hand's colour and opacity, a soft fresnel rim
// where its surface turns away from the eye, a glow at the fingertips, and the outline's
// antialiasing (how much of the pixel's neighbourhood is hand). Outside the hand: nothing.
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
uniform float body;       // the body's opacity, facing the eye
uniform vec3 base;        // its colour
uniform float shade;      // how much darker away from the light
uniform float fres;       // how strong the rim (fresnel)
uniform float rimPow;     // and how tight to the edges
uniform vec3 glow;        // the rim's colour
uniform float tip;        // how much the fingertips glow
varying vec2 uv;
float hand(vec2 d) { return step(0.25, texture2D(tex, clamp(uv + d * texel, box.xy, box.zw)).a); }
void main() {
    vec4 c = texture2D(tex, uv);
    if (c.a < 0.25) discard;
    float cov = 2.0;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        cov += hand(vec2(cos(a), sin(a)) * 0.7);
    }
    float rim = fres * pow(1.0 - c.r, rimPow);
    float t = tip * clamp(c.a * 2.0 - 1.0, 0.0, 1.0);
    float edge = clamp(rim + t, 0.0, 1.0);
    vec3 rgb = mix(base * (1.0 - shade + shade * c.b), glow, edge);
    float alpha = (cov / 10.0) * c.g * mix(body, 0.95, edge);
    gl_FragColor = vec4(rgb, alpha);
})";

// The robot's second pass: each material's colour and opacity, the shells and joints lit (and
// shining), the accents glowing, the same fresnel rim and outline as the models'.
const char *kRobotRimFs = R"(
precision highp float;
uniform sampler2D tex;
uniform vec4 box;
uniform vec2 texel;
uniform vec3 shell;      // the shells' colour
uniform vec3 dark;       // the joints' and the core's
uniform vec3 accent;     // the glowing accents'
uniform vec4 opacity;    // each material's: shell, joint, accent, line
uniform vec2 shine;      // the highlight's strength: shell, joint
uniform float shade;     // how much darker away from the light
uniform float fres;      // the rim
uniform float rimPow;
uniform vec3 glow;
varying vec2 uv;
float hand(vec2 d) { return step(0.25, texture2D(tex, clamp(uv + d * texel, box.xy, box.zw)).a); }
void main() {
    vec4 c = texture2D(tex, uv);
    if (c.a < 0.25) discard;
    float cov = 2.0;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        cov += hand(vec2(cos(a), sin(a)) * 0.7);
    }
    float m = floor((c.a - 0.5) * 8.0 + 0.5);
    float lit = 1.0 - shade + shade * c.b;
    vec3 rgb;
    float op, rim = fres * pow(1.0 - c.r, rimPow);
    if (m < 0.5) rgb = shell * lit + shine.x * c.g, op = opacity.x;
    else if (m < 1.5) rgb = dark * lit + shine.y * c.g, op = opacity.y;
    else if (m < 2.5) rgb = accent * (0.85 + 0.25 * c.r) + 0.15 * c.g, op = opacity.z, rim = 0.0;
    else rgb = mix(dark, shell, 0.3) * lit, op = opacity.w;
    gl_FragColor = vec4(mix(rgb, glow, rim), (cov / 10.0) * mix(op, 0.95, clamp(rim, 0.0, 1.0)));
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
    GLuint rb = 0, fbo = 0, tex = 0;   // tex: an offscreen one's (--snapshot)
    vr::SharedTextureHandle_t handle = 0;
    EGLSyncKHR fence = EGL_NO_SYNC_KHR;
    handmesh::Quad quad;   // what it was drawn for
};

// A look (HANDS_VIEW_STYLE): the second pass's colours and opacities, and the robot's.
struct Style {
    const char *name;
    float base[3], shade, body, fres, rimPow, glow[3], tip;
    // the robot (kRobotRimFs): its shells', joints' and accents' colours, each material's opacity
    // (shell, joint, accent, line), and how much the shells and the joints shine
    float shell[3], dark[3], accent[3], opacity[4], shine[2];
};
// ghost: Quest-like, light and see-through, glowing white toward its edges and at the fingertips;
// solid: a matte light hand lit from above, with a soft cool rim light, over anything. The robot:
// ghost, pale see-through shells over darker joints, the accents glowing nearly solid; solid,
// matte ceramic white shells and dark grey metal joints with cyan accents.
const Style kStyles[] = {{"ghost", {0.84f, 0.88f, 0.95f}, 0.30f, 0.30f, 0.90f, 2.2f, {1, 1, 1}, 0.30f,
                          {0.86f, 0.92f, 1}, {0.30f, 0.36f, 0.44f}, {0.35f, 0.90f, 1}, {0.30f, 0.50f, 0.95f, 0.50f}, {0.25f, 0.35f}},
                         {"solid", {0.82f, 0.83f, 0.86f}, 0.45f, 0.96f, 0.55f, 2.6f, {0.85f, 0.93f, 1}, 0.12f,
                          {0.93f, 0.94f, 0.95f}, {0.15f, 0.16f, 0.18f}, {0.30f, 0.88f, 1}, {0.97f, 0.98f, 1, 0.97f}, {0.22f, 0.50f}}};
const Style *FindStyle(const char *name) {
    for (const Style &s : kStyles)
        if (!std::strcmp(s.name, name)) return &s;
    return nullptr;
}

// The model's wrist end where the fade cuts it (the cap): the cut's middle, its normal (along
// the hand, toward the fingers), and an ellipse in it: its axes over their radii (1/m). !ok: no cap.
struct Cut {
    float centre[3], axis[3], u[3], v[3];
    bool ok = false;
};
constexpr float kModelFade[2] = {-0.035f, -0.004f};   // the model's fade: vback from its start to the cut
constexpr float kCapOpacity = 0.9f;                     // the cap's middle, against the body's

// A 4x4 matrix's inverse (GL order). False if it has none.
bool Invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[c * 4 + r] : double(c - 4 == r);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = float(a[r][c + 4]);
    return true;
}

class Gpu {
public:
    bool Init(const std::vector<uint64_t> &modifiers, int eyePx);
    // vr false: an offscreen buffer to read back (--snapshot), no SteamVR
    bool MakeOutput(Output &o, bool vr = true);
    void FreeOutput(Output &o);
    // Draws the mesh (the capsules' hand) for both eyes into o and fences it; returns at once.
    // `tips`: the fingertips (they glow). `cut`: the model's wrist end (CutOf), capped.
    void Draw(Output &o, const handmesh::Mesh &m, const handmesh::Quad &q, const double eyes[2][3], const float tips[15],
              const Style &st);
    // ...or the model of that side, posed by `bones`.
    void DrawModel(Output &o, bool right, const handskin::Bones &bones, const handmesh::Quad &q, const double eyes[2][3],
                   const float tips[15], const struct Cut &cut, const Style &st);
    // ...or the robot, posed by `bones`.
    void DrawRobot(Output &o, const handrobot::Bones &bones, const handmesh::Quad &q, const double eyes[2][3], const Style &st);
    bool Passed(Output &o);
    // The output's pixels, RGBA, its first row the top (--snapshot).
    void Read(Output &o, std::vector<uint8_t> &rgba);
    void Done();

private:
    void Body(GLuint prog, const float tips[15]);
    void Eyes(const handmesh::Quad &q, const double eyes[2][3], GLuint prog, GLsizei count);
    void Cap(const handmesh::Quad &q, const double eyes[2][3], const Cut &cut);
    void Rim(Output &o, const handmesh::Quad &q, const Style &st, bool robot = false);
    int drm_ = -1, eye_ = 0;
    gbm_device *gbm_ = nullptr;
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    std::vector<uint64_t> mods_;
    GLuint body_ = 0, skin_ = 0, robot_ = 0, robotRim_ = 0, robotVbo_ = 0, robotIbo_ = 0, cap_ = 0, rim_ = 0, scratchTex_ = 0, scratchDepth_ = 0, scratch_ = 0, vbo_ = 0, ibo_ = 0, quad_ = 0;
    GLuint modelVbo_[handasset::kModels][2] = {}, modelIbo_[handasset::kModels][2] = {};   // the models' hands, uploaded once
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
    skin_ = Program(kSkinVs, kBodyFs, {"pos", "nrm", "joints", "weights"});
    cap_ = Program(kCapVs, kCapFs, {"pos"});
    rim_ = Program(kRimVs, kRimFs, {"pos"});
    robot_ = Program(kRobotVs, kRobotFs, {"pos", "nrm", "info"});
    robotRim_ = Program(kRimVs, kRobotRimFs, {"pos"});
    if (!body_ || !skin_ || !cap_ || !rim_ || !robot_ || !robotRim_) return false;
    // the scratch buffer: both eyes side by side, with depth and stencil (the cap)
    glGenTextures(1, &scratchTex_);
    glBindTexture(GL_TEXTURE_2D, scratchTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2 * eye_, eye_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &scratchDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, scratchDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, 2 * eye_, eye_);
    glGenFramebuffers(1, &scratch_);
    glBindFramebuffer(GL_FRAMEBUFFER, scratch_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scratchDepth_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scratchDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "handview: scratch framebuffer incomplete\n"), false;
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    // the model: positions, normals, joints, weights one after another, and the triangles
    glGenBuffers(2 * handasset::kModels, &modelVbo_[0][0]);
    glGenBuffers(2 * handasset::kModels, &modelIbo_[0][0]);
    for (int k = 0; k < 2 * handasset::kModels; ++k) {
        const int model = k / 2, side = k % 2;
        const handasset::Hand &a = handasset::kModel[model].hands[side];
        const size_t n = size_t(a.verts), sizes[4] = {n * 6, n * 3, n * 4, n * 4};
        glBindBuffer(GL_ARRAY_BUFFER, modelVbo_[model][side]);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizes[0] + sizes[1] + sizes[2] + sizes[3]), nullptr, GL_STATIC_DRAW);
        const void *parts[4] = {a.pos, a.nrm, a.joints, a.weights};
        size_t at = 0;
        for (int k = 0; k < 4; ++k) glBufferSubData(GL_ARRAY_BUFFER, GLintptr(at), GLsizeiptr(sizes[k]), parts[k]), at += sizes[k];
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modelIbo_[model][side]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(a.tris * 3 * sizeof(uint16_t)), a.idx, GL_STATIC_DRAW);
    }
    // the robot: its vertices as they are (handrobot::Vertex), one mesh for both hands
    const handrobot::Mesh &r = handrobot::Model();
    glGenBuffers(1, &robotVbo_);
    glGenBuffers(1, &robotIbo_);
    glBindBuffer(GL_ARRAY_BUFFER, robotVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.v.size() * sizeof(handrobot::Vertex)), r.v.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, robotIbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(r.idx.size() * sizeof(uint16_t)), r.idx.data(), GL_STATIC_DRAW);
    return true;
}

bool Gpu::MakeOutput(Output &o, bool vr) {
    const int w = 2 * eye_, h = eye_;
    if (!vr) {
        glGenTextures(1, &o.tex);
        glBindTexture(GL_TEXTURE_2D, o.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &o.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o.tex, 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
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
    if (o.tex) glDeleteTextures(1, &o.tex);
    if (o.image != EGL_NO_IMAGE_KHR) pDestroyImage(dpy_, o.image);
    if (o.bo) gbm_bo_destroy(o.bo);
    o = Output{};
}

// The body pass's start: the scratch buffer cleared, depth on, `prog` in use.
void Gpu::Body(GLuint prog, const float tips[15]) {
    glBindFramebuffer(GL_FRAMEBUFFER, scratch_);
    glViewport(0, 0, 2 * eye_, eye_);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CW);   // the eye matrices put the top in the first row: counterclockwise turns clockwise
    glUseProgram(prog);
    glUniform3fv(glGetUniformLocation(prog, "tips"), 5, tips);
}

// Both eyes' halves of the body: `count` indices of what's bound.
void Gpu::Eyes(const handmesh::Quad &q, const double eyes[2][3], GLuint prog, GLsizei count) {
    for (int e = 0; e < 2; ++e) {
        float mvp[16];
        handmesh::EyeMatrix(q, eyes[e], 0.05, q.distance + 1.0, mvp);
        glViewport(e * eye_, 0, eye_, eye_);
        glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, mvp);
        glUniform3f(glGetUniformLocation(prog, "eye"), float(eyes[e][0]), float(eyes[e][1]), float(eyes[e][2]));
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr);
    }
}

void Gpu::Draw(Output &o, const handmesh::Mesh &m, const handmesh::Quad &q, const double eyes[2][3], const float tips[15],
               const Style &st) {
    Body(body_, tips);
    glUniform2f(glGetUniformLocation(body_, "fade"), -0.01f, 0.10f);   // gone by handmesh.cpp kFaded
    glUniform1f(glGetUniformLocation(body_, "open"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.v.size() * sizeof(handmesh::Vertex)), m.v.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(m.idx.size() * sizeof(uint16_t)), m.idx.data(), GL_STREAM_DRAW);
    const GLsizei stride = sizeof(handmesh::Vertex);
    glEnableVertexAttribArray(0), glEnableVertexAttribArray(1), glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, p)));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, n)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handmesh::Vertex, back)));
    Eyes(q, eyes, body_, GLsizei(m.idx.size()));
    glDisableVertexAttribArray(1), glDisableVertexAttribArray(2);
    Rim(o, q, st);
}

void Gpu::DrawModel(Output &o, bool right, const handskin::Bones &bones, const handmesh::Quad &q, const double eyes[2][3],
                    const float tips[15], const Cut &cut, const Style &st) {
    Body(skin_, tips);
    // the model's wrist: its heel fades out toward the tracked wrist, and the short cuff past it
    // isn't drawn (seen end on, with the hand pointing away, it would hide the hand)
    glUniform2f(glGetUniformLocation(skin_, "fade"), kModelFade[0], kModelFade[1]);
    glUniform1f(glGetUniformLocation(skin_, "open"), 1);
    glUniform4fv(glGetUniformLocation(skin_, "bones"), 3 * handskin::kJoints, &bones.m[0][0]);
    glUniform1f(glGetUniformLocation(skin_, "posStep"), handasset::kPosStep);
    glUniform3fv(glGetUniformLocation(skin_, "wrist"), 1, bones.wrist);
    glUniform3fv(glGetUniformLocation(skin_, "fwd"), 1, bones.fwd);
    const handasset::Hand &a = handasset::kModel[bones.model].hands[right];
    const size_t n = size_t(a.verts);
    glBindBuffer(GL_ARRAY_BUFFER, modelVbo_[bones.model][right]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modelIbo_[bones.model][right]);
    for (int k = 0; k < 4; ++k) glEnableVertexAttribArray(k);
    glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 0, nullptr);
    glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, 0, reinterpret_cast<void *>(n * 6));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, 0, reinterpret_cast<void *>(n * 9));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, reinterpret_cast<void *>(n * 13));
    const GLsizei count = GLsizei(a.tris * 3);
    if (cut.ok) {
        // the cap's count first, on its own: every fragment drawn counts, in front or behind (with
        // the depth test on, the GPU may drop hidden ones before the stencil sees them), front
        // faces up, back faces down
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xff);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        glDisable(GL_DEPTH_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        Eyes(q, eyes, skin_, count);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
    }
    Eyes(q, eyes, skin_, count);
    for (int k = 1; k < 4; ++k) glDisableVertexAttribArray(k);
    if (cut.ok) glEnable(GL_STENCIL_TEST), Cap(q, eyes, cut);
    glDisable(GL_STENCIL_TEST);
    Rim(o, q, st);
}

void Gpu::DrawRobot(Output &o, const handrobot::Bones &bones, const handmesh::Quad &q, const double eyes[2][3], const Style &st) {
    const float none[15] = {};
    Body(robot_, none);
    glUniform4fv(glGetUniformLocation(robot_, "segs"), 4 * handrobot::kSegs, &bones.seg[0][0]);
    glBindBuffer(GL_ARRAY_BUFFER, robotVbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, robotIbo_);
    const GLsizei stride = sizeof(handrobot::Vertex);
    for (int k = 0; k < 3; ++k) glEnableVertexAttribArray(k);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handrobot::Vertex, p)));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handrobot::Vertex, n)));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(handrobot::Vertex, seg)));
    Eyes(q, eyes, robot_, GLsizei(handrobot::Model().idx.size()));
    for (int k = 1; k < 3; ++k) glDisableVertexAttribArray(k);
    Rim(o, q, st, true);
}

// The cap (kCapFs) over the body in the scratch buffer, where the stencil says the view went in
// through the cut.
void Gpu::Cap(const handmesh::Quad &q, const double eyes[2][3], const Cut &cut) {
    glDisable(GL_DEPTH_TEST);
    glStencilFunc(GL_EQUAL, 0xff, 0xff);   // -1: one more back face than front, so in through the cut
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glUseProgram(cap_);
    glUniform3fv(glGetUniformLocation(cap_, "centre"), 1, cut.centre);
    glUniform3fv(glGetUniformLocation(cap_, "axis"), 1, cut.axis);
    glUniform3fv(glGetUniformLocation(cap_, "u"), 1, cut.u);
    glUniform3fv(glGetUniformLocation(cap_, "v"), 1, cut.v);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        float mvp[16], inv[16];
        handmesh::EyeMatrix(q, eyes[e], 0.05, q.distance + 1.0, mvp);
        if (!Invert(mvp, inv)) continue;
        // how squarely the cut faces this eye: the rim pass's fresnel, as for the body
        double to[3], n = 0, facing = 0;
        for (int i = 0; i < 3; ++i) to[i] = eyes[e][i] - cut.centre[i], n += to[i] * to[i];
        for (int i = 0; i < 3; ++i) facing += to[i] * cut.axis[i] / std::sqrt(std::max(n, 1e-12));
        glViewport(e * eye_, 0, eye_, eye_);
        glUniformMatrix4fv(glGetUniformLocation(cap_, "inv"), 1, GL_FALSE, inv);
        glUniform2f(glGetUniformLocation(cap_, "look"), float(0.45 * std::fabs(facing)), kCapOpacity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

// The rim pass, from the scratch buffer into the output; fenced.
void Gpu::Rim(Output &o, const handmesh::Quad &q, const Style &st, bool robot) {
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glViewport(0, 0, 2 * eye_, eye_);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint prog = robot ? robotRim_ : rim_;
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scratchTex_);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glUniform2f(glGetUniformLocation(prog, "texel"), 1.0f / (2 * eye_), 1.0f / eye_);
    glUniform1f(glGetUniformLocation(prog, "shade"), st.shade);
    glUniform1f(glGetUniformLocation(prog, "fres"), st.fres);
    glUniform1f(glGetUniformLocation(prog, "rimPow"), st.rimPow);
    glUniform3fv(glGetUniformLocation(prog, "glow"), 1, st.glow);
    if (robot) {
        glUniform3fv(glGetUniformLocation(prog, "shell"), 1, st.shell);
        glUniform3fv(glGetUniformLocation(prog, "dark"), 1, st.dark);
        glUniform3fv(glGetUniformLocation(prog, "accent"), 1, st.accent);
        glUniform4fv(glGetUniformLocation(prog, "opacity"), 1, st.opacity);
        glUniform2fv(glGetUniformLocation(prog, "shine"), 1, st.shine);
    } else {
        glUniform1f(glGetUniformLocation(prog, "body"), st.body);
        glUniform3fv(glGetUniformLocation(prog, "base"), 1, st.base);
        glUniform1f(glGetUniformLocation(prog, "tip"), st.tip);
    }
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        glViewport(e * eye_, 0, eye_, eye_);
        const float half = 0.5f / (2 * eye_);   // texel centres: the taps stay in this eye's half
        glUniform4f(glGetUniformLocation(prog, "box"), e * 0.5f + half, 0.5f / eye_, e * 0.5f + 0.5f - half, 1 - 0.5f / eye_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (o.fence != EGL_NO_SYNC_KHR) pDestroySync(dpy_, o.fence);
    o.fence = pCreateSync(dpy_, EGL_SYNC_FENCE_KHR, nullptr);
    glFlush();
    o.quad = q;
}

void Gpu::Read(Output &o, std::vector<uint8_t> &rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    rgba.resize(size_t(2 * eye_) * eye_ * 4);
    glReadPixels(0, 0, 2 * eye_, eye_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
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
    float flash = 0;                   // the flash's pulse it was last drawn with
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

// --demo: a hand opening and closing, fingers up and the palm toward you, 45 cm ahead of `head`,
// 25 cm down and 12 cm to its side, turning slowly about its arm.
handcut::Hand DemoHand(const Mat &head, bool right, double t) {
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]);   // the head's heading, level
    const double fx = -std::sin(yaw), fz = -std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    const double side = right ? 1 : -1, turn = 0.5 * std::sin(t * 0.8);
    const double at[3] = {head.m[0][3] + 0.45 * fx + 0.12 * side * rx, head.m[1][3] - 0.33, head.m[2][3] + 0.45 * fz + 0.12 * side * rz};
    // the palm toward you, turned by `turn` about the vertical
    const double palm[3] = {-fx * std::cos(turn) + rx * std::sin(turn), 0, -fz * std::cos(turn) + rz * std::sin(turn)};
    const double up[3] = {0, 1, 0};
    handcut::Hand h;
    handskin::DemoPose("wave", right, at, palm, up, t, h);
    return h;
}

// The settings that pick how a hand is drawn (HANDS_VIEW_*), and the live commands' changes.
constexpr int kCapsules = -1, kRobot = -2;   // Look::model's that aren't handasset's

struct Look {
    int model = 0;              // the 3D model (handasset::kModel's), or kCapsules, kRobot
    const Style *style = &kStyles[0];
    bool inRoom = true;         // draw with Room View on too
};

// A model's name (HANDS_VIEW_MODEL, the model command): one of handasset's, robot or capsules.
bool ParseModel(const char *name, int &model) {
    const int k = !std::strcmp(name, "capsules") ? kCapsules : !std::strcmp(name, "robot") ? kRobot : handskin::FindModel(name);
    if (k == -1 && std::strcmp(name, "capsules")) return false;
    model = k;
    return true;
}

std::string ModelNames() {
    std::string s;
    for (const handasset::Model &m : handasset::kModel) s += std::string(m.name) + "|";
    return s + "robot|capsules";
}

const char *ModelName(int model) { return model == kRobot ? "robot" : model < 0 ? "capsules" : handasset::kModel[model].name; }

Look ReadLook(std::string &log) {
    Look k;
    const std::string m = handcut::Setting("HANDS_VIEW_MODEL", "asset"), st = handcut::Setting("HANDS_VIEW_STYLE", "ghost"),
                      r = handcut::Setting("HANDS_VIEW_ROOMVIEW", "1");
    if (!ParseModel(m.c_str(), k.model)) log += "HANDS_VIEW_MODEL=" + m + " (ignored: " + ModelNames() + ") ";
    if (const Style *f = FindStyle(st.c_str())) k.style = f;
    else log += "HANDS_VIEW_STYLE=" + st + " (ignored: ghost|solid) ";
    if (r == "0" || r == "1") k.inRoom = r == "1";
    else log += "HANDS_VIEW_ROOMVIEW=" + r + " (ignored: 1|0) ";
    return k;
}

// How a hand is drawn, the CPU's part: pose it (a model, the robot) or build its mesh (the
// capsules), and place its square. False if there's nothing to draw. `mesh` holds the capsules'
// mesh, or the model's or the robot's posed points (its bounds).
bool Prepare(const Look &look, const handcut::Hand &h, const double headPos[3], const double eyes[2][3],
             handmesh::Mesh &mesh, handskin::Bones &bones, handrobot::Bones &robot, handmesh::Quad &q) {
    if (look.model == kRobot) {
        if (!handrobot::Pose(h, robot)) return false;
        handrobot::Points(robot, 4, mesh);
        return handmesh::Place(mesh, eyes, 0.05, q);
    }
    if (look.model >= 0) {
        if (!handskin::Pose(look.model, h, bones)) return false;
        handskin::Points(h.right, bones, 3, mesh);   // a third of the vertices: the square has a margin
        return handmesh::Place(mesh, eyes, 0.05, q);
    }
    handmesh::Build(h, headPos, mesh);
    return handmesh::Place(mesh, eyes, 0.04, q);
}

struct V {
    double x, y, z;
    V(double a = 0, double b = 0, double c = 0) : x(a), y(b), z(c) {}
    V operator+(const V &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(const V &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const V &o) const { return x * o.x + y * o.y + z * o.z; }
    V cross(const V &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    V unit() const {
        const double n = std::sqrt(dot(*this));
        return n > 1e-12 ? *this * (1 / n) : V{};
    }
};

// The model's cut (the cap): from its skinned points (Prepare's), those within 5 mm of the cut:
// their middle, and the ellipse with their spread across the hand (points evenly round an
// ellipse spread by half its radius squared along each axis).
Cut CutOf(const handmesh::Mesh &points, const handskin::Bones &b) {
    Cut c;
    const V f(b.fwd[0], b.fwd[1], b.fwd[2]), w(b.wrist[0], b.wrist[1], b.wrist[2]);
    std::vector<V> near;
    for (const handmesh::Vertex &p : points.v) {
        const V q(p.p[0], p.p[1], p.p[2]);
        const double k = (w - q).dot(f);
        if (std::fabs(k - kModelFade[0]) <= 0.005) near.push_back(q);
    }
    if (near.size() < 6) return c;
    V m;
    for (const V &q : near) m = m + q;
    m = m * (1.0 / near.size());
    m = m + f * ((w - m).dot(f) - kModelFade[0]);   // onto the cut's plane
    // a square pair across the hand, and the spread along them
    const V a = (std::fabs(f.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0)).cross(f).unit(), bb = f.cross(a);
    double saa = 0, sab = 0, sbb = 0;
    for (const V &q : near) {
        const double x = (q - m).dot(a), y = (q - m).dot(bb);
        saa += x * x, sab += x * y, sbb += y * y;
    }
    saa /= near.size(), sab /= near.size(), sbb /= near.size();
    // its principal axes (a 2x2 symmetric matrix's eigenvectors)
    const double th = 0.5 * std::atan2(2 * sab, saa - sbb), cs = std::cos(th), sn = std::sin(th);
    const V e1 = a * cs + bb * sn, e2 = bb * cs - a * sn;
    const double l1 = saa * cs * cs + 2 * sab * cs * sn + sbb * sn * sn, l2 = saa * sn * sn - 2 * sab * cs * sn + sbb * cs * cs;
    const double r1 = std::sqrt(std::max(2 * l1, 1e-6)), r2 = std::sqrt(std::max(2 * l2, 1e-6));
    const V u = e1 * (1 / r1), v = e2 * (1 / r2);
    const double out[4][3] = {{m.x, m.y, m.z}, {f.x, f.y, f.z}, {u.x, u.y, u.z}, {v.x, v.y, v.z}};
    for (int i = 0; i < 3; ++i) c.centre[i] = float(out[0][i]), c.axis[i] = float(out[1][i]), c.u[i] = float(out[2][i]), c.v[i] = float(out[3][i]);
    c.ok = true;
    return c;
}

// flash: how far into the cyan a pulse (the flash command) is, 0..1, at `ago` seconds after it began.
constexpr double kFlashS = 0.5;
float FlashAmount(double ago) { return ago < 0 || ago > kFlashS ? 0.0f : float(std::sin(M_PI * ago / kFlashS)); }

// The look with a flash's pulse in it: the colours toward cyan, the accents brighter.
Style Flashed(const Style &st, float f) {
    Style o = st;
    const float cyan[3] = {0.30f, 0.88f, 1.0f};
    for (int i = 0; i < 3; ++i) {
        o.base[i] += (cyan[i] - o.base[i]) * 0.7f * f;
        o.glow[i] += (cyan[i] - o.glow[i]) * f;
        o.shell[i] += (cyan[i] - o.shell[i]) * 0.5f * f;
        o.accent[i] += (1.0f - o.accent[i]) * 0.6f * f;
    }
    o.body += (1.0f - o.body) * 0.5f * f, o.fres += (1.0f - o.fres) * 0.5f * f, o.tip += (1.0f - o.tip) * f;
    for (float &op : o.opacity) op += (1.0f - op) * 0.5f * f;
    return o;
}

void Render(Gpu &gpu, Output &o, const Look &look, const handcut::Hand &h, const handmesh::Mesh &mesh,
            const handskin::Bones &bones, const handrobot::Bones &robot, const handmesh::Quad &q, const double eyes[2][3]) {
    if (look.model == kRobot) return gpu.DrawRobot(o, robot, q, eyes, *look.style);
    float tips[15];
    for (int f = 0; f < 5; ++f)
        for (int i = 0; i < 3; ++i) tips[3 * f + i] = h.p[4 + 4 * f][i];
    if (look.model >= 0) gpu.DrawModel(o, h.right, bones, q, eyes, tips, CutOf(mesh, bones), *look.style);
    else gpu.Draw(o, mesh, q, eyes, tips, *look.style);
}

// --snapshot: demo poses of both hands drawn offscreen into a PNG, then the cost of drawing.
int Snapshot(const char *path, int res, double seconds, const Look &look) {
    Gpu gpu;
    if (!gpu.Init({}, res)) return 1;
    Output out;
    if (!gpu.MakeOutput(out, false)) return std::fprintf(stderr, "snapshot: no offscreen buffer\n"), 1;
    const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}}, head[3] = {0, 0, 0};
    struct Shot {
        const char *pose;
        double palm[3], up[3];
    };
    // the head at the origin looking along -z; each hand 40 cm ahead, 15 cm down, 10 cm aside
    const Shot shots[] = {{"open", {0, 0.3, 1}, {0, 1, -0.3}},    {"open", {0, -0.3, -1}, {0, 1, -0.3}},  // palm toward you, away
                          {"open", {0, 1, 0}, {0, 0, -1}},         {"fist", {0.5, 0, 1}, {0, 1, -0.3}},
                          {"point", {0, -1, 0.2}, {0, 0.2, -1}},   {"pinch", {1, 0, 0.6}, {0, 1, -0.4}},
                          {"spread", {0, 0.2, 1}, {0, 1, -0.2}}};
    const int rows = int(sizeof shots / sizeof shots[0]), W = 4 * res, H = rows * res;
    std::vector<uint8_t> img(size_t(W) * H * 3), px;
    handmesh::Mesh mesh;
    handskin::Bones bones;
    handrobot::Bones robot;
    for (int r = 0; r < rows; ++r)
        for (int right = 0; right < 2; ++right) {
            const Shot &sh = shots[r];
            const double side = right ? 1 : -1;
            // the left hand mirrored across the middle: its palm and fingers' sideways parts flip
            const double at[3] = {0.10 * side, -0.22, -0.40}, palm[3] = {sh.palm[0] * -side, sh.palm[1], sh.palm[2]},
                         up[3] = {sh.up[0] * -side, sh.up[1], sh.up[2]};
            handcut::Hand h;
            handmesh::Quad q;
            if (!handskin::DemoPose(sh.pose, right, at, palm, up, 0, h)) continue;
            if (!Prepare(look, h, head, eyes, mesh, bones, robot, q)) continue;
            Render(gpu, out, look, h, mesh, bones, robot, q, eyes);
            glFinish();
            gpu.Read(out, px);
            // over dark (the left hand) or light (the right): the rows' top first
            const float bg = right ? 0.62f : 0.12f;
            for (int y = 0; y < res; ++y)
                for (int x = 0; x < 2 * res; ++x) {
                    const uint8_t *c = &px[(size_t(y) * 2 * res + x) * 4];
                    uint8_t *d = &img[(size_t(r * res + y) * W + right * 2 * res + x) * 3];
                    const float a = c[3] / 255.0f;
                    for (int k = 0; k < 3; ++k) d[k] = uint8_t(std::lround(c[k] * a + 255 * bg * (1 - a)));
                }
        }
    if (!stbi_write_png(path, W, H, 3, img.data(), W * 3)) return std::fprintf(stderr, "snapshot: can't write %s\n", path), 1;
    std::printf("wrote %s: %d poses (open palm toward you, palm away, palm up, fist, point, pinch, spread), "
                "each row the left hand (left eye | right eye, over dark) and the right (over light); %s, %s\n",
                path, rows, ModelName(look.model), look.style->name);
    // the cost: both waving hands, each drawn 30 times a second, as ft-handview draws them
    const double cpu0 = CpuSeconds();
    double drawMs = 0, poseMs = 0, gpuMs = 0;
    int draws = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds; ++frame) {
        const auto tick = std::chrono::steady_clock::now();
        Mat ident{};
        ident.m[0][0] = ident.m[1][1] = ident.m[2][2] = 1;
        for (int right = 0; right < 2; ++right) {
            const auto t0 = std::chrono::steady_clock::now();
            const handcut::Hand h = DemoHand(ident, right, frame / 30.0);
            handmesh::Quad q;
            if (!Prepare(look, h, head, eyes, mesh, bones, robot, q)) continue;
            poseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Render(gpu, out, look, h, mesh, bones, robot, q, eyes);
            drawMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++draws;
        }
        const auto f0 = std::chrono::steady_clock::now();
        glFinish();   // as the fence would have passed by the next frame
        gpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
        std::this_thread::sleep_until(tick + std::chrono::microseconds(33333));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%d draws in %.1f s: CPU %.3f ms per draw (posing and placing %.3f), %.1f%% of a core in all; "
                "then waiting for the GPU %.3f ms per draw\n", draws, wall, draws ? drawMs / draws : 0.0, draws ? poseMs / draws : 0.0,
                100 * (CpuSeconds() - cpu0) / wall, draws ? gpuMs / draws : 0.0);
    gpu.FreeOutput(out);
    gpu.Done();
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    double statusS = 0, seconds = 0;
    int res = 320;
    bool demo = false;
    const char *snapshot = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--status") && more) statusS = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--res") && more) res = std::clamp(std::atoi(argv[++i]), 64, 1024);
        else if (!std::strcmp(argv[i], "--seconds") && more) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--demo")) demo = true;
        else if (!std::strcmp(argv[i], "--snapshot") && more) snapshot = argv[++i];
        else return std::fprintf(stderr, "usage: ft-handview [--status S] [--res PX] [--demo] [--seconds S]\n"
                                         "       ft-handview --snapshot FILE.png [--res PX] [--seconds S]\n"), 2;
    }
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string lookLog;
    Look look = ReadLook(lookLog);
    if (!lookLog.empty()) std::fprintf(stderr, "%s\n", lookLog.c_str());
    if (snapshot) return Snapshot(snapshot, res, seconds > 0 ? seconds : 5, look);

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
    handskin::Bones bones;
    handrobot::Bones robot;
    std::printf("ft-handview: %dx%d per eye per hand%s; %s, %s; Room View %s, %s with it on\n", res, res,
                demo ? ", demo hands" : "", ModelName(look.model), look.style->name,
                force == roomview::Force::Auto ? "detected" : roomview::Name(force), look.inRoom ? "drawn" : "not drawn");

    bool scene = false, constructRoomView = true;
    int64_t nextScene = 0, nextSettings = 0;
    const int64_t start = MonoNs();
    int64_t lastStatus = start;
    double cpu0 = CpuSeconds();
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    Mat demoHead{};
    bool haveDemoHead = false;
    std::string statusLine = "starting";
    int64_t flashAt = INT64_MIN / 2;   // the flash command's time
    // the head's poses for `poses` (see the top): 30 a second, 20 s
    struct HeadPose {
        int64_t ns;
        Mat m;
    };
    std::vector<HeadPose> headPoses;
    size_t headNext = 0;
    constexpr size_t kHeadPoses = 600;
    int64_t headAt = 0;

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
            } else if (std::sscanf(buf, "model %15s", word) == 1 && ParseModel(word, look.model)) {
                reply = "ok";
            } else if (const Style *st = std::sscanf(buf, "style %15s", word) == 1 ? FindStyle(word) : nullptr) {
                look.style = st, reply = "ok";
            } else if (std::sscanf(buf, "inroom %15s", word) == 1 && (!std::strcmp(word, "1") || !std::strcmp(word, "0"))) {
                look.inRoom = word[0] == '1', reply = "ok";
            } else if (!std::strcmp(buf, "state")) {
                reply = "ok " + statusLine;
            } else if (!std::strcmp(buf, "flash")) {
                flashAt = now;
                reply = std::string("ok ") + std::to_string(int(sides[0].visible) + int(sides[1].visible));
            } else if (double secs = 0; std::sscanf(buf, "poses %lf", &secs) == 1) {
                reply = "ok";
                const int64_t since = now - int64_t(std::clamp(secs, 0.0, 20.0) * 1e9);
                for (size_t k = 0; k < headPoses.size(); ++k) {
                    const HeadPose &p = headPoses[(headNext + k) % headPoses.size()];
                    if (p.ns < since) continue;
                    char line[320];
                    int len = std::snprintf(line, sizeof line, "\n%lld", (long long)p.ns);
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 4; ++c) len += std::snprintf(line + len, sizeof line - len, " %.5f", p.m.m[r][c]);
                    reply += line;
                }
            } else reply = "error hands <L> <R> <allowed>|roomview auto|on|off|model " + ModelNames() + "|style ghost|solid|inroom 1|0|state|flash|poses <S>";
            if (reply == "ok")
                for (Side &s : sides) s.lastFrame = ~uint64_t(0);   // a new look shows at once
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
        if (hp.bPoseIsValid && now - headAt >= 33'000'000) {
            headAt = now;
            if (headPoses.size() < kHeadPoses) headPoses.push_back({now, head});
            else headPoses[headNext] = {now, head}, headNext = (headNext + 1) % kHeadPoses;
        }
        const float flash = FlashAmount((now - flashAt) / 1e9);
        std::vector<handcut::Hand> shown;
        uint64_t frame = 0;
        if (demo) {
            if (!haveDemoHead && hp.bPoseIsValid) demoHead = head, haveDemoHead = true;
            if (haveDemoHead) {
                const double t = (now - start) / 1e9;
                shown = {DemoHand(demoHead, false, t), DemoHand(demoHead, true, t)};
                frame = uint64_t(t * 30);   // a "hands frame" 30 times a second
            }
        } else if (hp.bPoseIsValid && (!roomOn || look.inRoom) && hands.Update(head, now)) {
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
            else if (roomOn && !demo && !look.inRoom) why = "Room View on";
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
            // and through a flash, until it's drawn without it again
            const bool flashing = (flash > 0 || s.flash > 0) && now - s.lastDraw >= 20'000'000;
            const bool want = frame != s.lastFrame || (moved > 0.003 && now - s.lastDraw >= 20'000'000) || !s.visible || flashing;
            if (!want) continue;
            if (s.drawing >= 0) {   // the GPU is still on the last one
                s.busy += s.visible;
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            handmesh::Quad q;
            if (!Prepare(look, *h, headPos, eyes, mesh, bones, robot, q)) continue;
            s.meshMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            int pick = 0;
            while (pick == s.shown || pick == s.before) ++pick;
            if (flash > 0) {
                const Style st = Flashed(*look.style, flash);
                Look lit = look;
                lit.style = &st;
                Render(gpu, s.out[pick], lit, *h, mesh, bones, robot, q, eyes);
            } else {
                Render(gpu, s.out[pick], look, *h, mesh, bones, robot, q, eyes);
            }
            s.flash = flash;
            s.drawing = pick, s.drawnAt = MonoNs();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++s.draws, s.cpuMs += ms, s.worstMs = std::max(s.worstMs, ms);
            s.lastFrame = frame, s.lastDraw = now;
            std::memcpy(s.lastPalm, palm, sizeof palm);
        }

        if (now - lastStatus >= int64_t((statusS > 0 ? statusS : 2) * 1e9)) {
            const double dt = (now - lastStatus) / 1e9, cpu1 = CpuSeconds();
            char line[640];
            int k = std::snprintf(line, sizeof line, "room view %s (%s%s), helper %s; %s, %s; CPU %.1f%%", roomOn ? "on" : "off",
                                  room.why(), roomOn && !look.inRoom ? ": not drawn" : "", heard ? "heard" : "silent",
                                  ModelName(look.model), look.style->name, 100 * (cpu1 - cpu0) / dt);
            for (Side &s : sides)
                k += std::snprintf(line + k, sizeof line - k, "; %s %s, %.1f draws/s, CPU %.2f ms avg (posing %.2f) %.2f worst, up after %.1f ms, %d busy",
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
        const bool busy = sides[0].visible || sides[1].visible || sides[0].drawing >= 0 || sides[1].drawing >= 0 || flash > 0;
        const bool idle = roomOn && !demo && !look.inRoom;
        std::this_thread::sleep_until(tick + std::chrono::microseconds(busy ? 11111 : idle ? 100000 : 50000));
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
