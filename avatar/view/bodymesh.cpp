// A body's mesh for the vertex buffers (bodymesh.h).
#include "bodymesh.h"

#include <algorithm>
#include <cstring>

namespace bodymesh {

std::vector<Fixed> FixedOf(const bodyfile::Body &b) {
    std::vector<Fixed> out(size_t(b.verts()));
    for (size_t v = 0; v < out.size(); ++v) {
        Fixed &f = out[v];
        f.nrm[3] = 0;
        for (int k = 0; k < 3; ++k) f.nrm[k] = b.nrm[v * 3 + k];
        for (int k = 0; k < 2; ++k) f.uv[k] = b.uv[v * 2 + k];
        for (int k = 0; k < 4; ++k) f.bones[k] = b.bones[v * 4 + k], f.weights[k] = b.weights[v * 4 + k];
    }
    return out;
}

Face::Face(const bodyfile::Body &b) : rest_(b.pos.size()), shapes_(b.shapes), unit_(b.unit) {
    for (size_t i = 0; i < rest_.size(); ++i) rest_[i] = float(b.pos[i]) * b.unit;
    pos_ = rest_;
    std::fill(of_, of_ + kShapes, -1);
    for (size_t s = 0; s < shapes_.size(); ++s)
        if (shapes_[s].id < uint32_t(kShapes) && !shapes_[s].verts.empty()) of_[shapes_[s].id] = int(s);
}

bool Face::Set(const float weights[kShapes], int &first, int &count) {
    float want[kShapes] = {};
    for (int id = 1; id < kShapes; ++id)
        if (of_[id] >= 0 && weights[id] > 0) want[id] = std::min(weights[id], 1.0f);
    if (!std::memcmp(want, set_, sizeof want)) return false;
    // back to rest where the weights before moved it, then out again by these
    std::copy(rest_.begin() + first_ * 3, rest_.begin() + end_ * 3, pos_.begin() + first_ * 3);
    int lo = first_, hi = end_;
    first_ = end_ = 0;
    for (int id = 1; id < kShapes; ++id) {
        if (!(want[id] > 0)) continue;
        const bodyfile::Shape &s = shapes_[size_t(of_[id])];
        const float k = want[id] * unit_;
        for (size_t i = 0; i < s.verts.size(); ++i)
            for (int c = 0; c < 3; ++c) pos_[size_t(s.verts[i]) * 3 + c] += k * float(s.delta[i * 3 + c]);
        const int from = s.verts.front(), to = s.verts.back() + 1;   // they rise
        if (first_ == end_) first_ = from, end_ = to;
        else first_ = std::min(first_, from), end_ = std::max(end_, to);
    }
    if (first_ != end_) {
        if (lo == hi) lo = first_, hi = end_;
        else lo = std::min(lo, first_), hi = std::max(hi, end_);
    }
    std::memcpy(set_, want, sizeof want);
    first = lo, count = hi - lo;
    return count > 0;
}

void Skinned(const bodyfile::Body &b, const float at[3], const float bones[bodyfile::kRoles][12], int i, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 4; ++k) {
        const float w = float(b.weights[size_t(i) * 4 + k]) / 255.0f;
        if (w <= 0) continue;
        const float *m = bones[b.bones[size_t(i) * 4 + k]];
        for (int r = 0; r < 3; ++r) out[r] += w * (m[r * 4] * at[0] + m[r * 4 + 1] * at[1] + m[r * 4 + 2] * at[2] + m[r * 4 + 3]);
    }
}

bool Bounds(const bodyfile::Body &b, const std::vector<float> &pos, const float bones[bodyfile::kRoles][12], int step, float lo[3], float hi[3]) {
    const int n = std::min(b.verts(), int(pos.size() / 3));
    for (int i = 0; i < n; i += std::max(1, step)) {
        float p[3];
        Skinned(b, &pos[size_t(i) * 3], bones, i, p);
        for (int k = 0; k < 3; ++k) lo[k] = i ? std::min(lo[k], p[k]) : p[k], hi[k] = i ? std::max(hi[k], p[k]) : p[k];
    }
    return n > 0;
}

}  // namespace bodymesh
