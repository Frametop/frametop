// The abstract body (avatar/abstract): a body by the format's rules, closed lumps that face
// outward, hands a tracked hand fits, and a head and hands that go where their wearer's are.
// No GPU, OpenVR or headset.
#include "../abstract/abstract.h"
#include "../pose/bodypose.h"
#include "../view/demo.h"
#include "../view/handfit.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

using namespace bodyfile;
using avatar::Vec;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static Vec vertex(const Body &b, int v) { return Vec{double(b.pos[3 * v]), double(b.pos[3 * v + 1]), double(b.pos[3 * v + 2])} * double(b.unit); }
static Vec normal(const Body &b, int v) { return Vec{double(b.nrm[3 * v]), double(b.nrm[3 * v + 1]), double(b.nrm[3 * v + 2])} * (1 / 127.0); }
static Vec at(const Body &b, int role) { return {b.at[role][0], b.at[role][1], b.at[role][2]}; }
static Vec moved(const float m[12], const Vec &p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7], m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

static void format() {
    const Body b = abstract::Body(0);
    std::string why;
    check("it is a body by the format's rules", Valid(b, why));
    if (!why.empty()) std::printf("      %s\n", why.c_str());
    bool all = true;
    for (int r = 0; r < kRoles; ++r) all = all && b.present[r];
    check("it has every bone a person has", all);
    check("it is light: under 3000 vertices and 5000 triangles", b.verts() < 3000 && b.tris() < 5000);

    std::string err;
    const std::vector<uint8_t> file = modelfile::Write(modelfile::Model(), bodyfile::Write(b));
    std::vector<modelfile::Chunk> chunks;
    modelfile::Model model;
    Body back;
    bool has = false;
    const bool read = modelfile::Read(file.data(), file.size(), model, err) && modelfile::Chunks(file.data(), file.size(), chunks, err) && Read(chunks, back, has, err);
    check("it goes into a model file and comes back the same", read && has && back.pos == b.pos && back.idx == b.idx && back.bones == b.bones &&
                                                                    !std::memcmp(back.at, b.at, sizeof b.at) && back.materials.size() == 2);
    if (!err.empty()) std::printf("      %s\n", err.c_str());
}

static void colours() {
    bool differ = true;
    for (int a = 0; a < abstract::kColours; ++a)
        for (int c = a + 1; c < abstract::kColours; ++c) {
            double far = 0;
            for (int k = 0; k < 3; ++k) far += std::fabs(abstract::Colour(a)[k] - abstract::Colour(c)[k]);
            differ = differ && far > 0.4;
        }
    check("each place has a colour of its own", differ);
    const Body two = abstract::Body(2), six = abstract::Body(6), minus = abstract::Body(-2);
    check("the colours go round, for any number", !std::memcmp(two.materials[0].color, six.materials[0].color, sizeof two.materials[0].color) &&
                                                     !std::memcmp(two.materials[0].color, minus.materials[0].color, sizeof two.materials[0].color));
    check("only the colour differs from place to place", two.pos == abstract::Body(1).pos && two.idx == abstract::Body(1).idx &&
                                                             two.materials[0].color[0] == abstract::Colour(2)[0] && two.materials[1].color[0] == abstract::Body(1).materials[1].color[0]);
}

static void lumps() {
    const Body b = abstract::Body(1);
    // every triangle has an area, faces the way its vertices' normals do, and hangs from one bone
    bool area = true, outward = true, oneBone = true;
    std::map<std::pair<int, int>, int> edges;   // each edge of a closed lump is walked once each way
    const auto place = [&b](int v) { return std::make_pair(std::make_pair(b.pos[3 * v], b.pos[3 * v + 1]), std::make_pair(b.pos[3 * v + 2], b.bones[4 * v])); };
    std::map<decltype(place(0)), int> points;
    for (int v = 0; v < b.verts(); ++v) points.emplace(place(v), int(points.size()));
    for (int t = 0; t < b.tris(); ++t) {
        const int v[3] = {b.idx[3 * t], b.idx[3 * t + 1], b.idx[3 * t + 2]};
        const Vec face = (vertex(b, v[1]) - vertex(b, v[0])).cross(vertex(b, v[2]) - vertex(b, v[0]));
        area = area && face.norm() > 1e-9;
        for (int k = 0; k < 3; ++k) {
            outward = outward && face.unit().dot(normal(b, v[k])) > 0.2;
            oneBone = oneBone && b.bones[4 * v[k]] == b.bones[4 * v[0]] && b.weights[4 * v[k]] == 255;
            ++edges[{points[place(v[k])], points[place(v[(k + 1) % 3])]}];
        }
    }
    check("no triangle is a sliver", area);
    check("every triangle faces the way its normals do", outward);
    check("every triangle hangs from one bone", oneBone);
    bool closed = true;
    for (const auto &e : edges) closed = closed && e.second == 1 && edges.count({e.first.second, e.first.first}) == 1;
    check("every lump is closed: no edge without its other side", closed);

    // what is on a bone is near it: the head about the eyes, a hand's about its wrist, a finger's on its bone
    bool near = true, eyesIn = false;
    int onHead = 0, onHand[2] = {0, 0}, fingerBones = 0;
    bool seen[kRoles] = {};
    for (int v = 0; v < b.verts(); ++v) {
        const int role = b.bones[4 * v];
        const Vec p = vertex(b, v);
        seen[role] = true;
        if (role == kHead) ++onHead, near = near && (p - Vec{0, 1.6, 0}).norm() < 0.15;
        else if (role == kLeftHand || role == kRightHand) ++onHand[role == kRightHand], near = near && (p - at(b, role)).norm() < 0.12 && std::fabs(p.x) > std::fabs(b.at[role][0]) - 0.01;
        else near = near && role >= kLeftFingers && (p - at(b, role)).norm() < 0.06;
        eyesIn = eyesIn || (role == kHead && p.z < b.eyes[2] - 0.02 && std::fabs(p.y - b.eyes[1]) < 0.03);
    }
    for (int r = kLeftFingers; r < kRoles; ++r) fingerBones += seen[r];
    check("the head, both hands and all thirty finger bones have a mesh, and nothing else has", onHead > 100 && onHand[0] > 30 && onHand[0] == onHand[1] && fingerBones == 30 && near);
    check("the eyes are inside the head, behind the visor", eyesIn && b.parts.size() == 2 && b.parts[1].material == 1);
    bool mirrored = true;   // the left hand is the right one turned over
    for (int k = 0; k < 15; ++k) {
        const Vec l = at(b, kLeftFingers + k), r = at(b, kRightFingers + k);
        mirrored = mirrored && std::fabs(l.x + r.x) < 1e-6 && std::fabs(l.y - r.y) < 1e-6 && std::fabs(l.z - r.z) < 1e-6;
    }
    check("the left hand's bones are the right one's, turned over", mirrored);
}

static void worn() {
    const Body b = abstract::Body(0);
    const handfit::Fit fit(b);
    bool rest = true;
    for (int right = 0; right < 2; ++right) {
        bodypose::Hand hand;
        rest = rest && fit.Of(fit.rest(right), hand) && hand.tracked && hand.fingers && hand.turn.AngleTo(bodypose::Turn()) < 1e-6;
        for (int k = 0; k < bodypose::kFingerBones; ++k) rest = rest && hand.finger[k].AngleTo(bodypose::Turn()) < 1e-6;
        // the tracked hand's joints are the body's finger bones
        const int fingers = right ? kRightFingers : kLeftFingers;
        for (int f = 0; f < 5; ++f)
            for (int k = 0; k < 3; ++k) {
                const float *joint = fit.rest(right).p[4 * f + 1 + k];
                rest = rest && (Vec{joint[0], joint[1], joint[2]} - at(b, fingers + f * 3 + k)).norm() < 1e-6;
            }
    }
    check("a tracked hand fits both its hands, and its own hand at rest turns nothing", rest);

    bodypose::Solver solver(b);
    bodypose::Input in;
    bodypose::Pose pose;
    float bones[kRoles][12];
    check("it is posed as a waving wearer", demo::Pose("wave", 1, b, fit, in) && in.hand[1].tracked && solver.Solve(in, pose));
    solver.Matrices(pose, bones);
    Vec head, palm;
    int heads = 0, palms = 0;
    for (int v = 0; v < b.verts(); ++v) {
        if (b.bones[4 * v] == kHead) head = head + moved(bones[kHead], vertex(b, v)), ++heads;
        if (b.bones[4 * v] == kRightHand) palm = palm + moved(bones[kRightHand], vertex(b, v)), ++palms;
    }
    head = head * (1.0 / heads), palm = palm * (1.0 / palms);
    check("its head is where the wearer's is", (head - in.eyes).norm() < 0.1);
    check("its right hand is at the wearer's wrist, up beside the head", (palm - in.hand[1].wrist).norm() < 0.09 && palm.y > 1.4 && palm.x > 0.2);
    check("the wrist bone itself is on the wearer's wrist", (moved(bones[kRightHand], at(b, kRightHand)) - in.hand[1].wrist).norm() < 0.02);
}

int main() {
    format();
    colours();
    lumps();
    worn();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
