// See reduce.h.
#include "reduce.h"

#include "meshoptimizer.h"

namespace importer {

size_t Reduce(const std::vector<float> &positions, std::vector<uint32_t> &tris, size_t maxTris) {
    if (tris.size() / 3 <= maxTris) return tris.size() / 3;
    const size_t verts = positions.size() / 3, target = maxTris * 3;
    std::vector<uint32_t> out(tris.size());
    // any error is allowed: the count is the limit that must be kept
    size_t n = meshopt_simplify(out.data(), tris.data(), tris.size(), positions.data(), verts, 3 * sizeof(float), target, 1.0f, 0, nullptr);
    if (n > target + target / 4) {
        // stopped short (it won't join pieces or close holes): the sloppy pass will
        n = meshopt_simplifySloppy(out.data(), tris.data(), tris.size(), positions.data(), verts, 3 * sizeof(float), nullptr, target, 1.0f, nullptr);
    }
    out.resize(n);
    tris = out;
    return n / 3;
}

size_t ReduceKeepingLook(const std::vector<float> &positions, const std::vector<float> &attributes, size_t perVertex,
                         const std::vector<float> &attributeWeights, const std::vector<uint8_t> &keep, std::vector<uint32_t> &tris, size_t maxTris) {
    if (tris.size() / 3 <= maxTris) return tris.size() / 3;
    const size_t verts = positions.size() / 3, target = maxTris * 3;
    std::vector<uint32_t> out(tris.size());
    std::vector<unsigned char> lock(verts, 0);
    for (size_t i = 0; i < verts && i < keep.size(); ++i) lock[i] = keep[i] ? meshopt_SimplifyVertex_Lock : 0;
    // small loose pieces (a strand of hair, a button) go before the body's shape does
    const size_t n = meshopt_simplifyWithAttributes(out.data(), tris.data(), tris.size(), positions.data(), verts, 3 * sizeof(float), attributes.data(),
                                                    perVertex * sizeof(float), attributeWeights.data(), perVertex, lock.data(), target, 1.0f,
                                                    meshopt_SimplifyPrune, nullptr);
    out.resize(n);
    tris = out;
    return n / 3;
}

}  // namespace importer
