// glTF, GLB and VRM into a Scene (scene.h), with cgltf. A VRM is a GLB with an extension that
// names its humanoid's bones, its licence and its expressions (gltfvrm.cpp).
//
// Every triangle is taken, in the rest pose the file's nodes hold: each vertex of a skinned mesh
// moved by its joints (the nodes' transforms times the skin's inverse bind matrices), as a
// viewer would show the model before any animation; a mesh without a skin moves with its node.
// The face shapes Frametop moves are taken from the meshes' morph targets, each vertex's move
// turned as its vertex was.
#include <cstring>
#include <sys/stat.h>

#define CGLTF_IMPLEMENTATION
#include "gltfparts.h"
#include "image.h"

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

bool BuffersStayHome(const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->buffers_count; ++i) {
        const char *uri = d->buffers[i].uri;
        if (uri && std::strncmp(uri, "data:", 5) != 0 && !PlainFileName(uri)) return false;
    }
    return true;
}

Mat Scaled(const Mat &a, double k) {
    Mat o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[r][c] * k;
    return o;
}

Mat Sum(const Mat &a, const Mat &b) {
    Mat o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[r][c] + b.m[r][c];
    return o;
}

// What reading a mesh needs of the file, and where it goes.
struct Reader {
    const cgltf_data *d;
    const std::vector<Mat> &world;        // each node's
    const std::vector<MorphBind> &binds;
    Scene &out;
    std::string &err;

    bool Fail(const char *why) { return err = why, false; }

    SceneShape &ShapeOf(uint32_t id) {
        for (SceneShape &s : out.shapes)
            if (s.id == id) return s;
        out.shapes.emplace_back();
        out.shapes.back().id = id;
        return out.shapes.back();
    }

    // A primitive's shapes: each morph target bound to one of Frametop's adds its moves, turned
    // as the vertices were (`turn`: each vertex's, the primitive's first at `first`).
    bool Morphs(size_t mesh, const cgltf_primitive &prim, size_t first, const std::vector<Mat> &turn) {
        std::vector<cgltf_float> deltas;
        for (const MorphBind &b : binds) {
            if (b.mesh != mesh || b.target >= prim.targets_count) continue;
            const cgltf_accessor *moves = nullptr;
            for (cgltf_size a = 0; a < prim.targets[b.target].attributes_count; ++a)
                if (prim.targets[b.target].attributes[a].type == cgltf_attribute_type_position) moves = prim.targets[b.target].attributes[a].data;
            if (!moves) continue;
            if (moves->count != turn.size() || moves->type != cgltf_type_vec3) return Fail("a shape's array doesn't match its mesh");
            deltas.assign(turn.size() * 3, 0);
            if (cgltf_accessor_unpack_floats(moves, deltas.data(), deltas.size()) != deltas.size()) return Fail("a shape can't be read");
            SceneShape &shape = ShapeOf(b.shape);
            for (size_t i = 0; i < turn.size(); ++i) {
                const Vec move = turn[i].dir({deltas[3 * i], deltas[3 * i + 1], deltas[3 * i + 2]}) * b.weight;
                if (!move.finite()) return Fail("a shape's move isn't a number");
                if (move.norm() < 1e-7) continue;
                Vec &sum = shape.move[uint32_t(first + i)];
                sum = sum + move;
            }
        }
        return true;
    }

    // One node's mesh. `move`: each of its skin's joints' (the mesh's space -> the rest pose);
    // no skin: the node itself moves it.
    bool Mesh(size_t ni, const std::vector<Mat> &move, const std::vector<int> &jointNode) {
        const cgltf_node &node = d->nodes[ni];
        const bool skinned = !move.empty();
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive &prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *joints[2] = {}, *weights[2] = {};
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute &at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position && at.index == 0) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal && at.index == 0) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index < 2) joints[at.index] = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index < 2) weights[at.index] = at.data;
            }
            if (!pos || (skinned && (!joints[0] || !weights[0]))) continue;
            const size_t count = pos->count, first = out.verts.size();
            if (first + count > kMaxVerts) return Fail("too many vertices");
            for (const cgltf_accessor *a : {nrm, uv, joints[0], weights[0], joints[1], weights[1]})
                if (a && a->count != count) return Fail("a mesh's arrays don't match");
            if (bool(joints[1]) != bool(weights[1])) return Fail("a mesh's arrays don't match");
            out.verts.resize(first + count);
            std::vector<Mat> turn(count, world[ni]);
            const int material = prim.material ? int(prim.material - d->materials) : -1;
            for (size_t i = 0; i < count; ++i) {
                cgltf_float p[3] = {}, n[3] = {}, t[2] = {};
                if (!cgltf_accessor_read_float(pos, i, p, 3) || (nrm && !cgltf_accessor_read_float(nrm, i, n, 3)) || (uv && !cgltf_accessor_read_float(uv, i, t, 2)))
                    return Fail("a vertex can't be read");
                Vertex &v = out.verts[first + i];
                v = Vertex();
                for (int k = 0; k < kInfluences; ++k) v.node[k] = -1, v.weight[k] = 0;
                if (std::isfinite(t[0]) && std::isfinite(t[1])) v.u = t[0], v.v = t[1];
                v.material = material;
                if (skinned) {
                    cgltf_uint j[kInfluences] = {};
                    cgltf_float w[kInfluences] = {};
                    for (int set = 0; set < 2 && joints[set]; ++set)
                        if (!cgltf_accessor_read_uint(joints[set], i, j + 4 * set, 4) || !cgltf_accessor_read_float(weights[set], i, w + 4 * set, 4))
                            return Fail("a vertex's joints can't be read");
                    double total = 0;
                    for (int k = 0; k < kInfluences; ++k) {
                        if (!(w[k] > 0) || !std::isfinite(w[k])) w[k] = 0;
                        else if (j[k] >= move.size()) return Fail("a vertex names a joint its skin doesn't have");
                        total += w[k];
                    }
                    if (total <= 0) continue;   // no joint moves it: it stays out of every part (weights all 0)
                    Mat sum = {};
                    for (int k = 0; k < kInfluences; ++k) {
                        if (w[k] <= 0) continue;
                        sum = Sum(sum, Scaled(move[j[k]], w[k] / total));
                        v.node[k] = jointNode[j[k]], v.weight[k] = float(w[k] / total);
                    }
                    turn[i] = sum;
                } else {
                    v.node[0] = int(ni), v.weight[0] = 1;
                }
                v.p = turn[i].point({p[0], p[1], p[2]});
                v.n = turn[i].dir({n[0], n[1], n[2]}).unit();
                if (!v.p.finite()) return Fail("a vertex's place isn't a number");
            }
            const size_t corners = prim.indices ? prim.indices->count : count;
            if (corners % 3 || out.tris.size() + corners > kMaxTris * 3) return Fail("too many triangles, or not whole ones");
            for (size_t c = 0; c < corners; ++c) {
                const size_t i = prim.indices ? cgltf_accessor_read_index(prim.indices, c) : c;
                if (i >= count) return Fail("a triangle names a vertex its mesh doesn't have");
                out.tris.push_back(uint32_t(first + i));
            }
            if (!Morphs(size_t(node.mesh - d->meshes), prim, first, turn)) return false;
        }
        return true;
    }
};

}  // namespace

std::string GltfText(const char *raw) {
    std::string text = raw ? raw : "";
    if (text.find('\\') != std::string::npos) text.resize(cgltf_decode_string(&text[0]));
    return text;
}

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
        n.name = modelfile::Plain(GltfText(d->nodes[i].name));
        n.parent = d->nodes[i].parent ? int(d->nodes[i].parent - d->nodes) : -1;
        n.at = {world[i].m[0][3], world[i].m[1][3], world[i].m[2][3]};
        if (!n.at.finite()) return err = "a node's place isn't a number", false;
    }
    out.info.format = "gltf";
    const size_t slash = path.rfind('/');
    if (!ReadLook(d, slash == std::string::npos ? "." : path.substr(0, slash), out, err)) return false;
    std::vector<MorphBind> binds;
    if (!ReadVrm(d, out, binds)) NamedMorphs(d, binds);

    Reader reader = {d, world, binds, out, err};
    for (cgltf_size ni = 0; ni < d->nodes_count; ++ni) {
        const cgltf_node &node = d->nodes[ni];
        if (!node.mesh) continue;
        std::vector<Mat> move;   // each joint's: the mesh's space -> the rest pose
        std::vector<int> jointNode;
        if (node.skin && node.skin->inverse_bind_matrices) {
            const cgltf_skin &skin = *node.skin;
            if (skin.inverse_bind_matrices->count < skin.joints_count) return err = "a skin is short of bind matrices", false;
            for (cgltf_size k = 0; k < skin.joints_count; ++k) {
                cgltf_float ibm[16];
                if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, k, ibm, 16)) return err = "a skin's bind matrix can't be read", false;
                jointNode.push_back(int(skin.joints[k] - d->nodes));
                move.push_back(Mul(world[size_t(jointNode[k])], FromGl(ibm)));
            }
        } else if (node.skin) {
            continue;   // a skin without bind matrices: its mesh's rest pose isn't told
        }
        if (!reader.Mesh(ni, move, jointNode)) return false;
    }
    if (out.tris.empty()) return err = "the file has no mesh", false;
    return true;
}

}  // namespace importer
