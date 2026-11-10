// FBX into a Scene (scene.h), with ufbx: every skinned mesh's triangles in the rest pose (each
// vertex moved by its clusters' bones as the file's nodes stand), in metres, y up. An FBX names
// no humanoid and no licence: the bones are found by their names (handbones.h).
#include "scene.h"

#include <sys/stat.h>

#include "ufbx.h"

namespace importer {
namespace {

struct Freed {
    ufbx_scene *s = nullptr;
    ~Freed() { ufbx_free_scene(s); }
};

Vec V(const ufbx_vec3 &v) { return {v.x, v.y, v.z}; }

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
    opts.ignore_embedded = true;   // textures: not read here, and never files the FBX points at
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
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh *mesh = scene->meshes.data[mi];
        if (!mesh->skin_deformers.count || !mesh->instances.count) continue;
        const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
        std::vector<uint32_t> corners(mesh->max_face_triangles * 3);
        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            const uint32_t tris = ufbx_triangulate_face(corners.data(), corners.size(), mesh, mesh->faces.data[fi]);
            if (out.tris.size() + size_t(tris) * 3 > kMaxTris * 3 || out.verts.size() + size_t(tris) * 3 > kMaxVerts)
                return err = "too many triangles", false;
            // a vertex for each corner: the ones that are the same are joined when a part is cut out
            for (uint32_t c = 0; c < tris * 3; ++c) {
                const uint32_t ix = corners[c];
                if (ix >= mesh->vertex_indices.count) return err = "the file is damaged", false;
                const uint32_t vi = mesh->vertex_indices.data[ix];
                if (vi >= mesh->vertices.count || vi >= skin->vertices.count) return err = "the file is damaged", false;
                const ufbx_vec3 at = mesh->vertices.data[vi];
                const ufbx_vec3 normal = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, ix) : ufbx_vec3{0, 0, 0};
                const ufbx_skin_vertex sv = skin->vertices.data[vi];
                Vertex v = {};
                for (int k = 0; k < kInfluences; ++k) v.node[k] = -1, v.weight[k] = 0;
                double total = 0;
                const uint32_t count = sv.num_weights < uint32_t(kInfluences) ? sv.num_weights : uint32_t(kInfluences);   // heaviest first
                for (uint32_t k = 0; k < count; ++k) {
                    const ufbx_skin_weight w = skin->weights.data[sv.weight_begin + k];
                    const ufbx_skin_cluster *cluster = skin->clusters.data[w.cluster_index];
                    if (!cluster->bone_node || !(w.weight > 0)) continue;
                    v.node[k] = int(cluster->bone_node->typed_id), v.weight[k] = float(w.weight);
                    total += w.weight;
                    v.p = v.p + V(ufbx_transform_position(&cluster->geometry_to_world, at)) * w.weight;
                    v.n = v.n + V(ufbx_transform_direction(&cluster->geometry_to_world, normal)) * w.weight;
                }
                if (total > 0) {
                    v.p = v.p * (1 / total), v.n = v.n.unit();
                    for (int k = 0; k < kInfluences; ++k) v.weight[k] = float(v.weight[k] / total);
                } else {
                    for (int k = 0; k < kInfluences; ++k) v.node[k] = -1, v.weight[k] = 0;
                }
                if (!v.p.finite()) return err = "a vertex's place isn't a number", false;
                out.tris.push_back(uint32_t(out.verts.size()));
                out.verts.push_back(v);
            }
        }
    }
    if (out.tris.empty()) return err = "the file has no skinned mesh", false;
    return true;
}

}  // namespace importer
