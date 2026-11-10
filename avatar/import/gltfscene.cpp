// glTF, GLB and VRM into a Scene (scene.h), with cgltf. A VRM is a GLB with an extension that
// names its humanoid's bones and its licence: VRM 0.x's "VRM" and VRM 1.0's "VRMC_vrm" are both
// read, into 1.0's names.
//
// Every skinned triangle is taken, in the rest pose the file's nodes hold: each vertex moved by
// its joints (the nodes' transforms times the skin's inverse bind matrices), as a viewer would
// show the model before any animation. Meshes without a skin are left out.
#include "scene.h"

#include <cstring>
#include <memory>
#include <sys/stat.h>

#include <json/json.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace importer {
namespace {

struct Mat {
    double m[3][4];
    Vec point(const Vec &v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3], m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3]};
    }
    Vec dir(const Vec &v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

// A column-major 4x4 (glTF's) as an affine 3x4.
Mat FromGl(const cgltf_float *g) {
    Mat a;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) a.m[r][c] = g[c * 4 + r];
    return a;
}

Mat Mul(const Mat &a, const Mat &b) {
    Mat o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + (c == 3 ? a.m[r][3] : 0);
    return o;
}

struct Freed {
    cgltf_data *d = nullptr;
    ~Freed() { cgltf_free(d); }
};

// A buffer may be inside the file (a GLB's, a data: URI) or a file beside it, named plainly: not
// one somewhere else on the disk, whose bytes would end up in the model.
bool BuffersStayHome(const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->buffers_count; ++i) {
        const char *uri = d->buffers[i].uri;
        if (!uri || !std::strncmp(uri, "data:", 5)) continue;
        if (std::strchr(uri, '/') || std::strchr(uri, '\\') || std::strstr(uri, "..") || std::strchr(uri, '%') || std::strchr(uri, ':'))
            return false;
    }
    return true;
}

// VRM's humanoid and licence, from whichever of its two extensions the file has.
void ReadVrm(const cgltf_data *d, Scene &out) {
    for (cgltf_size i = 0; i < d->data_extensions_count; ++i) {
        const cgltf_extension &e = d->data_extensions[i];
        const bool v0 = !std::strcmp(e.name, "VRM"), v1 = !std::strcmp(e.name, "VRMC_vrm");
        if ((!v0 && !v1) || !e.data) continue;
        Json::Value root;
        Json::CharReaderBuilder b;
        b["stackLimit"] = 64;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(b.newCharReader());
        if (!reader->parse(e.data, e.data + std::strlen(e.data), &root, &errs) || !root.isObject()) continue;
        const auto bone = [&](std::string name, const Json::Value &node) {
            if (!node.isUInt() || node.asUInt() >= d->nodes_count) return;
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
        } else if (v1 && bones.isObject()) {
            for (const std::string &name : bones.getMemberNames())
                if (bones[name].isObject()) bone(name, bones[name]["node"]);
        }
        const Json::Value &meta = root["meta"];
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
        return;
    }
}

}  // namespace

bool LoadGltf(const std::string &path, Scene &out, std::string &err) {
    out = Scene();
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return err = "the file can't be read", false;
    if (size_t(st.st_size) > kMaxSourceBytes) return err = "the file is too large", false;
    cgltf_options options = {};
    Freed f;
    if (cgltf_parse_file(&options, path.c_str(), &f.d) != cgltf_result_success) return err = "not a glTF, GLB or VRM file", false;
    cgltf_data *d = f.d;
    if (!BuffersStayHome(d)) return err = "its data is in a file somewhere else", false;
    if (cgltf_load_buffers(&options, d, path.c_str()) != cgltf_result_success) return err = "its data can't be read", false;
    if (cgltf_validate(d) != cgltf_result_success) return err = "the file is damaged", false;
    if (d->nodes_count > kMaxNodes) return err = "too many nodes", false;

    std::vector<Mat> world(d->nodes_count);
    out.nodes.resize(d->nodes_count);
    for (cgltf_size i = 0; i < d->nodes_count; ++i) {
        cgltf_float g[16];
        cgltf_node_transform_world(&d->nodes[i], g);
        world[i] = FromGl(g);
        Node &n = out.nodes[i];
        n.name = modelfile::Plain(d->nodes[i].name ? d->nodes[i].name : "");
        n.parent = d->nodes[i].parent ? int(d->nodes[i].parent - d->nodes) : -1;
        n.at = {world[i].m[0][3], world[i].m[1][3], world[i].m[2][3]};
        if (!n.at.finite()) return err = "a node's place isn't a number", false;
    }
    out.info.format = "gltf";
    ReadVrm(d, out);

    for (cgltf_size ni = 0; ni < d->nodes_count; ++ni) {
        const cgltf_node &node = d->nodes[ni];
        if (!node.mesh || !node.skin || !node.skin->inverse_bind_matrices) continue;
        const cgltf_skin &skin = *node.skin;
        if (skin.inverse_bind_matrices->count < skin.joints_count) return err = "a skin is short of bind matrices", false;
        std::vector<Mat> move(skin.joints_count);   // each joint's: the mesh's space -> the rest pose
        std::vector<int> jointNode(skin.joints_count);
        for (cgltf_size k = 0; k < skin.joints_count; ++k) {
            cgltf_float ibm[16];
            if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, k, ibm, 16)) return err = "a skin's bind matrix can't be read", false;
            jointNode[k] = int(skin.joints[k] - d->nodes);
            move[k] = Mul(world[size_t(jointNode[k])], FromGl(ibm));
        }
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive &prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *joints[2] = {}, *weights[2] = {};
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute &at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position && at.index == 0) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal && at.index == 0) nrm = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index < 2) joints[at.index] = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index < 2) weights[at.index] = at.data;
            }
            if (!pos || !joints[0] || !weights[0]) continue;
            const size_t count = pos->count, first = out.verts.size();
            if (first + count > kMaxVerts) return err = "too many vertices", false;
            for (const cgltf_accessor *a : {nrm, joints[0], weights[0], joints[1], weights[1]})
                if (a && a->count != count) return err = "a mesh's arrays don't match", false;
            if (bool(joints[1]) != bool(weights[1])) return err = "a mesh's arrays don't match", false;
            out.verts.resize(first + count);
            for (size_t i = 0; i < count; ++i) {
                cgltf_float p[3] = {}, n[3] = {};
                if (!cgltf_accessor_read_float(pos, i, p, 3) || (nrm && !cgltf_accessor_read_float(nrm, i, n, 3)))
                    return err = "a vertex can't be read", false;
                cgltf_uint j[kInfluences] = {};
                cgltf_float w[kInfluences] = {};
                for (int set = 0; set < 2 && joints[set]; ++set)
                    if (!cgltf_accessor_read_uint(joints[set], i, j + 4 * set, 4) || !cgltf_accessor_read_float(weights[set], i, w + 4 * set, 4))
                        return err = "a vertex's joints can't be read", false;
                double total = 0;
                for (int k = 0; k < kInfluences; ++k) {
                    if (!(w[k] > 0) || !std::isfinite(w[k])) w[k] = 0;
                    else if (j[k] >= skin.joints_count) return err = "a vertex names a joint its skin doesn't have", false;
                    total += w[k];
                }
                Vertex &v = out.verts[first + i];
                v = Vertex();
                for (int k = 0; k < kInfluences; ++k) v.node[k] = -1, v.weight[k] = 0;
                if (total <= 0) continue;   // no joint moves it: it stays out of every part (weights all 0)
                const Vec at = {p[0], p[1], p[2]}, normal = {n[0], n[1], n[2]};
                for (int k = 0; k < kInfluences; ++k) {
                    if (w[k] <= 0) continue;
                    const double share = w[k] / total;
                    v.p = v.p + move[j[k]].point(at) * share;
                    v.n = v.n + move[j[k]].dir(normal) * share;
                    v.node[k] = jointNode[j[k]], v.weight[k] = float(share);
                }
                v.n = v.n.unit();
                if (!v.p.finite()) return err = "a vertex's place isn't a number", false;
            }
            const size_t corners = prim.indices ? prim.indices->count : count;
            if (corners % 3 || out.tris.size() + corners > kMaxTris * 3) return err = "too many triangles, or not whole ones", false;
            for (size_t c = 0; c < corners; ++c) {
                const size_t i = prim.indices ? cgltf_accessor_read_index(prim.indices, c) : c;
                if (i >= count) return err = "a triangle names a vertex its mesh doesn't have", false;
                out.tris.push_back(uint32_t(first + i));
            }
        }
    }
    if (out.tris.empty()) return err = "the file has no skinned mesh", false;
    return true;
}

}  // namespace importer
