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

}  // namespace importer
