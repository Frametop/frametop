// See handpart.h.
#include "handpart.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "reduce.h"
#include "weights.h"

namespace importer {
namespace {

constexpr double kCuff = 0.04;    // the forearm kept behind the wrist, metres
constexpr double kCuffOut = 0.10; // ...and no further from the wrist than this
constexpr double kReach = 0.30;   // nothing further from the wrist is a hand's (positions hold 0.327)
// The palm's size (the wrist to the four knuckles, added up): the bundled hands' range, and what
// a hand outside it is scaled to.
constexpr double kPalmMin = 0.30, kPalmMax = 0.45, kPalm = 0.36;
constexpr int kMinTris = 40;

constexpr int kBase[5] = {1, 5, 10, 15, 20};   // each chain's first joint: thumb, index .. little
constexpr int kEnd[5] = {4, 9, 14, 19, 24};    // ...and its tip
const char *const kFinger[5] = {"thumb", "index finger", "middle finger", "ring finger", "little finger"};

// A vertex as the file holds it, compared whole.
struct Packed {
    int16_t pos[3];
    int8_t nrm[3];
    uint8_t joints[4], weights[4];
    uint8_t zero;   // the struct's last byte, so that all 18 are set
    bool operator<(const Packed &o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
static_assert(sizeof(Packed) == 18, "no padding");

// Each node's joint: its own, or the one it hangs from (a palm bone, a ring's bone); -1 for a
// node that isn't part of the hand.
std::vector<int> JointOfNodes(const Scene &s, const HandBones &b) {
    std::vector<int> own(s.nodes.size(), -1), of(s.nodes.size(), -1);
    for (int j = 0; j < kHandJoints; ++j)
        if (b.joint[j] >= 0) own[size_t(b.joint[j])] = j;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        int steps = 0;
        for (int n = int(i); n >= 0 && steps <= int(s.nodes.size()); n = s.nodes[size_t(n)].parent, ++steps)
            if (own[size_t(n)] >= 0) {
                of[i] = own[size_t(n)];
                break;
            }
    }
    return of;
}

// The nodes of the arm the hand is on: the wrist's parent and what hangs from it, the hand aside.
std::vector<bool> ArmNodes(const Scene &s, const HandBones &b, const std::vector<int> &jointOf) {
    std::vector<bool> arm(s.nodes.size(), false);
    const int fore = s.nodes[size_t(b.joint[0])].parent;
    if (fore < 0) return arm;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (jointOf[i] >= 0) continue;
        int steps = 0;
        for (int n = int(i); n >= 0 && steps <= int(s.nodes.size()); n = s.nodes[size_t(n)].parent, ++steps)
            if (n == fore) {
                arm[i] = true;
                break;
            }
    }
    return arm;
}

}  // namespace

bool CutHand(const Scene &s, const HandBones &b, modelfile::Hand &out, HandReport &report, std::string &why) {
    out = modelfile::Hand();
    report = HandReport();
    for (const int n : b.joint)
        if (n >= int(s.nodes.size())) return why = "a bone isn't one of the model's", false;
    if (b.joint[0] < 0) return why = "the hand has no wrist", false;
    const std::vector<int> jointOf = JointOfNodes(s, b);
    const std::vector<bool> arm = ArmNodes(s, b, jointOf);

    // the joints: the rig's, then the ones it lacks
    Vec J[kHandJoints];
    bool has[kHandJoints];
    for (int j = 0; j < kHandJoints; ++j)
        if ((has[j] = b.joint[j] >= 0)) J[j] = s.nodes[size_t(b.joint[j])].at;
    for (int c = 0; c < 5; ++c)
        for (int j = kBase[c] + (c ? 1 : 0); j < kEnd[c]; ++j)
            if (!has[j]) return why = std::string("the ") + kFinger[c] + " is missing a bone", false;
    for (int c = 1; c < 5; ++c)
        if (!has[kBase[c]]) J[kBase[c]] = J[0] + (J[kBase[c] + 1] - J[0]) * (1.0 / 3);
    for (int c = 0; c < 5; ++c) {
        if (has[kEnd[c]]) continue;
        // where the mesh the last bone moves ends, along the bone before it
        const Vec last = J[kEnd[c] - 1], bone = last - J[kEnd[c] - 2], dir = bone.unit();
        double reach = 0;
        for (const Vertex &v : s.verts) {
            double w = 0;
            for (int k = 0; k < kInfluences; ++k)
                if (v.node[k] >= 0 && jointOf[size_t(v.node[k])] == kEnd[c] - 1) w += v.weight[k];
            if (w >= 0.5) reach = std::max(reach, (v.p - last).dot(dir));
        }
        if (reach < 0.2 * bone.norm() || reach > 2 * bone.norm()) reach = 0.8 * bone.norm();   // no mesh there, or a claw
        J[kEnd[c]] = last + dir * reach;
    }
    double palm = 0;
    for (const int j : {6, 11, 16, 21}) palm += (J[j] - J[0]).norm();
    if (!(palm > 1e-6) || !std::isfinite(palm)) return why = "the hand's bones are all in one place", false;
    const double scale = palm >= kPalmMin && palm <= kPalmMax ? 1 : kPalm / palm;
    report.scale = scale;
    const Vec wrist = J[0], along = (J[11] - J[0]).unit();
    for (int c = 0; c < 5; ++c)
        for (int j = kBase[c] + (c ? 1 : 0); j < kEnd[c]; ++j)
            if ((J[j + 1] - J[j]).norm() * scale < 0.002) return why = std::string("the ") + kFinger[c] + "'s bones are in one place", false;

    // the vertices: the hand's, and the cuff's
    std::vector<int32_t> slot(s.verts.size(), -1);   // -1 not looked at, -2 not the hand's, else its place in `packed`
    std::vector<Packed> packed;
    std::vector<Vec> normals;                        // of `packed`'s: summed from the faces where the file had none
    std::map<Packed, uint32_t> welded;
    std::vector<uint32_t> tris;
    const auto place = [&](uint32_t vi) -> int32_t {
        if (slot[vi] != -1) return slot[vi];
        const Vertex &v = s.verts[vi];
        float acc[kHandJoints] = {};
        double hand = 0, cuff = 0;
        for (int k = 0; k < kInfluences; ++k) {
            if (v.node[k] < 0 || !(v.weight[k] > 0)) continue;
            const int j = jointOf[size_t(v.node[k])];
            if (j >= 0) hand += v.weight[k], acc[j] += v.weight[k];
            else {
                acc[0] += v.weight[k];   // the forearm's share: the wrist's
                if (arm[size_t(v.node[k])]) cuff += v.weight[k];
            }
        }
        const Vec p = (v.p - wrist) * scale;
        const double out = p.norm();
        const bool keep = out <= kReach && (hand >= 0.05 || (cuff >= 0.5 && p.dot(along) >= -kCuff && out <= kCuffOut));
        if (!keep) return slot[vi] = -2;
        Packed q = {};
        const double c[3] = {p.x, p.y, p.z}, n[3] = {v.n.x, v.n.y, v.n.z};
        for (int k = 0; k < 3; ++k) {
            q.pos[k] = int16_t(std::lround(c[k] / modelfile::kPosStep));
            q.nrm[k] = int8_t(std::clamp(std::lround(127 * n[k]), -127L, 127L));
        }
        PackWeights(acc, kHandJoints, q.joints, q.weights);
        const auto found = welded.emplace(q, uint32_t(packed.size()));
        if (found.second) packed.push_back(q), normals.push_back({});
        return slot[vi] = int32_t(found.first->second);
    };
    for (size_t t = 0; t + 2 < s.tris.size(); t += 3) {
        if (s.tris[t] >= s.verts.size() || s.tris[t + 1] >= s.verts.size() || s.tris[t + 2] >= s.verts.size())
            return why = "a triangle names a vertex the model doesn't have", false;
        const int32_t a = place(s.tris[t]), c = place(s.tris[t + 1]), d = place(s.tris[t + 2]);
        if (a < 0 || c < 0 || d < 0 || a == c || c == d || a == d) continue;
        tris.insert(tris.end(), {uint32_t(a), uint32_t(c), uint32_t(d)});
    }
    report.sourceTris = int(tris.size() / 3);
    if (tris.size() / 3 < size_t(kMinTris)) return why = "no mesh moves with the hand's bones", false;
    for (int c = 0; c < 5; ++c) {
        bool moved = false;
        for (const Packed &q : packed)
            for (int k = 0; k < 4 && !moved; ++k) moved = q.weights[k] >= 128 && q.joints[k] > kBase[c] - (c ? 0 : 1) && q.joints[k] < kEnd[c];
        if (!moved) return why = std::string("no mesh moves with the ") + kFinger[c], false;
    }

    // fewer triangles, if a hand doesn't need so many
    std::vector<float> positions(packed.size() * 3);
    for (size_t i = 0; i < packed.size(); ++i)
        for (int k = 0; k < 3; ++k) positions[i * 3 + k] = float(packed[i].pos[k]) * modelfile::kPosStep;
    Reduce(positions, tris, size_t(kHandTargetTris));

    // a normal for each vertex the file gave none: its faces', by their size
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const float *p0 = &positions[tris[t] * 3], *p1 = &positions[tris[t + 1] * 3], *p2 = &positions[tris[t + 2] * 3];
        const Vec e1 = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2 = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const Vec face = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        for (int k = 0; k < 3; ++k) normals[tris[t + size_t(k)]] = normals[tris[t + size_t(k)]] + face;
    }
    // only the vertices still used, in the order the triangles meet them
    std::vector<int32_t> final(packed.size(), -1);
    for (const uint32_t i : tris) {
        if (final[i] < 0) {
            final[i] = int32_t(out.pos.size() / 3);
            Packed q = packed[i];
            if (!q.nrm[0] && !q.nrm[1] && !q.nrm[2]) {
                const Vec n = normals[i].unit();
                q.nrm[0] = int8_t(std::lround(127 * n.x)), q.nrm[1] = int8_t(std::lround(127 * n.y)), q.nrm[2] = int8_t(std::lround(127 * n.z));
            }
            out.pos.insert(out.pos.end(), q.pos, q.pos + 3), out.nrm.insert(out.nrm.end(), q.nrm, q.nrm + 3);
            out.joints.insert(out.joints.end(), q.joints, q.joints + 4), out.weights.insert(out.weights.end(), q.weights, q.weights + 4);
        }
        if (final[i] > 0xffff) return why = "the hand has too many vertices", false;
        out.idx.push_back(uint16_t(final[i]));
    }
    report.tris = out.tris();
    for (int j = 0; j < kHandJoints; ++j) {
        const Vec p = (J[j] - wrist) * scale;
        out.bind[j][0] = float(p.x), out.bind[j][1] = float(p.y), out.bind[j][2] = float(p.z);
    }
    return modelfile::Valid(out, why);
}

modelfile::Hand Mirrored(const modelfile::Hand &h) {
    modelfile::Hand m = h;
    for (size_t i = 0; i < m.pos.size(); i += 3) m.pos[i] = int16_t(-m.pos[i]), m.nrm[i] = int8_t(-std::max<int>(m.nrm[i], -127));
    for (auto &joint : m.bind) joint[0] = -joint[0];
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) std::swap(m.idx[t + 1], m.idx[t + 2]);
    return m;
}

}  // namespace importer
