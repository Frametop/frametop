// Offline tests for ft-handview's CPU side: Room View's detector (roomview.h) against a fake
// shared state, the rendered hand's mesh and square (handmesh.cpp), the hand model's posing
// (handskin.cpp), the robot hand's (handrobot.cpp), and the drawn hand between and past the
// tracking's frames (handhold.h). No OpenVR, GPU or headset.
//
// Build and run: the justfile's test-c.
#include "../handmesh.h"
#include "../handasset.h"
#include "../handhold.h"
#include "../handrobot.h"
#include "../handskin.h"
#include "../roomview.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_fail = 0, g_checks = 0;
void Check(bool ok, int line, const char *what) {
    ++g_checks;
    if (!ok) ++g_fail, std::printf("handview_test.cpp:%d: FAILED: %s\n", line, what);
}
#define CHECK(cond) Check((cond), __LINE__, #cond)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= (tol))

// A fake /dev/shm: the passthrough state with `enabled`, and a decoy of another size.
std::string FakeShm(int enabled) {
    static std::string dir;
    if (dir.empty()) {
        char tmpl[] = "/tmp/handview-test-XXXXXX";
        dir = mkdtemp(tmpl);
    }
    const std::string prefix = dir + "/u" + std::to_string(getuid()) + "-Shm_";
    std::string data(roomview::kStateSize, '\0');
    data[0] = 1, data[2] = char(enabled), data[3] = 1, data[4] = 1, data[6] = 1;
    FILE *f = std::fopen((prefix + "abc").c_str(), "wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    f = std::fopen((prefix + "decoy").c_str(), "wb");
    std::fwrite(data.data(), 1, 100, f);
    std::fclose(f);
    return dir;
}

void TestRoomView() {
    const int64_t s = 1'000'000'000;
    roomview::Detector d;
    d.SetDir(FakeShm(1));
    // no game: the construct's setting
    CHECK(d.Update(1 * s, false, true));
    CHECK(d.sharedEnabled() == 1);
    CHECK(!d.Update(2 * s, false, false));
    // a game: off until it's shown, then the events
    CHECK(!d.Update(3 * s, true, true));
    CHECK(!std::strcmp(d.why(), "VR game"));
    d.Event(roomview::kEventShown);
    CHECK(d.Update(4 * s, true, true));
    d.Event(roomview::kEventHidden);
    CHECK(!d.Update(5 * s, true, true));
    d.Event(12345);   // not ours
    CHECK(!d.Update(6 * s, true, true));
    // the game ends: its events are old
    d.Event(roomview::kEventHidden);
    CHECK(d.Update(7 * s, false, true));
    // passthrough disabled: off whatever else; enabled again: start over
    FakeShm(0);
    CHECK(!d.Update(8 * s, false, true));
    CHECK(d.sharedEnabled() == 0);
    CHECK(!std::strcmp(d.why(), "passthrough disabled"));
    FakeShm(1);
    CHECK(!d.Update(8 * s + 100, false, true));   // read at most twice a second
    CHECK(d.Update(9 * s, false, true));
    // forced
    d.SetForce(roomview::Force::Off);
    CHECK(!d.Update(10 * s, false, true));
    d.SetForce(roomview::Force::On);
    CHECK(d.Update(11 * s, true, false));
    roomview::Force f;
    CHECK(roomview::ParseForce("off", &f) && f == roomview::Force::Off);
    CHECK(!roomview::ParseForce("maybe", &f));
    // no state at all: the rest decides
    roomview::Detector none;
    none.SetDir("/nonexistent");
    CHECK(none.Update(1 * s, false, true));
    CHECK(none.sharedEnabled() == -1);
}

// An open hand, fingers up, palm toward a head at the origin, `z` metres ahead (OpenVR: -z).
handcut::Hand OpenHand(double x, double y, double z) {
    handcut::Hand h;
    for (int j = 0; j < 21; ++j) {
        const int finger = j == 0 ? 0 : (j - 1) / 4, k = j == 0 ? 0 : (j - 1) % 4 + 1;
        h.p[j][0] = float(x + (finger - 2) * 0.02);
        h.p[j][1] = float(y + (j == 0 ? 0 : 0.06 + 0.03 * k));
        h.p[j][2] = float(-z);
        h.radius[j] = 0.008f;
    }
    for (int b = 0; b < 20; ++b) h.bone[b] = 0.03f;
    h.elbow[0] = float(x), h.elbow[1] = float(y - 0.25), h.elbow[2] = float(-z + 0.05);
    h.hasElbow = true;
    h.v[0] = h.v[1] = h.v[2] = 0;
    return h;
}

void Apply(const float m[16], const float p[3], double out[4]) {
    for (int r = 0; r < 4; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];   // column-major
}

void TestMesh() {
    // a hull: a unit cube's 8 corners make 12 faces' worth of triangles, all outward
    handmesh::Mesh m;
    std::vector<std::array<float, 3>> cube;
    for (int i = 0; i < 8; ++i) cube.push_back({float(i & 1), float(i >> 1 & 1), float(i >> 2 & 1)});
    handmesh::AddHull(cube, m);
    CHECK(m.idx.size() >= 36 && m.idx.size() % 3 == 0);
    bool outward = true;
    for (const handmesh::Vertex &v : m.v) {
        const double d = (v.p[0] - 0.5) * v.n[0] + (v.p[1] - 0.5) * v.n[1] + (v.p[2] - 0.5) * v.n[2];
        outward &= d > 0.4;
    }
    CHECK(outward);
    // a capsule: every vertex at its radius from the axis' ends or side
    m.clear();
    handmesh::AddCapsule({{0, 0, 0}, {0, 0.1f, 0}, 0.01f, 0.01f}, m);
    bool round = !m.v.empty();
    for (const handmesh::Vertex &v : m.v) {
        const double y = std::clamp(double(v.p[1]), 0.0, 0.1);
        const double r = std::sqrt(v.p[0] * v.p[0] + (v.p[1] - y) * (v.p[1] - y) + v.p[2] * v.p[2]);
        round &= std::fabs(r - 0.01) < 1e-5;
    }
    CHECK(round);
    for (uint16_t i : m.idx) CHECK(i < m.v.size());

    // the hand: a mesh, the forearm behind the wrist
    const handcut::Hand h = OpenHand(0.1, -0.2, 0.45);
    const double head[3] = {0, 0, 0};
    handmesh::Build(h, head, m);
    CHECK(m.v.size() > 500);
    float maxBack = -1, minBack = 1;
    for (const handmesh::Vertex &v : m.v) maxBack = std::max(maxBack, v.back), minBack = std::min(minBack, v.back);
    NEAR(maxBack, 0.11, 1e-4);  // the forearm's stub, to where it has faded out
    CHECK(minBack < -0.15);     // the fingertips

    // its square: at the hand, facing the head, holding what each eye sees of it
    const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}};
    handmesh::Quad q;
    CHECK(handmesh::Place(m, eyes, 0.04, q));
    NEAR(q.distance, std::sqrt(0.1 * 0.1 + 0.2 * 0.2 + 0.45 * 0.45), 0.08);
    CHECK(q.size > 0.15 && q.size < 0.45);
    const double toHead[3] = {-q.pose.m[0][3], -q.pose.m[1][3], -q.pose.m[2][3]};
    const double n = std::sqrt(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
    NEAR((q.pose.m[0][2] * toHead[0] + q.pose.m[1][2] * toHead[1] + q.pose.m[2][2] * toHead[2]) / n, 1, 1e-3);   // +z to the head
    CHECK(std::fabs(q.pose.m[0][1]) < 1e-6 || q.pose.m[1][1] > 0.8f);   // +y up (level square)
    for (int e = 0; e < 2; ++e) {
        float mvp[16];
        handmesh::EyeMatrix(q, eyes[e], 0.05, q.distance + 1, mvp);
        // every vertex inside the viewport, in front of the near plane
        bool inside = true;
        for (const handmesh::Vertex &v : m.v) {
            double c[4];
            Apply(mvp, v.p, c);
            inside &= c[3] > 0.05 && std::fabs(c[0] / c[3]) <= 1 && std::fabs(c[1] / c[3]) <= 1 && std::fabs(c[2] / c[3]) <= 1;
        }
        CHECK(inside);
        // a point on the square lands where it is on it: its top at the buffer's top (y -1)
        const double lx = 0.3 * q.size / 2, ly = 0.6 * q.size / 2;
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = float(q.pose.m[i][3] + q.pose.m[i][0] * lx + q.pose.m[i][1] * ly);
        double c[4];
        Apply(mvp, p, c);
        NEAR(c[0] / c[3], 0.3, 1e-3);
        NEAR(c[1] / c[3], -0.6, 1e-3);
    }
    // the eyes see a near point at different places: depth (the right eye sees it further left)
    float mvpL[16], mvpR[16];
    handmesh::EyeMatrix(q, eyes[0], 0.05, 2, mvpL), handmesh::EyeMatrix(q, eyes[1], 0.05, 2, mvpR);
    float nearPt[3];
    for (int i = 0; i < 3; ++i) nearPt[i] = float(q.pose.m[i][3] + q.pose.m[i][2] * 0.1);
    double cl[4], cr[4];
    Apply(mvpL, nearPt, cl), Apply(mvpR, nearPt, cr);
    CHECK(cr[0] / cr[3] < cl[0] / cl[3] - 0.01);
    // further depth is further in the depth buffer
    float farPt[3];
    for (int i = 0; i < 3; ++i) farPt[i] = float(q.pose.m[i][3] - q.pose.m[i][2] * 0.1);
    double cf[4];
    Apply(mvpL, farPt, cf);
    CHECK(cf[2] / cf[3] > cl[2] / cl[3]);
    // too close to the eyes: not drawn
    const handcut::Hand close = OpenHand(0, -0.05, 0.03);
    handmesh::Build(close, head, m);
    CHECK(!handmesh::Place(m, eyes, 0.04, q));
}

}  // namespace

// Turns p by `rad` about the unit axis through `at` (Rodrigues).
void Turn(float p[3], const float at[3], const double axis[3], double rad) {
    const double v[3] = {p[0] - at[0], p[1] - at[1], p[2] - at[2]};
    const double c = std::cos(rad), s = std::sin(rad), d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    const double x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
    for (int i = 0; i < 3; ++i) p[i] = float(at[i] + v[i] * c + x[i] * s + axis[i] * d * (1 - c));
}

// How far joint j's matrix turns things (degrees), and how far it moves the point q (metres).
double Angle(const handskin::Bones &b, int j) {
    const float *m = b.m[j];
    const double s = std::cbrt(m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]));
    const double tr = (m[0] + m[5] + m[10]) / s;
    return std::acos(std::clamp((tr - 1) / 2, -1.0, 1.0)) * 180 / M_PI;
}

void TestModels() {
    // the default first (HANDS_VIEW_MODEL=asset), each found by its name, nothing else
    CHECK(handasset::kModels >= 2 && !std::strcmp(handasset::kModel[0].name, "asset"));
    for (int k = 0; k < handasset::kModels; ++k) CHECK(handskin::FindModel(handasset::kModel[k].name) == k);
    CHECK(handskin::FindModel("capsules") == -1 && handskin::FindModel("") == -1);
    for (int model = 0; model < handasset::kModels; ++model)
        for (bool right : {false, true}) {
            const handasset::Hand &a = handasset::kModel[model].hands[right];
            // every vertex's weights add up, on WebXR's joints; every triangle's corners exist
            bool ok = a.verts > 0 && a.tris > 0;
            for (int i = 0; i < a.verts && ok; ++i) {
                int sum = 0;
                for (int k = 0; k < 4; ++k) sum += a.weights[4 * i + k], ok = ok && a.joints[4 * i + k] < handskin::kJoints;
                ok = ok && sum == 255;
            }
            for (int i = 0; i < 3 * a.tris && ok; ++i) ok = a.idx[i] < a.verts;
            CHECK(ok);
            // each fingertip's joint at the end of its finger: the mesh reaches at most 14 mm
            // past it (generic-hand's sit about a centimetre in) and not 3 mm short of it
            for (int f = 0; f < 5; ++f) {
                const int tip = f ? 4 + 5 * f : 4, distal = tip - 1;
                double d[3], n = 0, reach = -1;
                for (int i = 0; i < 3; ++i) d[i] = a.bind[tip][i] - a.bind[distal][i], n += d[i] * d[i];
                for (int i = 0; i < a.verts; ++i) {
                    int w = 0;
                    for (int k = 0; k < 4; ++k) w += a.joints[4 * i + k] == distal || a.joints[4 * i + k] == tip ? a.weights[4 * i + k] : 0;
                    if (w <= 127) continue;
                    float p[3];
                    handskin::BindVertex(model, right, i, p);
                    double along = 0;
                    for (int k = 0; k < 3; ++k) along += (p[k] - a.bind[tip][k]) * d[k] / std::sqrt(n);
                    reach = std::max(reach, along);
                }
                CHECK(reach > -0.003 && reach < 0.014);
            }
            // the side's own hand, not the other mirrored: (index knuckle - wrist) x (little knuckle
            // - wrist) points out of a right hand's palm and a left one's back, and the thumb's tip
            // sits in front of the palm (it opposes the fingers; 5-7 cm in every model's bind pose)
            const float *w = a.bind[0], *ix = a.bind[6], *lt = a.bind[21], *th = a.bind[4];
            const double u[3] = {ix[0] - w[0], ix[1] - w[1], ix[2] - w[2]}, v[3] = {lt[0] - w[0], lt[1] - w[1], lt[2] - w[2]};
            const double nrm[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            const double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            const double front = ((th[0] - w[0]) * nrm[0] + (th[1] - w[1]) * nrm[1] + (th[2] - w[2]) * nrm[2]) / len;
            CHECK((right ? front : -front) > 0.02);
        }
}

void TestSkin(int model) {
    for (bool right : {false, true}) {
        // the model's own pose: every matrix the identity, every vertex where it was
        handskin::Bones b;
        const handcut::Hand bind = handskin::BindHand(model, right);
        CHECK(handskin::Pose(model, bind, b));
        CHECK(b.model == model);
        double worst = 0;
        for (int j = 0; j < handskin::kJoints; ++j)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(b.m[j][r * 4 + c] - (r == c ? 1.0 : 0.0)));
        CHECK(worst < 1e-4);
        NEAR(b.scale, 1, 1e-4);
        double moved = 0;
        for (int i = 0; i < handskin::Vertices(model, right); ++i) {
            float p[3], q[3];
            handskin::BindVertex(model, right, i, p);
            handskin::Skin(right, b, i, q);
            moved = std::max(moved, double(std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2])));
        }
        CHECK(moved < 1e-4);

        // moved and turned whole, and 20% bigger: the model follows, scaled
        handcut::Hand big = bind;
        const double axis[3] = {0.48, 0.6, 0.64};
        const float origin[3] = {0, 0, 0};
        for (auto &p : big.p) {
            Turn(p, origin, axis, 1.1);
            for (int i = 0; i < 3; ++i) p[i] = p[i] * 1.2f + (i == 2 ? -0.5f : 0.1f);
        }
        CHECK(handskin::Pose(model, big, b));
        NEAR(b.scale, 1.2, 1e-3);
        double off = 0;
        for (int i = 0; i < handskin::Vertices(model, right); i += 7) {
            float p[3], q[3];
            handskin::BindVertex(model, right, i, p);
            Turn(p, origin, axis, 1.1);
            for (int k = 0; k < 3; ++k) p[k] = p[k] * 1.2f + (k == 2 ? -0.5f : 0.1f);
            handskin::Skin(right, b, i, q);
            off = std::max(off, double(std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2])));
        }
        CHECK(off < 1e-3);

        // the index finger bent 60 degrees at its middle joint (MediaPipe 6): its last two bones
        // (the model's index intermediate, distal and tip joints, 7-9) turn that much, nothing else
        handcut::Hand bent = bind;
        const float *pip = bind.p[6];
        double ax[3];
        {
            const double d[3] = {bind.p[7][0] - pip[0], bind.p[7][1] - pip[1], bind.p[7][2] - pip[2]};
            const double a[3] = {bind.p[17][0] - bind.p[5][0], bind.p[17][1] - bind.p[5][1], bind.p[17][2] - bind.p[5][2]};
            // the bending axis: across the palm, square to the bone
            const double k = (a[0] * d[0] + a[1] * d[1] + a[2] * d[2]) / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int i = 0; i < 3; ++i) ax[i] = a[i] - k * d[i];
            const double n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            for (double &v : ax) v /= n;
        }
        for (int j : {7, 8}) Turn(bent.p[j], pip, ax, 60 * M_PI / 180);
        CHECK(handskin::Pose(model, bent, b));
        for (int j = 0; j < handskin::kJoints; ++j) {
            const double a = Angle(b, j);
            if (j >= 7 && j <= 9) NEAR(a, 60, 2);   // a bind pose's twisted finger rolls a little
            else CHECK(a < 0.5);
        }
        // a vertex of the index fingertip went with it (most of its weight on the last bones: MRTK2's
        // share some with the one before); one of the little finger's stayed
        const handasset::Hand &a = handasset::kModel[model].hands[right];
        int tipV = -1, littleV = -1;
        for (int i = 0; i < a.verts; ++i) {   // its weight on the index's last bones, or all on the little finger's
            int index = 0, little = 0;
            for (int k = 0; k < 4; ++k) {
                const int j = a.joints[4 * i + k], w = a.weights[4 * i + k];
                index += j >= 8 && j <= 9 ? w : 0, little += j >= 22 && j <= 24 ? w : 0;
            }
            if (index >= 200 && tipV < 0) tipV = i;
            if (little == 255 && littleV < 0) littleV = i;
        }
        CHECK(tipV >= 0 && littleV >= 0);
        if (tipV >= 0 && littleV >= 0) {
            float p[3], q[3];
            handskin::BindVertex(model, right, tipV, p), handskin::Skin(right, b, tipV, q);
            CHECK(std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) > 0.01);
            handskin::BindVertex(model, right, littleV, p), handskin::Skin(right, b, littleV, q);
            CHECK(std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) < 1e-4);
        }
    }
    // left and right: the same pose mirrored gives the mirrored hand (the model's two hands
    // aren't one mirrored mesh, so compare their extents and middles)
    for (const char *pose : {"open", "fist", "pinch"}) {
        handskin::Bones b[2];
        double lo[2][3], hi[2][3], mid[2][3];
        for (int right = 0; right < 2; ++right) {
            const double side = right ? 1 : -1, at[3] = {0.1 * side, -0.2, -0.4}, palm[3] = {-0.3 * side, 0.2, 1},
                         up[3] = {0.2 * side, 1, -0.2};
            handcut::Hand h;
            CHECK(handskin::DemoPose(pose, right, at, palm, up, 0, h) && h.right == bool(right));
            CHECK(handskin::Pose(model, h, b[right]));
            const int n = handskin::Vertices(model, right);
            for (int i = 0; i < 3; ++i) lo[right][i] = 1e9, hi[right][i] = -1e9, mid[right][i] = 0;
            for (int v = 0; v < n; ++v) {
                float q[3];
                handskin::Skin(right, b[right], v, q);
                for (int i = 0; i < 3; ++i) {
                    const double x = i == 0 ? q[i] * side : q[i];   // the left hand flipped across x = 0
                    lo[right][i] = std::min(lo[right][i], x), hi[right][i] = std::max(hi[right][i], x), mid[right][i] += x / n;
                }
            }
            // the model's thumb and index tips where the tracked ones are
            for (int k : {4, 8}) {
                const int j = handskin::kFromMediaPipe[k];
                const float *m = b[right].m[j];
                float tip[3];
                const handcut::Hand bh = handskin::BindHand(model, right);
                for (int r = 0; r < 3; ++r)
                    tip[r] = m[r * 4] * bh.p[k][0] + m[r * 4 + 1] * bh.p[k][1] + m[r * 4 + 2] * bh.p[k][2] + m[r * 4 + 3];
                NEAR(std::hypot(tip[0] - h.p[k][0], tip[1] - h.p[k][1], tip[2] - h.p[k][2]), 0, 1e-4);
            }
        }
        for (int i = 0; i < 3; ++i) {
            NEAR(lo[0][i], lo[1][i], 0.008), NEAR(hi[0][i], hi[1][i], 0.008);
            NEAR(mid[0][i], mid[1][i], 0.005);
        }
    }
    // a degenerate hand: no pose
    handcut::Hand flat;
    for (auto &p : flat.p) p[0] = p[1] = p[2] = 0;
    handskin::Bones b;
    CHECK(!handskin::Pose(model, flat, b));
}

// A demo pose of that side, mirrored across x = 0 for the left (as --snapshot draws them).
handcut::Hand Demo(const char *pose, bool right) {
    const double side = right ? 1 : -1, at[3] = {0.1 * side, -0.2, -0.4}, palm[3] = {-0.3 * side, 0.2, 1}, up[3] = {0.2 * side, 1, -0.2};
    handcut::Hand h;
    CHECK(handskin::DemoPose(pose, right, at, palm, up, 0, h));
    return h;
}

double Dist(const float a[3], const float b[3]) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

void TestRobot() {
    const handrobot::Mesh &m = handrobot::Model();
    // the mesh: every triangle's corners exist, every vertex on a segment, fixed to an end, with a
    // material and a unit normal; about as many triangles as the models (it draws once, they twice)
    CHECK(!m.v.empty() && m.idx.size() % 3 == 0 && m.v.size() < 65536);
    bool ok = true;
    for (uint16_t i : m.idx) ok = ok && i < m.v.size();
    int mats[4] = {};
    for (const handrobot::Vertex &v : m.v) {
        ok = ok && v.seg >= 0 && v.seg < handrobot::kSegs && v.seg == std::floor(v.seg) && (v.end == 0 || v.end == 1);
        ok = ok && v.mat >= 0 && v.mat <= 3 && std::fabs(std::hypot(v.n[0], v.n[1], v.n[2]) - 1) < 1e-3;
        if (v.mat >= 0 && v.mat <= 3) ++mats[int(v.mat)];
    }
    CHECK(ok);
    for (int k = 0; k < 4; ++k) CHECK(mats[k] > 0);
    std::printf("robot: %zu vertices, %zu triangles\n", m.v.size(), m.idx.size() / 3);
    CHECK(m.idx.size() / 3 < 4500);
    // its normals point out: away from its segment's bone line (shells, joints) for most vertices
    // of the fingers
    int out = 0, fingers = 0;
    for (const handrobot::Vertex &v : m.v)
        if (v.seg >= 4 && v.mat == handrobot::kShell) ++fingers, out += v.p[0] * v.n[0] + v.p[1] * v.n[1] > -1e-6;
    CHECK(out > 0.97 * fingers);

    for (const char *pose : {"open", "fist", "pinch", "point", "spread"}) {
        handrobot::Bones b[2];
        handcut::Hand h[2];
        for (int right = 0; right < 2; ++right) {
            h[right] = Demo(pose, right);
            CHECK(handrobot::Pose(h[right], b[right]));
            const handcut::Hand &hh = h[right];
            // each segment starts at its joint, along its bone
            for (int k = 0; k < handrobot::kSegs; ++k) {
                const float *s = b[right].seg[k];
                const float o[3] = {s[3], s[7], s[11]};
                NEAR(Dist(o, hh.p[handrobot::kFrom[k]]), 0, 1e-5);
                const float L = Dist(hh.p[handrobot::kTo[k]], hh.p[handrobot::kFrom[k]]);
                NEAR(s[12] * b[right].scale, L, 1e-4);
                // its z axis toward its end joint
                double along = 0;
                for (int i = 0; i < 3; ++i) along += s[i * 4 + 2] / b[right].scale * (hh.p[handrobot::kTo[k]][i] - o[i]) / L;
                NEAR(along, 1, 1e-4);
            }
            // every vertex within its bone's length along it (a ball pokes out behind its joint),
            // and the fingertips' furthest reach at the tracked tips
            double worstPast = -1, worstBehind = -1, tipOff[5] = {1, 1, 1, 1, 1};
            for (const handrobot::Vertex &v : m.v) {
                const int k = int(v.seg);
                if (k == 0) continue;
                float p[3];
                handrobot::Place(b[right], v, p);
                const float *a = hh.p[handrobot::kFrom[k]], *e = hh.p[handrobot::kTo[k]];
                const double L = Dist(a, e);
                double along = 0;
                for (int i = 0; i < 3; ++i) along += (p[i] - a[i]) * (e[i] - a[i]) / L;
                worstPast = std::max(worstPast, along - L);
                worstBehind = std::max(worstBehind, -along);
                if (k == 3 || (k >= 6 && (k - 6) % 3 == 0)) {
                    const int f = k == 3 ? 0 : 1 + (k - 6) / 3;
                    tipOff[f] = std::min(tipOff[f], Dist(p, e));
                }
            }
            CHECK(worstPast < 1e-4);
            CHECK(worstBehind < 0.0115);   // the thumb's base ball, 10.8 mm
            for (double d : tipOff) CHECK(d < 0.0012);
        }
        // the left hand is the right one mirrored, vertex for vertex
        double off = 0;
        for (const handrobot::Vertex &v : m.v) {
            float l[3], r[3];
            handrobot::Place(b[0], v, l), handrobot::Place(b[1], v, r);
            off = std::max(off, double(std::hypot(l[0] + r[0], l[1] - r[1], l[2] - r[2])));
        }
        CHECK(off < 1e-5);
        // rigid: in a pose with the same bone lengths as the open hand, each segment's vertices keep
        // their distances to each other
        if (!std::strcmp(pose, "fist")) {
            handrobot::Bones open;
            CHECK(handrobot::Pose(Demo("open", true), open));
            double worst = 0;
            for (size_t i = 0; i < m.v.size(); i += 5)
                for (size_t j = i + 1; j < m.v.size(); j += 37) {
                    if (m.v[i].seg != m.v[j].seg || m.v[i].seg < 4) continue;   // the fingers (the thumb's lengths differ)
                    float a0[3], a1[3], c0[3], c1[3];
                    handrobot::Place(open, m.v[i], a0), handrobot::Place(open, m.v[j], a1);
                    handrobot::Place(b[1], m.v[i], c0), handrobot::Place(b[1], m.v[j], c1);
                    worst = std::max(worst, std::fabs(Dist(a0, a1) - Dist(c0, c1)));
                }
            CHECK(worst < 1e-5);
        }
        // the palm's side: its glowing dot toward the palm, the knuckles' line on the back; the
        // fingertips' pads on the palm side of the open hand, the thumb's facing the index
        for (int right = 0; right < 2; ++right) {
            const handcut::Hand &hh = h[right];
            double palm[3];
            {
                const double side = right ? 1 : -1, raw[3] = {-0.3 * side, 0.2, 1};
                const double n = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
                for (int i = 0; i < 3; ++i) palm[i] = raw[i] / n;
            }
            for (const handrobot::Vertex &v : m.v) {
                if (v.mat != handrobot::kAccent) continue;
                float p[3];
                handrobot::Place(b[right], v, p);
                const double toward = (p[0] - hh.p[0][0]) * palm[0] + (p[1] - hh.p[0][1]) * palm[1] + (p[2] - hh.p[0][2]) * palm[2];
                const double z = v.p[2] + v.end * handrobot::ReferenceLength(0);   // on the reference hand
                if (v.seg == 0 && z > 0.035 && z < 0.08 && std::fabs(v.p[1]) > 0.01) CHECK(v.p[1] > 0 ? toward > 0.008 : toward < -0.008);
                if (!std::strcmp(pose, "open") && v.seg >= 6 && (int(v.seg) - 6) % 3 == 0) {
                    const float *tip = hh.p[handrobot::kTo[int(v.seg)]];
                    CHECK((p[0] - tip[0]) * palm[0] + (p[1] - tip[1]) * palm[1] + (p[2] - tip[2]) * palm[2] > 0);
                }
            }
        }
        if (!std::strcmp(pose, "pinch")) {
            // the thumb's pad and the index's: their middles within 15 mm (they meet)
            for (int right = 0; right < 2; ++right) {
                double c[2][3] = {};
                int n[2] = {};
                for (const handrobot::Vertex &v : m.v) {
                    if (v.mat != handrobot::kAccent || (v.seg != 3 && v.seg != 6)) continue;
                    const int w = v.seg == 6;
                    float p[3];
                    handrobot::Place(b[right], v, p);
                    for (int i = 0; i < 3; ++i) c[w][i] += p[i];
                    ++n[w];
                }
                CHECK(n[0] > 0 && n[1] > 0);
                if (n[0] && n[1]) NEAR(std::hypot(c[0][0] / n[0] - c[1][0] / n[1], c[0][1] / n[0] - c[1][1] / n[1], c[0][2] / n[0] - c[1][2] / n[1]), 0, 0.015);
            }
        }
    }
    // a longer finger: its shells' ends stay as they are (only the middle stretches), the tip at the tip
    {
        handcut::Hand h = Demo("open", true), longer = h;
        for (int j : {7, 8})
            for (int i = 0; i < 3; ++i) longer.p[j][i] += 0.2f * (h.p[7][i] - h.p[6][i]);
        handrobot::Bones a, b;
        CHECK(handrobot::Pose(h, a) && handrobot::Pose(longer, b));
        const double stretch = 0.2 * Dist(h.p[7], h.p[6]);
        double startMoved = 0, endOff = 0;
        for (const handrobot::Vertex &v : m.v)
            if (v.seg == 5) {
                float p[3], q[3];
                handrobot::Place(a, v, p), handrobot::Place(b, v, q);
                if (v.end == 0) startMoved = std::max(startMoved, Dist(p, q));
                else endOff = std::max(endOff, std::fabs(Dist(p, q) - stretch));
            }
        CHECK(startMoved < 1e-5);
        CHECK(endOff < 1e-5);
    }
    // a degenerate hand: no pose
    handcut::Hand flat;
    for (auto &p : flat.p) p[0] = p[1] = p[2] = 0;
    handrobot::Bones b;
    CHECK(!handrobot::Pose(flat, b));
}

// handhold.h: blending between frames, holding a lost hand, refusing a jump
handcut::Hand HeldHand(double x, double y = 0, double z = -0.4, bool flipped = false) {
    handcut::Hand h{};
    h.right = true;
    for (int j = 0; j < 21; ++j) h.p[j][0] = float(x), h.p[j][1] = float(y + 0.01 * j), h.p[j][2] = float(z);
    // a palm: the wrist, the index knuckle up, the little knuckle beside it (flipped: the other side)
    h.p[5][1] = float(y + 0.08), h.p[17][1] = float(y + 0.07);
    h.p[17][0] = float(x + (flipped ? -0.06 : 0.06));
    for (int j : {0, 9, 13}) h.p[j][1] = float(y + (j ? 0.08 : 0));
    return h;
}

double PalmX(const handcut::Hand *h) {
    double p[3];
    h->Palm(p);
    return p[0];
}

void TestHold() {
    handhold::Params k;
    handhold::Side s;
    const int64_t ms = 1'000'000;
    int64_t t = 1000 * ms;
    uint64_t frame = 1;
    // first seen: drawn on the second frame that agrees
    handcut::Hand a = HeldHand(0.0);
    CHECK(!s.Update(&a, frame, t, k));
    a = HeldHand(0.01), t += 33 * ms, ++frame;
    const handcut::Hand *d = s.Update(&a, frame, t, k);
    CHECK(d && std::fabs(PalmX(d) - PalmX(&a)) < 1e-6);
    // a new frame 3 cm on: blended in over about the frame's interval
    handcut::Hand b = HeldHand(0.04);
    t += 33 * ms, ++frame;
    d = s.Update(&b, frame, t, k);
    NEAR(PalmX(d), PalmX(&a), 1e-6);   // starts where it was drawn
    CHECK(s.blending(t + 10 * ms));
    d = s.Update(&b, frame, t + 16 * ms, k);
    CHECK(PalmX(d) > PalmX(&a) + 0.01 && PalmX(d) < PalmX(&b) - 0.01);
    d = s.Update(&b, frame, t + 40 * ms, k);
    NEAR(PalmX(d), PalmX(&b), 1e-6);
    CHECK(!s.blending(t + 40 * ms));
    t += 40 * ms;
    // one misread frame 60 cm off: not drawn; the next one back on the track is
    handcut::Hand wild = HeldHand(0.64), c = HeldHand(0.05);
    t += 33 * ms, ++frame;
    d = s.Update(&wild, frame, t, k);
    NEAR(PalmX(d), PalmX(&b), 1e-6);
    CHECK(s.frozen() && s.jumps == 1);
    t += 33 * ms, ++frame;
    s.Update(&c, frame, t, k);
    CHECK(!s.frozen());
    t += 80 * ms;
    d = s.Update(&c, frame, t, k);
    NEAR(PalmX(d), PalmX(&c), 1e-6);
    // the palm turned over in one frame: a misreading too
    handcut::Hand over = HeldHand(0.05, 0, -0.4, true);
    t += 33 * ms, ++frame;
    d = s.Update(&over, frame, t, k);
    CHECK(s.frozen() && s.jumps == 2);
    // a real move far away (two frames agreeing): drawn there on the second, straight there
    handcut::Hand far1 = HeldHand(1.2), far2 = HeldHand(1.21);   // (146 ms since the last frame taken)
    t += 33 * ms, ++frame;
    s.Update(&far1, frame, t, k);
    t += 33 * ms, ++frame;
    d = s.Update(&far2, frame, t, k);
    CHECK(!s.frozen());
    NEAR(PalmX(d), PalmX(&far2), 1e-6);
    // fast but steady (5 m/s, 16.5 cm a frame): followed frame by frame
    handcut::Hand fast = far2;
    bool kept = true;
    for (int i = 0; i < 5; ++i) {
        fast = HeldHand(1.21 - 0.165 * (i + 1));
        t += 33 * ms, ++frame;
        s.Update(&fast, frame, t, k);
        kept = kept && !s.frozen();
    }
    CHECK(kept);
    // lost: held still for holdS, then gone
    t += 100 * ms;
    const double lastX = PalmX(s.Update(&fast, frame, t, k));
    t += 33 * ms, ++frame;
    d = s.Update(nullptr, frame, t, k);
    CHECK(d && s.frozen() && s.holds == 1);
    NEAR(PalmX(d), lastX, 1e-6);
    d = s.Update(nullptr, frame, t + int64_t(k.holdS * 1000 - 50) * ms, k);
    CHECK(d);
    // back within the hold, 30 cm away: drawn there on the second frame, without sweeping across
    handcut::Hand back1 = HeldHand(lastX + 0.3), back2 = HeldHand(lastX + 0.31);
    t += 500 * ms, ++frame;
    d = s.Update(&back1, frame, t, k);
    NEAR(PalmX(d), lastX, 1e-6);
    t += 33 * ms, ++frame;
    d = s.Update(&back2, frame, t, k);
    NEAR(PalmX(d), PalmX(&back2), 1e-6);
    // lost past the hold: nothing
    t += 33 * ms, ++frame;
    s.Update(nullptr, frame, t, k);
    CHECK(!s.Update(nullptr, frame, t + int64_t(k.holdS * 1000 + 50) * ms, k));
    // a reset (a controller taken up): gone at once, and a hand seen after starts afresh
    s.Update(&back2, ++frame, t + 4000 * ms, k);
    s.Reset();
    CHECK(!s.Update(nullptr, frame, t + 4010 * ms, k));
}

int main() {
    TestRoomView();
    TestHold();
    TestRobot();
    TestMesh();
    TestModels();
    for (int model = 0; model < handasset::kModels; ++model) {
        const int before = g_fail;
        TestSkin(model);
        if (g_fail > before) std::printf("  (model %s)\n", handasset::kModel[model].name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
