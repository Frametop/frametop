// FBX into a Scene (scene.h), with ufbx: every mesh's triangles in the rest pose (each vertex
// of a skinned mesh moved by its clusters' bones as the file's nodes stand; a mesh without a
// skin moves with its node), in metres, y up, with the materials (fbxlook.cpp) and the blend
// shapes named as Frametop's face shapes (shapenames.h). An FBX names no humanoid and no
// licence: the bones are found by their names (handbones.h, bodybones.h).
#include <sys/stat.h>

#include "fbxparts.h"
#include "shapenames.h"

namespace importer {
namespace {

struct Freed {
    ufbx_scene *s = nullptr;
    ~Freed() { ufbx_free_scene(s); }
};

Vec V(const ufbx_vec3 &v) { return {v.x, v.y, v.z}; }

// A mesh's blend shapes that are Frametop's: each vertex's offset, by the mesh's own vertices.
struct MeshShape {
    uint32_t id;
    std::vector<ufbx_vec3> offset;
    std::vector<bool> moved;
};

std::vector<MeshShape> ShapesOf(const ufbx_mesh *mesh) {
    std::vector<MeshShape> out;
    for (size_t d = 0; d < mesh->blend_deformers.count; ++d) {
        const ufbx_blend_deformer *deformer = mesh->blend_deformers.data[d];
        for (size_t c = 0; c < deformer->channels.count && out.size() < 16; ++c) {
            const ufbx_blend_channel *channel = deformer->channels.data[c];
            const ufbx_blend_shape *shape = channel->target_shape;
            const uint32_t id = ShapeOfName(std::string(channel->name.data, channel->name.length));
            if (!id || !shape) continue;
            MeshShape m = {id, std::vector<ufbx_vec3>(mesh->num_vertices, ufbx_vec3{0, 0, 0}), std::vector<bool>(mesh->num_vertices, false)};
            for (size_t k = 0; k < shape->num_offsets && k < shape->offset_vertices.count && k < shape->position_offsets.count; ++k) {
                const uint32_t v = shape->offset_vertices.data[k];
                if (v < mesh->num_vertices) m.offset[v] = shape->position_offsets.data[k], m.moved[v] = true;
            }
            out.push_back(std::move(m));
        }
    }
    return out;
}

SceneShape &ShapeIn(Scene &out, uint32_t id) {
    for (SceneShape &s : out.shapes)
        if (s.id == id) return s;
    out.shapes.emplace_back();
    out.shapes.back().id = id;
    return out.shapes.back();
}

// The triangles of one mesh, a vertex for each corner (the ones that are the same are joined
// when a part is cut out).
bool ReadMesh(const ufbx_scene *scene, const ufbx_mesh *mesh, Scene &out, std::string &err) {
    const ufbx_skin_deformer *skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
    const ufbx_node *node = mesh->instances.data[0];
    const std::vector<MeshShape> shapes = ShapesOf(mesh);
    std::vector<uint32_t> corners(mesh->max_face_triangles * 3);
    for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
        const uint32_t tris = ufbx_triangulate_face(corners.data(), corners.size(), mesh, mesh->faces.data[fi]);
        if (out.tris.size() + size_t(tris) * 3 > kMaxTris * 3 || out.verts.size() + size_t(tris) * 3 > kMaxVerts) return err = "too many triangles", false;
        int material = -1;
        if (fi < mesh->face_material.count && mesh->face_material.data[fi] < mesh->materials.count) {
            const ufbx_material *m = mesh->materials.data[mesh->face_material.data[fi]];
            if (m && m->typed_id < scene->materials.count) material = int(m->typed_id);
        }
        for (uint32_t c = 0; c < tris * 3; ++c) {
            const uint32_t ix = corners[c];
            if (ix >= mesh->vertex_indices.count) return err = "the file is damaged", false;
            const uint32_t vi = mesh->vertex_indices.data[ix];
            if (vi >= mesh->vertices.count || (skin && vi >= skin->vertices.count)) return err = "the file is damaged", false;
            const ufbx_vec3 at = mesh->vertices.data[vi];
            const ufbx_vec3 normal = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, ix) : ufbx_vec3{0, 0, 0};
            Vertex v = {};
            for (int k = 0; k < kInfluences; ++k) v.node[k] = -1, v.weight[k] = 0;
            v.material = material;
            if (mesh->vertex_uv.exists) {
                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                if (std::isfinite(uv.x) && std::isfinite(uv.y)) v.u = float(uv.x), v.v = float(1 - uv.y);   // an FBX's v goes up
            }
            // what moves the vertex, each with its share: the skin's bones, or the mesh's node
            const ufbx_matrix *by[kInfluences] = {};
            double share[kInfluences] = {}, total = 0;
            if (skin) {
                const ufbx_skin_vertex sv = skin->vertices.data[vi];
                const uint32_t count = sv.num_weights < uint32_t(kInfluences) ? sv.num_weights : uint32_t(kInfluences);   // heaviest first
                for (uint32_t k = 0; k < count; ++k) {
                    if (sv.weight_begin + k >= skin->weights.count) return err = "the file is damaged", false;
                    const ufbx_skin_weight w = skin->weights.data[sv.weight_begin + k];
                    if (w.cluster_index >= skin->clusters.count) return err = "the file is damaged", false;
                    const ufbx_skin_cluster *cluster = skin->clusters.data[w.cluster_index];
                    if (!cluster->bone_node || !(w.weight > 0)) continue;
                    by[k] = &cluster->geometry_to_world, share[k] = w.weight, total += w.weight;
                    v.node[k] = int(cluster->bone_node->typed_id);
                }
            } else {
                by[0] = &node->geometry_to_world, share[0] = total = 1;
                v.node[0] = int(node->typed_id);
            }
            if (total > 0) {
                for (int k = 0; k < kInfluences; ++k) {
                    if (!by[k]) continue;
                    share[k] /= total;
                    v.weight[k] = float(share[k]);
                    v.p = v.p + V(ufbx_transform_position(by[k], at)) * share[k];
                    v.n = v.n + V(ufbx_transform_direction(by[k], normal)) * share[k];
                }
                v.n = v.n.unit();
                for (const MeshShape &shape : shapes) {
                    if (!shape.moved[vi]) continue;
                    Vec move;
                    for (int k = 0; k < kInfluences; ++k)
                        if (by[k]) move = move + V(ufbx_transform_direction(by[k], shape.offset[vi])) * share[k];
                    if (move.finite() && move.norm() >= 1e-7) ShapeIn(out, shape.id).move[uint32_t(out.verts.size())] = move;
                }
            } else {
                for (int k = 0; k < kInfluences; ++k) v.node[k] = -1;
            }
            if (!v.p.finite()) return err = "a vertex's place isn't a number", false;
            out.tris.push_back(uint32_t(out.verts.size()));
            out.verts.push_back(v);
        }
    }
    return true;
}

}  // namespace

bool LoadFbx(const std::string &path, Scene &out, std::string &err) {
    out = Scene();
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return err = "the file can't be read", false;
    if (size_t(st.st_size) > kMaxSourceBytes) return err = "the file is too large", false;
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1;
    opts.generate_missing_normals = true;
    opts.ignore_animation = true;
    opts.clean_skin_weights = true;
    opts.temp_allocator.memory_limit = size_t(1) << 30;
    opts.result_allocator.memory_limit = size_t(1) << 30;
    ufbx_error error;
    Freed f;
    f.s = ufbx_load_file(path.c_str(), &opts, &error);
    if (!f.s) return err = "not an FBX file this reads", false;
    const ufbx_scene *scene = f.s;
    if (scene->nodes.count > kMaxNodes) return err = "too many nodes", false;

    out.info.format = "fbx";
    out.nodes.resize(scene->nodes.count);
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node *n = scene->nodes.data[i];
        Node &o = out.nodes[i];
        o.name = modelfile::Plain(std::string(n->name.data, n->name.length));
        o.parent = n->parent ? int(n->parent->typed_id) : -1;
        o.at = {n->node_to_world.m03, n->node_to_world.m13, n->node_to_world.m23};
        if (!o.at.finite()) return err = "a node's place isn't a number", false;
    }
    const size_t slash = path.rfind('/');
    if (!ReadFbxLook(scene, slash == std::string::npos ? "." : path.substr(0, slash), out, err)) return false;
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh *mesh = scene->meshes.data[mi];
        if (mesh->instances.count && !ReadMesh(scene, mesh, out, err)) return false;
    }
    if (out.tris.empty()) return err = "the file has no mesh", false;
    return true;
}

}  // namespace importer
