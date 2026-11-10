// Fewer triangles for a mesh that has too many (meshoptimizer's simplifier): the ones left use
// the same vertices, so whatever each vertex carries (its joints and weights) stays true.
#pragma once

#include <cstdint>
#include <vector>

namespace importer {

// `tris` (three vertices each, into `positions`: x y z each) cut down to about `maxTris`
// triangles. The shape is kept as far as it can be; a mesh that can't be brought down that way
// (many loose pieces) is cut further without keeping its seams. Returns the triangles left.
size_t Reduce(const std::vector<float> &positions, std::vector<uint32_t> &tris, size_t maxTris);

// The same for a mesh with a picture on it: `attributes` (`perVertex` numbers a vertex: its
// place in the picture, its normal) count toward what a cut costs, so seams and creases are
// kept longer, and the vertices marked in `keep` are never taken away (the ones a face's shapes
// move). It doesn't cut across seams, so it may stop short of `maxTris`: the caller decides
// what then. Returns the triangles left.
size_t ReduceKeepingLook(const std::vector<float> &positions, const std::vector<float> &attributes, size_t perVertex,
                         const std::vector<float> &attributeWeights, const std::vector<uint8_t> &keep, std::vector<uint32_t> &tris, size_t maxTris);

}  // namespace importer
