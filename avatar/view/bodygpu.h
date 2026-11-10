// A body (bodyfile.h) drawn with GLES 2: its mesh and textures on the GPU, skinned there by a
// pose's 55 matrices, each part with its material's look (lit, two-tone or in its own colours;
// solid, cut out or see-through). The context is the caller's, and so is where it draws: the
// framebuffer bound, its viewport, and clearing it (colour and depth).
#pragma once

#include "../bodyfile.h"
#include "bodymesh.h"

#include <GLES2/gl2.h>

#include <vector>

namespace bodygpu {

struct View {
    float clip[16];           // the room -> clip space (GL's order)
    float light[3];           // toward the light, in the room
    bool mirrored = false;    // the view turns the room over, as a mirror does
};

// The program bodies are drawn with: one for a context.
class Painter {
public:
    // False, with a line on stderr starting with `who`, when the GPU can't (a shader, or too
    // few uniforms for the bones).
    bool Init(const char *who);
    void Done();

private:
    friend class Avatar;
    GLuint prog_ = 0, white_ = 0;
    GLint bones_ = -1, clip_ = -1, light_ = -1, color_ = -1, look_ = -1;
    bool npot_ = false;
};

class Avatar {
public:
    void Upload(const Painter &p, const bodyfile::Body &b);
    // The face shapes at these weights (bodymesh::Face::Set), from the next draw on.
    void Shape(const float weights[bodymesh::kShapes]);
    bool has(uint32_t shape) const { return !face_.empty() && face_[0].has(shape); }
    const std::vector<float> &pos() const { return face_[0].pos(); }   // the vertices' places now (after Upload)
    // The solid and cut-out parts, then the see-through ones over them.
    void Draw(const Painter &p, const float bones[bodyfile::kRoles][12], const View &view) const;
    void Free();

private:
    std::vector<bodymesh::Face> face_;   // one, once uploaded
    std::vector<bodyfile::Part> parts_;
    std::vector<bodyfile::Material> materials_;
    std::vector<GLuint> textures_;
    GLuint pos_ = 0, fixed_ = 0, index_ = 0;
};

}  // namespace bodygpu
