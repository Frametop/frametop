// A body drawn with GLES 2 (bodygpu.h).
#include "bodygpu.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace bodygpu {

using bodyfile::Material;

namespace {

constexpr int kBoneRows = bodyfile::kRoles * 3;   // a 3x4 matrix is three vec4 rows

const char *const kVs = R"(
attribute vec3 pos;
attribute vec3 nrm;
attribute vec2 uv;
attribute vec4 joints;
attribute vec4 weights;
uniform vec4 bones[165];
uniform mat4 clip;
varying vec3 vn;
varying vec2 vuv;
vec3 by(float j, vec4 p) {
    int i = int(j) * 3;
    return vec3(dot(bones[i], p), dot(bones[i + 1], p), dot(bones[i + 2], p));
}
void main() {
    vec4 p = vec4(pos, 1.0), n = vec4(nrm, 0.0);
    vec3 at = by(joints.x, p) * weights.x + by(joints.y, p) * weights.y + by(joints.z, p) * weights.z + by(joints.w, p) * weights.w;
    vn = by(joints.x, n) * weights.x + by(joints.y, n) * weights.y + by(joints.z, n) * weights.z + by(joints.w, n) * weights.w;
    vuv = uv;
    gl_Position = clip * vec4(at, 1.0);
})";

// look: x how it's shaded (0 lit, 1 in two tones, 2 in its own colours), y the alpha under which
// a cut-out isn't drawn, z 1 for a part that isn't see-through (what is drawn of it is solid,
// whatever the texture's alpha).
const char *const kFs = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D tex;
uniform vec4 color;
uniform vec3 light;
uniform vec4 look;
varying vec3 vn;
varying vec2 vuv;
void main() {
    vec4 c = texture2D(tex, vuv) * color;
    if (c.a < look.y) discard;
    vec3 n = normalize(vn);
    if (!gl_FrontFacing) n = -n;
    float d = dot(n, light) * 0.5 + 0.5;
    float lit = 0.5 + 0.5 * d;
    if (look.x > 0.5) lit = 0.8 + 0.2 * smoothstep(0.40, 0.46, d);
    if (look.x > 1.5) lit = 1.0;
    gl_FragColor = vec4(c.rgb * lit, max(c.a, look.z));
})";

GLuint Shader(const char *who, GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "%s: shader: %s\n", who, log);
    }
    return s;
}

bool PowerOfTwo(int n) { return n > 0 && !(n & (n - 1)); }

}  // namespace

bool Painter::Init(const char *who) {
    GLint rows = 0;
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &rows);
    if (rows < kBoneRows + 8) return std::fprintf(stderr, "%s: the GPU takes %d uniform rows, a body's bones need %d\n", who, rows, kBoneRows + 8), false;
    prog_ = glCreateProgram();
    const GLuint vs = Shader(who, GL_VERTEX_SHADER, kVs), fs = Shader(who, GL_FRAGMENT_SHADER, kFs);
    glAttachShader(prog_, vs), glAttachShader(prog_, fs);
    GLuint at = 0;
    for (const char *name : {"pos", "nrm", "uv", "joints", "weights"}) glBindAttribLocation(prog_, at++, name);
    glLinkProgram(prog_);
    glDeleteShader(vs), glDeleteShader(fs);   // the program keeps them
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(prog_, sizeof log, nullptr, log);
        std::fprintf(stderr, "%s: program: %s\n", who, log);
        return false;
    }
    bones_ = glGetUniformLocation(prog_, "bones"), clip_ = glGetUniformLocation(prog_, "clip");
    light_ = glGetUniformLocation(prog_, "light"), color_ = glGetUniformLocation(prog_, "color"), look_ = glGetUniformLocation(prog_, "look");
    const char *ext = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    npot_ = ext && std::strstr(ext, "GL_OES_texture_npot");
    const uint8_t white[4] = {255, 255, 255, 255};   // what a material with no texture samples
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return true;
}

void Painter::Done() {
    if (prog_) glDeleteProgram(prog_);
    if (white_) glDeleteTextures(1, &white_);
    *this = Painter();
}

void Avatar::Upload(const Painter &p, const bodyfile::Body &b) {
    Free();
    face_.emplace_back(b);
    parts_ = b.parts, materials_ = b.materials;
    const std::vector<bodymesh::Fixed> fixed = bodymesh::FixedOf(b);
    glGenBuffers(1, &pos_), glGenBuffers(1, &fixed_), glGenBuffers(1, &index_);
    glBindBuffer(GL_ARRAY_BUFFER, pos_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(pos().size() * sizeof(float)), pos().data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, fixed_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(fixed.size() * sizeof(bodymesh::Fixed)), fixed.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(b.idx.size() * sizeof(uint16_t)), b.idx.data(), GL_STATIC_DRAW);
    textures_.resize(b.textures.size());
    glGenTextures(GLsizei(textures_.size()), textures_.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t i = 0; i < textures_.size(); ++i) {
        const bodyfile::Texture &t = b.textures[i];
        // the rows go in top first, so a texture place's v runs down as the file's does
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
        const bool whole = p.npot_ || (PowerOfTwo(t.width) && PowerOfTwo(t.height));   // GLES 2 tiles and mipmaps only these
        if (whole) glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, whole ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, whole ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, whole ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    }
}

void Avatar::Shape(const float weights[bodymesh::kShapes]) {
    int first = 0, count = 0;
    if (face_.empty() || !face_[0].Set(weights, first, count)) return;
    glBindBuffer(GL_ARRAY_BUFFER, pos_);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(first) * 3 * sizeof(float)), GLsizeiptr(size_t(count) * 3 * sizeof(float)), &pos()[size_t(first) * 3]);
}

void Avatar::Draw(const Painter &p, const float bones[bodyfile::kRoles][12], const View &view) const {
    if (face_.empty()) return;
    glUseProgram(p.prog_);
    glUniform4fv(p.bones_, kBoneRows, &bones[0][0]);
    glUniformMatrix4fv(p.clip_, 1, GL_FALSE, view.clip);
    glUniform3fv(p.light_, 1, view.light);
    glBindBuffer(GL_ARRAY_BUFFER, pos_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, fixed_);
    const GLsizei stride = sizeof(bodymesh::Fixed);
    const auto field = [](size_t offset) { return reinterpret_cast<const void *>(offset); };
    glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, stride, field(offsetof(bodymesh::Fixed, nrm)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, field(offsetof(bodymesh::Fixed, uv)));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, field(offsetof(bodymesh::Fixed, bones)));
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, field(offsetof(bodymesh::Fixed, weights)));
    for (GLuint a = 0; a < 5; ++a) glEnableVertexAttribArray(a);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glFrontFace(view.mirrored ? GL_CW : GL_CCW);
    glCullFace(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    // the parts come solid first, then cut out, then see-through (the importer orders them): the
    // see-through ones blend over what is drawn and leave the depth as it is
    for (const bodyfile::Part &part : parts_) {
        const Material &m = materials_[part.material];
        const bool blend = m.alpha == Material::kBlend;
        if (blend) glEnable(GL_BLEND), glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        else glDisable(GL_BLEND);
        glDepthMask(blend ? GL_FALSE : GL_TRUE);
        if (m.flags & Material::kDoubleSided) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        glBindTexture(GL_TEXTURE_2D, m.texture >= 0 ? textures_[size_t(m.texture)] : p.white_);
        glUniform4fv(p.color_, 1, m.color);
        const float shade = m.flags & Material::kUnlit ? 2.0f : m.flags & Material::kToon ? 1.0f : 0.0f;
        glUniform4f(p.look_, shade, m.alpha == Material::kMask ? m.cutoff : -1.0f, blend ? 0.0f : 1.0f, 0.0f);
        glDrawElements(GL_TRIANGLES, GLsizei(part.count * 3), GL_UNSIGNED_SHORT, field(size_t(part.first) * 3 * sizeof(uint16_t)));
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND), glDisable(GL_CULL_FACE), glDisable(GL_DEPTH_TEST);
    glFrontFace(GL_CCW);
    for (GLuint a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
}

void Avatar::Free() {
    if (pos_) glDeleteBuffers(1, &pos_);
    if (fixed_) glDeleteBuffers(1, &fixed_);
    if (index_) glDeleteBuffers(1, &index_);
    if (!textures_.empty()) glDeleteTextures(GLsizei(textures_.size()), textures_.data());
    *this = Avatar();
}

}  // namespace bodygpu
