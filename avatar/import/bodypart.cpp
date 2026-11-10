// See bodypart.h.
#include "bodypart.h"

#include <cmath>
#include <map>
#include <unordered_map>

#include "bodylook.h"
#include "reduce.h"
#include "weights.h"

namespace importer {
namespace {

using namespace bodyfile;

constexpr int kMinTris = 100;

// From the model's space to the body's: turned about y so its right is +x, its hips over the
// origin, the floor at y = 0.
struct Frame {
    Vec hips;
    double c = 1, s = 0, floor = 0;
    Vec Turn(const Vec &v) const { return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z}; }
    Vec Place(const Vec &p) const {
        Vec t = Turn(p - Vec{hips.x, 0, hips.z});
        t.y -= floor;
        return t;
    }
};

// A vertex before it is written: in the body's space but for the floor.
struct Raw {
    Vec p, n;
    float u = 0, v = 0;
    uint32_t material = 0;
    uint8_t bones[4], weights[4];
    std::string moves;   // the shapes that move it: an id, then three i16, each
};

// Each node's role: its own, or the one it hangs from; the hips' for a node above them. A node
// that turns with a bone it doesn't hang from (Node::follows) has that bone's role when it sits
// where the bone does: a body's bone turns about its own place, so a node that copies a turn
// from somewhere else (a robot arm on a backpack that moves as the wearer's does) stays with
// what it hangs from.
std::vector<int> RoleOfNodes(const Scene &s, const BodyBones &b) {
    constexpr double kSamePlace = 0.01;   // metres
    std::vector<int> own(s.nodes.size(), -1), of(s.nodes.size(), kHips);
    for (int r = 0; r < kRoles; ++r)
        if (b.node[r] >= 0) own[size_t(b.node[r])] = r;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        int steps = 0;
        for (int n = int(i); n >= 0 && steps <= int(s.nodes.size()); n = s.nodes[size_t(n)].parent, ++steps) {
            const int f = s.nodes[size_t(n)].follows;
            const bool led = own[size_t(n)] < 0 && f >= 0 && f < int(s.nodes.size()) && own[size_t(f)] >= 0 &&
                             (s.nodes[size_t(n)].at - s.nodes[size_t(f)].at).norm() < kSamePlace;
            if (own[size_t(n)] >= 0 || led) {
                of[i] = led ? own[size_t(f)] : own[size_t(n)];
                break;
            }
        }
    }
    return of;
}

template <typename T> void Append(std::string &key, const T &value) { key.append(reinterpret_cast<const char *>(&value), sizeof value); }

int16_t Quantized(double v) { return int16_t(std::clamp(std::lround(v), -32767L, 32767L)); }

// The mesh as it is cut: welded vertices, each array one entry a vertex.
struct Mesh {
    std::vector<int16_t> pos;   // x 3
    std::vector<int8_t> nrm;    // x 3
    std::vector<Raw> raw;       // what else each carries
    std::vector<uint32_t> tris;
    size_t verts() const { return raw.size(); }
};

// The vertices that are the same in everything become one; triangles left without an area go.
Mesh Weld(const std::vector<Raw> &raws, const std::vector<uint32_t> &tris, double unit) {
    Mesh m;
    std::unordered_map<std::string, uint32_t> seen;
    std::vector<uint32_t> to(raws.size());
    for (size_t i = 0; i < raws.size(); ++i) {
        const Raw &r = raws[i];
        const int16_t p[3] = {Quantized(r.p.x / unit), Quantized(r.p.y / unit), Quantized(r.p.z / unit)};
        const int8_t n[3] = {int8_t(Quantized(127 * r.n.x)), int8_t(Quantized(127 * r.n.y)), int8_t(Quantized(127 * r.n.z))};
        std::string key;
        Append(key, p), Append(key, n), Append(key, r.u), Append(key, r.v), Append(key, r.material), Append(key, r.bones), Append(key, r.weights);
        key += r.moves;
        const auto found = seen.emplace(key, uint32_t(m.raw.size()));
        if (found.second) {
            m.pos.insert(m.pos.end(), p, p + 3), m.nrm.insert(m.nrm.end(), n, n + 3);
            m.raw.push_back(r);
        }
        to[i] = found.first->second;
    }
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const uint32_t a = to[tris[t]], b = to[tris[t + 1]], c = to[tris[t + 2]];
        if (a != b && b != c && a != c) m.tris.insert(m.tris.end(), {a, b, c});
    }
    return m;
}

size_t UsedVerts(const Mesh &m) {
    std::vector<bool> used(m.verts(), false);
    size_t n = 0;
    for (const uint32_t i : m.tris)
        if (!used[i]) used[i] = true, ++n;
    return n;
}

// Fewer triangles, if a body doesn't need so many, and no more vertices than a file holds.
bool ReduceMesh(Mesh &m, double unit, std::string &why) {
    std::vector<float> positions(m.verts() * 3), attributes(m.verts() * 5);
    std::vector<uint8_t> keep(m.verts());
    for (size_t i = 0; i < m.verts(); ++i) {
        for (int k = 0; k < 3; ++k) positions[i * 3 + k] = float(m.pos[i * 3 + k] * unit), attributes[i * 5 + 2 + k] = float(m.nrm[i * 3 + k]) / 127;
        attributes[i * 5] = m.raw[i].u, attributes[i * 5 + 1] = m.raw[i].v;
        keep[i] = !m.raw[i].moves.empty();
    }
    ReduceKeepingLook(positions, attributes, 5, {1.0f, 1.0f, 0.5f, 0.5f, 0.5f}, keep, m.tris, size_t(kBodyTargetTris));
    // it stopped short at the seams: past what a file holds, they aren't kept
    size_t limit = size_t(kMaxTris);
    for (int round = 0; round < 8 && (m.tris.size() / 3 > size_t(kMaxTris) || UsedVerts(m) > size_t(kMaxVerts)); ++round) {
        Reduce(positions, m.tris, limit);
        limit = std::min(limit, m.tris.size() / 3) * 3 / 4;
    }
    if (m.tris.size() / 3 > size_t(kMaxTris) || UsedVerts(m) > size_t(kMaxVerts)) return why = "its mesh couldn't be reduced to what a body may have", false;
    return true;
}

// The triangles in the order they are drawn in, as parts: by how see-through their material is, then by material.
void MakeParts(Mesh &m, Body &out) {
    const auto rank = [&](size_t t) {
        const uint32_t material = m.raw[m.tris[t * 3]].material;
        return out.materials[material].alpha * 1000 + material;
    };
    std::vector<size_t> order(m.tris.size() / 3);
    for (size_t t = 0; t < order.size(); ++t) order[t] = t;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rank(a) < rank(b); });
    std::vector<uint32_t> sorted;
    for (const size_t t : order) {
        const uint32_t material = m.raw[m.tris[t * 3]].material;
        if (out.parts.empty() || out.parts.back().material != material) out.parts.push_back({uint32_t(sorted.size() / 3), 0, material});
        ++out.parts.back().count;
        sorted.insert(sorted.end(), m.tris.begin() + long(t * 3), m.tris.begin() + long(t * 3 + 3));
    }
    m.tris = sorted;
}

// The mesh into the body's arrays: only the vertices still used, in the order the triangles
// meet them; a vertex the file gave no normal gets its faces'.
void WriteMesh(const Mesh &m, Body &out) {
    std::vector<Vec> faces(m.verts());
    for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
        Vec p[3];
        for (int k = 0; k < 3; ++k) p[k] = {double(m.pos[m.tris[t + size_t(k)] * 3]), double(m.pos[m.tris[t + size_t(k)] * 3 + 1]), double(m.pos[m.tris[t + size_t(k)] * 3 + 2])};
        const Vec e1 = p[1] - p[0], e2 = p[2] - p[0];
        const Vec face = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        for (int k = 0; k < 3; ++k) faces[m.tris[t + size_t(k)]] = faces[m.tris[t + size_t(k)]] + face;
    }
    std::vector<int32_t> final(m.verts(), -1);
    std::map<uint32_t, std::map<uint16_t, const char *>> moves;   // each shape's vertices' moves, rising
    for (const uint32_t i : m.tris) {
        if (final[i] < 0) {
            final[i] = int32_t(out.pos.size() / 3);
            const Raw &r = m.raw[i];
            int8_t n[3] = {m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]};
            if (!n[0] && !n[1] && !n[2]) {
                const Vec f = faces[i].unit();
                n[0] = int8_t(std::lround(127 * f.x)), n[1] = int8_t(std::lround(127 * f.y)), n[2] = int8_t(std::lround(127 * f.z));
            }
            out.pos.insert(out.pos.end(), &m.pos[i * 3], &m.pos[i * 3] + 3), out.nrm.insert(out.nrm.end(), n, n + 3);
            out.uv.push_back(r.u), out.uv.push_back(r.v);
            out.bones.insert(out.bones.end(), r.bones, r.bones + 4), out.weights.insert(out.weights.end(), r.weights, r.weights + 4);
            for (size_t at = 0; at + 7 <= r.moves.size(); at += 7) moves[uint8_t(r.moves[at])][uint16_t(final[i])] = r.moves.data() + at + 1;
        }
        out.idx.push_back(uint16_t(final[i]));
    }
    for (const auto &shape : moves) {
        Shape sh;
        sh.id = shape.first;
        for (const auto &move : shape.second) {
            int16_t d[3];
            std::memcpy(d, move.second, sizeof d);
            sh.verts.push_back(move.first), sh.delta.insert(sh.delta.end(), d, d + 3);
        }
        out.shapes.push_back(std::move(sh));
    }
}

}  // namespace

bool CutBody(const Scene &s, const BodyBones &b, Body &out, BodyReport &report, std::string &why) {
    out = Body();
    report = BodyReport();
    for (int r = 0; r < kRoles; ++r) {
        if (b.node[r] >= int(s.nodes.size())) return why = "a bone isn't one of the model's", false;
        if (b.node[r] < 0 && RoleNeeded(r)) return why = std::string("the body has no ") + kRoleNames[r], false;
    }
    const auto at = [&](int role) { return s.nodes[size_t(b.node[role])].at; };
    Frame frame;
    frame.hips = at(kHips);
    Vec right = at(kRightUpperArm) - at(kLeftUpperArm);
    right.y = 0;
    if (right.norm() < 1e-4) return why = "its arms are in one place", false;
    right = right.unit();
    frame.c = right.x, frame.s = right.z;
    const double tall = at(kHead).y - at(kHips).y;
    if (!(tall > 0.02) || tall < 2 * std::fabs(frame.Turn(at(kHead) - at(kHips)).z)) return why = "it isn't standing upright", false;

    std::vector<uint32_t> materialOf;
    if (!CutLook(s, out, materialOf, why)) return false;
    const std::vector<int> roleOf = RoleOfNodes(s, b);

    // the vertices something moves, and the triangles between them
    std::vector<int32_t> slot(s.verts.size(), -1);   // -1 not looked at, -2 not the body's, else its place in `raws`
    std::vector<Raw> raws;
    std::vector<uint32_t> tris;
    const auto place = [&](uint32_t vi) -> int32_t {
        if (slot[vi] != -1) return slot[vi];
        const Vertex &v = s.verts[vi];
        float acc[kRoles] = {};
        double total = 0;
        for (int k = 0; k < kInfluences; ++k)
            if (v.node[k] >= 0 && v.node[k] < int(s.nodes.size()) && v.weight[k] > 0) acc[roleOf[size_t(v.node[k])]] += v.weight[k], total += v.weight[k];
        if (total <= 0) return slot[vi] = -2;
        Raw r;
        r.p = frame.Place(v.p), r.n = frame.Turn(v.n);
        r.u = v.u, r.v = v.v;
        r.material = materialOf[v.material >= 0 && v.material < int(s.materials.size()) ? size_t(v.material) : s.materials.size()];
        PackWeights(acc, kRoles, r.bones, r.weights);
        raws.push_back(r);
        return slot[vi] = int32_t(raws.size() - 1);
    };
    for (size_t t = 0; t + 2 < s.tris.size(); t += 3) {
        if (s.tris[t] >= s.verts.size() || s.tris[t + 1] >= s.verts.size() || s.tris[t + 2] >= s.verts.size())
            return why = "a triangle names a vertex the model doesn't have", false;
        const int32_t a = place(s.tris[t]), c = place(s.tris[t + 1]), d = place(s.tris[t + 2]);
        if (a >= 0 && c >= 0 && d >= 0) tris.insert(tris.end(), {uint32_t(a), uint32_t(c), uint32_t(d)});
    }
    report.sourceTris = int(tris.size() / 3);
    if (tris.size() / 3 < size_t(kMinTris)) return why = "no mesh moves with the skeleton", false;

    // the floor: under the feet, where they have a mesh
    double floor = 1e30, lowest = 1e30, extent = 0;
    for (const Raw &r : raws) {
        int feet = 0;
        for (int k = 0; k < 4; ++k)
            if (r.bones[k] == kLeftFoot || r.bones[k] == kRightFoot || r.bones[k] == kLeftToes || r.bones[k] == kRightToes) feet += r.weights[k];
        if (feet >= 128) floor = std::min(floor, r.p.y);
        lowest = std::min(lowest, r.p.y);
    }
    frame.floor = floor < 1e29 ? floor : lowest;
    for (Raw &r : raws) {
        r.p.y -= frame.floor;
        extent = std::max({extent, std::fabs(r.p.x), std::fabs(r.p.y), std::fabs(r.p.z)});
    }
    const double unit = std::max(extent / 32000, 1e-5);
    if (!(unit <= 1e-2)) return why = "it is too large", false;

    for (const SceneShape &shape : s.shapes) {
        if (shape.id < 1 || shape.id > Shape::kLastId) continue;
        for (const auto &move : shape.move) {
            if (move.first >= slot.size() || slot[move.first] < 0) continue;
            const Vec m = frame.Turn(move.second) * (1 / unit);
            const int16_t d[3] = {Quantized(m.x), Quantized(m.y), Quantized(m.z)};
            if (!d[0] && !d[1] && !d[2]) continue;
            std::string &moves = raws[size_t(slot[move.first])].moves;
            moves += char(shape.id);
            Append(moves, d);
        }
    }

    Mesh mesh = Weld(raws, tris, unit);
    if (!ReduceMesh(mesh, unit, why)) return false;
    MakeParts(mesh, out);
    WriteMesh(mesh, out);

    out.unit = float(unit);
    for (int r = 0; r < kRoles; ++r) {
        if (!(out.present[r] = b.node[r] >= 0)) continue;
        const Vec p = frame.Place(at(r));
        out.at[r][0] = float(p.x), out.at[r][1] = float(p.y), out.at[r][2] = float(p.z);
    }
    // the eyes: between the eye bones, or where a head of this size has them
    Vec eyes = {out.at[kHead][0], out.at[kHead][1] + 0.11 * tall, out.at[kHead][2] - 0.13 * tall};
    if (out.present[kLeftEye] && out.present[kRightEye])
        eyes = {(out.at[kLeftEye][0] + out.at[kRightEye][0]) / 2.0, (out.at[kLeftEye][1] + out.at[kRightEye][1]) / 2.0, (out.at[kLeftEye][2] + out.at[kRightEye][2]) / 2.0};
    out.eyes[0] = float(eyes.x), out.eyes[1] = float(eyes.y), out.eyes[2] = float(eyes.z);
    if (!(out.eyes[1] > 0.05f)) return why = "its head is on the floor", false;

    report.tris = out.tris(), report.verts = out.verts();
    for (const Texture &t : out.textures) report.textureBytes += t.rgba.size();
    return Valid(out, why);
}

}  // namespace importer
