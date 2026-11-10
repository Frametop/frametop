// A body's mesh (bodyfile.h) as ft-avatars' vertex buffers hold it, the CPU side: what stays put
// for each vertex, the vertices' places as the face shapes move them, and a vertex skinned here
// rather than on the GPU (to find where a posed body is, and for the tests).
//
// No GPU calls here.
#pragma once

#include "../bodyfile.h"

#include <cstdint>
#include <vector>

namespace bodymesh {

// What doesn't move with the face, one per vertex.
struct Fixed {
    int8_t nrm[4];        // out of 127; the fourth is padding
    float uv[2];
    uint8_t bones[4];     // roles
    uint8_t weights[4];   // adding up to 255
};
std::vector<Fixed> FixedOf(const bodyfile::Body &b);

constexpr int kShapes = bodyfile::Shape::kLastId + 1;   // weights go by Shape::Id: [0] is unused

// The vertices' places at rest, in metres, moved by the face shapes.
class Face {
public:
    explicit Face(const bodyfile::Body &b);
    // The shapes at these weights (0 to 1 each; a shape the body lacks is skipped). False when
    // nothing moved; else the run of vertices that did (`first`, `count`), to upload again.
    bool Set(const float weights[kShapes], int &first, int &count);
    const std::vector<float> &pos() const { return pos_; }   // vertices x 3
    bool has(uint32_t shape) const { return shape < uint32_t(kShapes) && of_[shape] >= 0; }

private:
    std::vector<float> rest_, pos_;
    std::vector<bodyfile::Shape> shapes_;
    float unit_ = 0;
    int of_[kShapes];             // a shape id's place in shapes_, or -1
    float set_[kShapes] = {};
    int first_ = 0, end_ = 0;     // the run the weights now set moved
};

// Vertex i (its place `at`, metres) under a pose's skinning matrices (bodypose::Solver::Matrices).
void Skinned(const bodyfile::Body &b, const float at[3], const float bones[bodyfile::kRoles][12], int i, float out[3]);
// The box around every `step`th vertex of the posed body. False for a body with no vertices.
bool Bounds(const bodyfile::Body &b, const std::vector<float> &pos, const float bones[bodyfile::kRoles][12], int step, float lo[3], float hi[3]);

}  // namespace bodymesh
