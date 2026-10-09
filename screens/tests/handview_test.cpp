// Offline tests for ft-handview's CPU side: Room View's detector (roomview.h) against a fake
// shared state, and the rendered hand's mesh and square (handmesh.cpp). No OpenVR, GPU or headset.
//
// Build and run: the justfile's test-c.
#include "../handmesh.h"
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

int main() {
    TestRoomView();
    TestMesh();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
