// See bodylook.h.
#include "bodylook.h"

#include <cstring>

#include "image.h"

namespace importer {
namespace {

bool Same(const bodyfile::Material &a, const bodyfile::Material &b) {
    return !std::memcmp(a.color, b.color, sizeof a.color) && a.texture == b.texture && a.alpha == b.alpha && a.cutoff == b.cutoff && a.flags == b.flags;
}

// A picture's place among the ones kept: a file may hold the same picture more than once (an
// FBX names one for each material that uses it).
int Kept(std::vector<Image> &images, const Image &im) {
    for (size_t i = 0; i < images.size(); ++i)
        if (images[i].width == im.width && images[i].height == im.height && images[i].rgba == im.rgba) return int(i);
    images.push_back(im);
    return int(images.size() - 1);
}

}  // namespace

bool CutLook(const Scene &s, bodyfile::Body &out, std::vector<uint32_t> &materialOf, std::string &why) {
    using bodyfile::Material;
    std::vector<bool> used(s.materials.size() + 1, false);
    for (const uint32_t vi : s.tris) {
        const int m = s.verts[vi].material;
        used[m >= 0 && m < int(s.materials.size()) ? size_t(m) : s.materials.size()] = true;
    }
    std::vector<int> textureOf(s.images.size(), -1);   // each picture's place among the body's
    std::vector<Image> images;
    materialOf.assign(s.materials.size() + 1, 0);
    for (size_t i = 0; i <= s.materials.size(); ++i) {
        if (!used[i]) continue;
        Material m;
        if (i < s.materials.size()) {
            const SceneMaterial &from = s.materials[i];
            std::memcpy(m.color, from.color, sizeof m.color);
            m.alpha = from.alpha <= Material::kBlend ? from.alpha : Material::kOpaque;
            m.cutoff = m.alpha == Material::kMask ? from.cutoff : 0.5f;
            m.flags = (from.doubleSided ? uint32_t(Material::kDoubleSided) : 0u) | (from.toon ? uint32_t(Material::kToon) : from.unlit ? uint32_t(Material::kUnlit) : 0u);
            if (from.image >= 0 && from.image < int(s.images.size()) && !s.images[size_t(from.image)].rgba.empty()) {
                int &place = textureOf[size_t(from.image)];
                if (place < 0) place = Kept(images, s.images[size_t(from.image)]);
                m.texture = place;
            }
        }
        size_t at = 0;
        while (at < out.materials.size() && !Same(out.materials[at], m)) ++at;
        if (at == out.materials.size()) out.materials.push_back(m);
        materialOf[i] = uint32_t(at);
    }
    if (out.materials.size() > size_t(bodyfile::kMaxParts)) return why = "it has more than " + std::to_string(bodyfile::kMaxParts) + " materials", false;
    if (images.size() > size_t(bodyfile::kMaxTextures)) return why = "it has more than " + std::to_string(bodyfile::kMaxTextures) + " pictures", false;
    for (;;) {
        size_t bytes = 0, largest = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            bytes += images[i].rgba.size();
            if (images[i].rgba.size() > images[largest].rgba.size()) largest = i;
        }
        if (bytes <= kTextureBudget || images[largest].rgba.size() <= 4) break;
        Halve(images[largest]);
    }
    for (Image &im : images) {
        bodyfile::Texture t;
        t.width = im.width, t.height = im.height;
        t.rgba = std::move(im.rgba);
        out.textures.push_back(std::move(t));
    }
    return true;
}

}  // namespace importer
