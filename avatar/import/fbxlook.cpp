// See fbxparts.h. An FBX's materials say little that holds across the programs that write them:
// the colour, its picture and whether it is see-through are taken.
#include "fbxparts.h"

#include <map>

#include "../bodyfile.h"
#include "image.h"

namespace importer {
namespace {

// A file's name without the folders the FBX says it was in (either kind of slash).
std::string BaseName(const ufbx_string &path) {
    const std::string p(path.data, path.length);
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool Decode(const ufbx_texture &t, const std::string &folder, int maxSide, Image &out) {
    if (t.content.size > 0 && DecodeImage(static_cast<const uint8_t *>(t.content.data), t.content.size, maxSide, out)) return true;
    for (const ufbx_string *name : {&t.relative_filename, &t.filename, &t.absolute_filename}) {
        const std::string base = BaseName(*name);
        if (!base.empty() && DecodeImageFile(folder, base.c_str(), maxSide, out)) return true;
    }
    return false;
}

}  // namespace

bool ReadFbxLook(const ufbx_scene *scene, const std::string &folder, Scene &out, std::string &err) {
    if (scene->materials.count > kMaxMaterials) return err = "too many materials", false;
    std::map<const ufbx_texture *, int> read;   // each picture's place in the scene, -1: unreadable
    out.materials.resize(scene->materials.count);
    for (size_t i = 0; i < scene->materials.count; ++i) {
        const ufbx_material &m = *scene->materials.data[i];
        SceneMaterial &o = out.materials[i];
        const ufbx_material_map &color = m.pbr.base_color.has_value || m.pbr.base_color.texture ? m.pbr.base_color : m.fbx.diffuse_color;
        const ufbx_texture *texture = color.texture;
        if (texture) {
            // the picture is the colour: the tint beside it is seldom meant (a grey left by the program that wrote the file)
            auto found = read.find(texture);
            if (found == read.end()) {
                Image im;
                int place = -1;
                if (out.images.size() < kMaxImages && Decode(*texture, folder, out.ImageSide(), im)) {
                    place = int(out.images.size());
                    out.images.push_back(std::move(im));
                } else {
                    ++out.unreadImages;
                }
                found = read.emplace(texture, place).first;
            }
            o.image = found->second;
        }
        if (color.has_value && (!texture || o.image < 0)) {
            const double c[3] = {color.value_vec4.x, color.value_vec4.y, color.value_vec4.z};
            for (int k = 0; k < 3; ++k) o.color[k] = std::isfinite(c[k]) ? float(c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]) : 1;
        }
        // a picture with see-through parts is cut out along them: blending needs an order an FBX doesn't give
        if (o.image >= 0) {
            const Image &im = out.images[size_t(o.image)];
            for (size_t p = 3; p < im.rgba.size() && o.alpha == 0; p += 4)
                if (im.rgba[p] < 128) o.alpha = bodyfile::Material::kMask;
        }
    }
    return true;
}

}  // namespace importer
