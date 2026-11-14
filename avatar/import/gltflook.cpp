// A glTF's materials and the pictures their colours use (gltfparts.h). Only what Frametop
// draws is taken: the base colour and its picture, how the material is see-through, whether it
// has two sides and whether it is lit. A picture is one inside the file (a GLB's, a data: URI)
// or a file beside it.
#include "gltfparts.h"

#include <cstring>
#include <map>

#include "../bodyfile.h"
#include "image.h"

namespace importer {
namespace {

constexpr size_t kMaxImageFile = 64u << 20;

// base64 (a data: URI's) into bytes; false for anything that isn't.
bool FromBase64(const char *text, std::vector<uint8_t> &out) {
    unsigned acc = 0;
    int bits = 0;
    for (const char *c = text; *c && *c != '='; ++c) {
        int v;
        if (*c >= 'A' && *c <= 'Z') v = *c - 'A';
        else if (*c >= 'a' && *c <= 'z') v = *c - 'a' + 26;
        else if (*c >= '0' && *c <= '9') v = *c - '0' + 52;
        else if (*c == '+') v = 62;
        else if (*c == '/') v = 63;
        else return false;
        acc = (acc << 6) | unsigned(v), bits += 6;
        if (bits >= 8) bits -= 8, out.push_back(uint8_t(acc >> bits));
        if (out.size() > kMaxImageFile) return false;
    }
    return !out.empty();
}

bool Decode(const cgltf_image &im, const std::string &folder, int maxSide, Image &out) {
    if (im.buffer_view) {
        const cgltf_buffer_view &view = *im.buffer_view;
        if (!view.buffer || !view.buffer->data || view.offset > view.buffer->size || view.size > view.buffer->size - view.offset) return false;
        return DecodeImage(static_cast<const uint8_t *>(view.buffer->data) + view.offset, view.size, maxSide, out);
    }
    if (!im.uri) return false;
    std::vector<uint8_t> bytes;
    if (!std::strncmp(im.uri, "data:", 5)) {
        const char *comma = std::strchr(im.uri, ',');
        if (!comma || !std::strstr(im.uri, ";base64") || !FromBase64(comma + 1, bytes)) return false;
    } else {
        return DecodeImageFile(folder, im.uri, maxSide, out);
    }
    return DecodeImage(bytes.data(), bytes.size(), maxSide, out);
}

}  // namespace

bool ReadLook(const cgltf_data *d, const std::string &folder, Scene &out, std::string &err) {
    if (d->materials_count > kMaxMaterials) return err = "too many materials", false;
    std::map<const cgltf_image *, int> read;   // each picture's place in the scene, -1: unreadable
    out.materials.resize(d->materials_count);
    for (cgltf_size i = 0; i < d->materials_count; ++i) {
        const cgltf_material &m = d->materials[i];
        SceneMaterial &o = out.materials[i];
        o.doubleSided = m.double_sided;
        o.unlit = m.unlit;
        o.alpha = m.alpha_mode == cgltf_alpha_mode_mask ? bodyfile::Material::kMask : m.alpha_mode == cgltf_alpha_mode_blend ? bodyfile::Material::kBlend : bodyfile::Material::kOpaque;
        if (std::isfinite(m.alpha_cutoff)) o.cutoff = m.alpha_cutoff < 0 ? 0 : m.alpha_cutoff > 1 ? 1 : m.alpha_cutoff;
        if (!m.has_pbr_metallic_roughness) continue;
        for (int c = 0; c < 4; ++c) {
            const float v = m.pbr_metallic_roughness.base_color_factor[c];
            o.color[c] = std::isfinite(v) ? (v < 0 ? 0 : v > 1 ? 1 : v) : 1;
        }
        const cgltf_texture *texture = m.pbr_metallic_roughness.base_color_texture.texture;
        if (!texture || !texture->image) continue;
        auto found = read.find(texture->image);
        if (found == read.end()) {
            Image im;
            int place = -1;
            if (out.images.size() < kMaxImages && Decode(*texture->image, folder, out.ImageSide(), im)) {
                place = int(out.images.size());
                out.images.push_back(std::move(im));
            } else {
                ++out.unreadImages;
            }
            found = read.emplace(texture->image, place).first;
        }
        o.image = found->second;
    }
    return true;
}

}  // namespace importer
