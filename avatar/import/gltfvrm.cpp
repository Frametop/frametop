// VRM's own fields in a glTF (gltfparts.h): 0.x's "VRM" extension and 1.0's "VRMC_vrm" are both
// read, into 1.0's names.
#include "gltfparts.h"

#include <cstring>
#include <memory>

#include <json/json.h>

#include "../bodyfile.h"
#include "shapenames.h"

namespace importer {
namespace {

using Shape = bodyfile::Shape;

bool Parse(const char *json, Json::Value &root) {
    Json::CharReaderBuilder b;
    b["stackLimit"] = 64;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    return json && reader->parse(json, json + std::strlen(json), &root, &errs) && root.isObject();
}

// 0.x's presets and 1.0's, as Frametop's shapes.
uint32_t ShapeOfPreset(const std::string &preset) {
    static const struct {
        const char *v0, *v1;
        uint32_t id;
    } kPresets[] = {{"a", "aa", Shape::kAa}, {"i", "ih", Shape::kIh}, {"u", "ou", Shape::kOu}, {"e", "ee", Shape::kEe}, {"o", "oh", Shape::kOh},
                    {"blink", "blink", Shape::kBlink}, {"blink_l", "blinkLeft", Shape::kBlinkLeft}, {"blink_r", "blinkRight", Shape::kBlinkRight}};
    for (const auto &p : kPresets)
        if (preset == p.v0 || preset == p.v1) return p.id;
    return 0;
}

void ReadHumanoid(const Json::Value &root, bool v0, size_t nodes, Scene &out) {
    const auto bone = [&](std::string name, const Json::Value &node) {
        if (!node.isUInt() || node.asUInt() >= nodes) return;
        if (v0) {   // 0.x's thumb bones are 1.0's, one name along
            const size_t at = name.find("Thumb");
            if (at != std::string::npos) {
                const std::string part = name.substr(at + 5);
                if (part == "Proximal") name.replace(at + 5, std::string::npos, "Metacarpal");
                else if (part == "Intermediate") name.replace(at + 5, std::string::npos, "Proximal");
            }
        }
        out.humanoid[name] = int(node.asUInt());
    };
    const Json::Value &bones = root["humanoid"]["humanBones"];
    if (v0 && bones.isArray()) {
        for (const Json::Value &hb : bones)
            if (hb.isObject() && hb["bone"].isString()) bone(hb["bone"].asString(), hb["node"]);
    } else if (!v0 && bones.isObject()) {
        for (const std::string &name : bones.getMemberNames())
            if (bones[name].isObject()) bone(name, bones[name]["node"]);
    }
}

void ReadMeta(const Json::Value &meta, bool v0, Scene &out) {
    const auto text = [&](const Json::Value &v) { return v.isString() ? modelfile::Plain(v.asString()) : std::string(); };
    if (v0) {
        out.info.name = text(meta["title"]), out.info.author = text(meta["author"]), out.info.licence = text(meta["licenseName"]);
        out.info.format = "vrm 0";
        // every licence 0.x names lets the file be passed on but these two
        const std::string l = out.info.licence;
        out.info.mayShare = !l.empty() && l != "Redistribution_Prohibited" && l != "Other";
    } else {
        out.info.name = text(meta["name"]), out.info.licence = text(meta["licenseUrl"]);
        if (meta["authors"].isArray() && !meta["authors"].empty()) out.info.author = text(meta["authors"][0]);
        out.info.format = "vrm 1";
        out.info.mayShare = meta["allowRedistribution"].isBool() && meta["allowRedistribution"].asBool();
    }
    out.licenceSaysNo = !out.info.mayShare;
}

// The expressions Frametop moves: 0.x binds a mesh's shape at 0 to 100, 1.0 a node's at 0 to 1.
void ReadExpressions(const Json::Value &root, bool v0, const cgltf_data *d, std::vector<MorphBind> &binds) {
    const auto bind = [&](uint32_t shape, const Json::Value &b) {
        if (!shape || !b.isObject() || !b["index"].isUInt() || !b["weight"].isNumeric()) return;
        const Json::Value &where = b[v0 ? "mesh" : "node"];
        if (!where.isUInt()) return;
        size_t mesh = where.asUInt();
        if (!v0) {
            if (mesh >= d->nodes_count || !d->nodes[mesh].mesh) return;
            mesh = size_t(d->nodes[mesh].mesh - d->meshes);
        }
        const double weight = b["weight"].asDouble() * (v0 ? 0.01 : 1);
        if (mesh < d->meshes_count && weight > 0 && weight <= 1.5 && binds.size() < 4096) binds.push_back({shape, mesh, b["index"].asUInt(), weight});
    };
    if (v0) {
        const Json::Value &groups = root["blendShapeMaster"]["blendShapeGroups"];
        if (!groups.isArray()) return;
        for (const Json::Value &g : groups) {
            if (!g.isObject() || !g["presetName"].isString() || !g["binds"].isArray()) continue;
            for (const Json::Value &b : g["binds"]) bind(ShapeOfPreset(g["presetName"].asString()), b);
        }
    } else {
        const Json::Value &preset = root["expressions"]["preset"];
        if (!preset.isObject()) return;
        for (const std::string &name : preset.getMemberNames()) {
            const Json::Value &binds1 = preset[name].isObject() ? preset[name]["morphTargetBinds"] : Json::Value::nullSingleton();
            if (!binds1.isArray()) continue;
            for (const Json::Value &b : binds1) bind(ShapeOfPreset(name), b);
        }
    }
}

// The materials VRM shades as a cartoon: 0.x lists each material's shader, 1.0 marks the material.
void ReadToon(const Json::Value &root, bool v0, const cgltf_data *d, Scene &out) {
    for (size_t i = 0; i < out.materials.size() && i < d->materials_count; ++i) {
        bool toon = false;
        if (v0) {
            const Json::Value &props = root["materialProperties"];
            toon = props.isArray() && i < props.size() && props[Json::ArrayIndex(i)].isObject() && props[Json::ArrayIndex(i)]["shader"].isString() &&
                   props[Json::ArrayIndex(i)]["shader"].asString() == "VRM/MToon";
        } else {
            for (cgltf_size k = 0; k < d->materials[i].extensions_count; ++k)
                toon = toon || !std::strcmp(d->materials[i].extensions[k].name, "VRMC_materials_mtoon");
        }
        if (toon) out.materials[i].toon = true, out.materials[i].unlit = false;   // 0.x writes them as unlit for other readers
    }
}

}  // namespace

bool ReadVrm(const cgltf_data *d, Scene &out, std::vector<MorphBind> &binds) {
    for (cgltf_size i = 0; i < d->data_extensions_count; ++i) {
        const cgltf_extension &e = d->data_extensions[i];
        const bool v0 = !std::strcmp(e.name, "VRM"), v1 = !std::strcmp(e.name, "VRMC_vrm");
        Json::Value root;
        if ((!v0 && !v1) || !Parse(e.data, root)) continue;
        ReadHumanoid(root, v0, d->nodes_count, out);
        ReadMeta(root["meta"], v0, out);
        ReadExpressions(root, v0, d, binds);
        ReadToon(root, v0, d, out);
        return true;
    }
    return false;
}

void NamedMorphs(const cgltf_data *d, std::vector<MorphBind> &binds) {
    for (cgltf_size m = 0; m < d->meshes_count; ++m)
        for (cgltf_size t = 0; t < d->meshes[m].target_names_count && binds.size() < 4096; ++t) {
            const uint32_t shape = ShapeOfName(GltfText(d->meshes[m].target_names[t]));
            if (shape) binds.push_back({shape, size_t(m), size_t(t), 1.0});
        }
}

}  // namespace importer
