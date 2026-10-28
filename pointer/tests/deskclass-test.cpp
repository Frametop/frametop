// Unit tests for the desk classifier's features and model (pointer/helper/deskclass.h): made-up hands
// and a hand-written model.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/deskclass-test.cpp).
#include "pointer/helper/deskclass.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace desk;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

namespace {

constexpr int N = Features::kCount;
constexpr double kEye = 1.2;

// A flat hand, palm down, fingers ahead (-z), its palm's centre at c; mirror: the left hand's shape.
Hand Flat(V3 c, int side) {
    Hand h;
    h.seen = true, h.side = side, h.stereo = true, h.fitted = true;
    const double m = side ? 1 : -1;
    h.pts[0] = c + V3{0, 0, 0.05};
    for (int f = 0; f < 5; ++f) {
        const double x = m * (f == 0 ? -0.045 : -0.03 + 0.02 * (f - 1));
        for (int j = 1; j <= 4; ++j) h.pts[f * 4 + j] = c + V3{x, 0, 0.03 - 0.025 * j - (f ? 0.03 : 0)};
    }
    h.normal = {0, -1, 0};
    return h;
}

Head Ahead() { return {{0, kEye, 0}, {0, 0, -1}}; }

int Find(const char *name) {
    for (int i = 0; i < N; ++i)
        if (std::string(Features::Name(i)) == name) return i;
    return -1;
}

void TestNames() {
    for (int i = 0; i < N; ++i) CHECK(*Features::Name(i), "feature %d has no name", i);
    CHECK(Find("palm_x") == 0 && Find("hand_size") == N - 1, "names out of order");
}

// A left hand is mirrored: the same hand on the other side reads the same.
void TestMirror() {
    Features f;
    Hand hands[2] = {Flat({-0.1, kEye - 0.35, -0.3}, 0), Flat({0.1, kEye - 0.35, -0.3}, 1)};
    double out[2][N];
    bool ok[2];
    f.Frame(1.0, Ahead(), hands, out, ok);
    CHECK(ok[0] && ok[1], "both seen");
    for (int i = 0; i < N; ++i) CHECK(std::fabs(out[0][i] - out[1][i]) < 1e-9, "%s: left %g, right %g", Features::Name(i), out[0][i], out[1][i]);
    CHECK(std::fabs(out[1][Find("palm_y")] + 0.35) < 1e-9, "palm_y %g", out[1][Find("palm_y")]);
    CHECK(out[1][Find("normal_y")] < -0.99, "palm down");
    CHECK(out[1][Find("other_seen")] == 1 && std::fabs(out[1][Find("other_dist")] - 0.2) < 1e-9, "the other hand");
}

// The level frame turns with the head's heading, not its pitch: the same hand, head turned to +x.
void TestHeading() {
    Features a, b;
    Hand ha[2] = {Hand{}, Flat({0.1, kEye - 0.35, -0.3}, 1)};
    Hand hb[2] = {Hand{}, ha[1]};
    for (int i = 0; i < 21; ++i) hb[1].pts[i] = {-ha[1].pts[i].z, ha[1].pts[i].y, ha[1].pts[i].x};  // turned -90 deg about y
    double oa[2][N], ob[2][N];
    bool ok[2];
    a.Frame(1.0, Ahead(), ha, oa, ok);
    Head turned{{0, kEye, 0}, {1, 0, 0}};
    b.Frame(1.0, turned, hb, ob, ok);
    for (const char *n : {"palm_x", "palm_y", "palm_z", "dir_x", "dir_z", "tip_index"})
        CHECK(std::fabs(oa[1][Find(n)] - ob[1][Find(n)]) < 1e-9, "%s: %g vs %g", n, oa[1][Find(n)], ob[1][Find(n)]);
    Head down{{0, kEye, 0}, Unit(V3{0, -1, -1})};
    double od[2][N];
    a.Reset();
    a.Frame(2.0, down, ha, od, ok);
    CHECK(std::fabs(od[1][Find("head_pitch")] + M_PI / 4) < 1e-9, "pitch %g", od[1][Find("head_pitch")]);
    CHECK(std::fabs(od[1][Find("palm_z")] - oa[1][Find("palm_z")]) < 1e-9, "pitch doesn't move the level frame");
}

// The history: speed and wander from the frames before, started over after a gap.
void TestHistory() {
    Features f;
    double out[2][N];
    bool ok[2];
    for (int k = 0; k <= 30; ++k) {
        Hand hands[2] = {Hand{}, Flat({0.1 + 0.05 * std::sin(k * 0.4), kEye - 0.35, -0.3}, 1)};
        f.Frame(k / 30.0, Ahead(), hands, out, ok);
    }
    CHECK(out[1][Find("wander_x")] > 0.02, "a sliding hand wanders: %g", out[1][Find("wander_x")]);
    CHECK(out[1][Find("wander_y")] < 1e-6, "not up and down: %g", out[1][Find("wander_y")]);
    CHECK(out[1][Find("speed")] > 0.1, "moving: %g", out[1][Find("speed")]);
    Hand still[2] = {Hand{}, Flat({0.3, kEye - 0.35, -0.3}, 1)};
    f.Frame(1.0 + 1.0, Ahead(), still, out, ok);  // after a gap: no speed from the old frames
    CHECK(out[1][Find("speed")] == 0 && out[1][Find("wander_x")] == 0, "history kept over a gap: %g", out[1][Find("speed")]);
    Hand none[2];
    f.Frame(2.1, Ahead(), none, out, ok);
    CHECK(!ok[0] && !ok[1], "unseen hands get no features");
}

// A model: 2 hidden units over palm_y alone (low: desk), as desk_train.py writes one.
std::string TinyModel(int features = N) {
    std::string s = "deskclass 1\nfeatures " + std::to_string(features) + "\nclasses free keys mouse\n# a comment\nmean";
    for (int i = 0; i < features; ++i) s += " 0";
    s += "\nstd";
    for (int i = 0; i < features; ++i) s += " 1";
    s += "\nlayer " + std::to_string(features) + " 2 relu\n";
    for (int o = 0; o < 2; ++o) {
        for (int i = 0; i < features; ++i) s += i == 1 ? (o ? " -10" : " 10") : " 0";
        s += "\n";
    }
    s += "0 0\nlayer 2 3 none\n5 0\n0 1\n0 0.5\n0 0 0\n";
    return s;
}

void TestModel() {
    Model m;
    CHECK(m.Parse(TinyModel()), "parse: %s", m.error.c_str());
    double f[N] = {};
    f[1] = -0.35;  // low: the second unit, keys and mouse
    double p[Model::kClasses];
    m.Probs(f, p);
    CHECK(std::fabs(p[0] + p[1] + p[2] - 1) < 1e-9, "probabilities sum to 1");
    CHECK(m.Desk(f) > 0.9, "low hand at the desk: %g", m.Desk(f));
    f[1] = 0.1;
    CHECK(m.Desk(f) < 0.3, "raised hand free: %g", m.Desk(f));
    CHECK(!m.Parse(TinyModel(N - 1)) && !m.Loaded(), "other features refused");
    CHECK(!m.Parse("deskclass 1\nfeatures 30\nclasses free keys mouse\nmean 0\n"), "short mean refused");
    std::string cut = TinyModel();
    cut.resize(cut.size() - 4);
    CHECK(!m.Parse(cut), "missing biases refused");
    CHECK(!m.Load("/nonexistent/desk.txt") && !m.error.empty(), "no file");
    for (const char *bad : {"", "deskclass 2\n", "deskclass 1\nfeatures 30\nclasses free mouse keys\n",
                            "deskclass 1\nfeatures 30\nclasses free keys mouse\nmean x\n"}) {
        CHECK(!m.Parse(bad) && !m.Loaded() && !m.error.empty(), "refused: %s", bad);
    }
    std::string wrongOut = TinyModel();
    wrongOut.replace(wrongOut.find("layer 2 3"), 9, "layer 2 4");
    CHECK(!m.Parse(wrongOut), "a last layer not 3 classes refused");
    std::string gap = TinyModel();
    gap.replace(gap.find("layer 2 3"), 9, "layer 5 3");
    CHECK(!m.Parse(gap), "layers that don't chain refused");
    std::string huge = "deskclass 1\nfeatures 30\nclasses free keys mouse\nmean";
    for (int i = 0; i < N; ++i) huge += " 0";
    huge += "\nstd";
    for (int i = 0; i < N; ++i) huge += " 1";
    huge += "\nlayer 30 4000 relu\n";  // 120k weights
    CHECK(!m.Parse(huge) && m.error.find("too big") != std::string::npos, "too big refused: %s", m.error.c_str());
    // the file's size is checked before it is read
    const char *path = "/tmp/deskclass-test-big.txt";
    if (FILE *f = std::fopen(path, "w")) {
        std::string big = TinyModel() + "#" + std::string(Model::kMaxBytes, ' ') + "\n";  // valid, but too long
        CHECK(m.Parse(big), "the same text parses: %s", m.error.c_str());
        std::fwrite(big.data(), 1, big.size(), f);
        std::fclose(f);
        CHECK(!m.Load(path) && m.error.find("1 MB") != std::string::npos, "an oversize file refused: %s", m.error.c_str());
        std::remove(path);
    }
    CHECK(m.Parse(TinyModel()), "parses again after the failures: %s", m.error.c_str());
}

// Three layers: the hidden layers chain.
void TestDeepModel() {
    std::string s = "deskclass 1\nfeatures " + std::to_string(N) + "\nclasses free keys mouse\nmean";
    for (int i = 0; i < N; ++i) s += " 0";
    s += "\nstd";
    for (int i = 0; i < N; ++i) s += " 1";
    s += "\nlayer " + std::to_string(N) + " 1 relu\n";
    for (int i = 0; i < N; ++i) s += i == 1 ? " -10" : " 0";
    s += "\n0\nlayer 1 1 relu\n1\n0\nlayer 1 3 none\n0\n1\n0\n0 0 0\n";
    Model m;
    CHECK(m.Parse(s), "three layers: %s", m.error.c_str());
    double f[N] = {};
    f[1] = -0.5;
    double p[Model::kClasses];
    m.Probs(f, p);
    CHECK(std::fabs(p[1] - std::exp(5.0) / (std::exp(5.0) + 2)) < 1e-9, "low hand: logits 0 5 0 through both hidden layers (%g)", p[1]);
    f[1] = 0.5;
    m.Probs(f, p);
    CHECK(std::fabs(p[0] - 1.0 / 3) < 1e-9, "relu cuts it: even (%g)", p[0]);
}

// Time going back (a new recording, a restarted clock): the history starts over.
void TestClockBack() {
    Features f;
    double out[2][N];
    bool ok[2];
    for (int k = 0; k <= 30; ++k) {
        Hand hands[2] = {Hand{}, Flat({0.1 + 0.05 * std::sin(k * 0.4), kEye - 0.35, -0.3}, 1)};
        f.Frame(5 + k / 30.0, Ahead(), hands, out, ok);
    }
    CHECK(out[1][Find("speed")] > 0.1, "moving");
    Hand still[2] = {Hand{}, Flat({0.3, kEye - 0.35, -0.3}, 1)};
    f.Frame(1.0, Ahead(), still, out, ok);
    CHECK(out[1][Find("speed")] == 0 && out[1][Find("wander_x")] == 0, "history kept after time went back");
}

}  // namespace

int main() {
    TestNames();
    TestMirror();
    TestHeading();
    TestHistory();
    TestModel();
    TestDeepModel();
    TestClockBack();
    if (failures) std::printf("%d failures\n", failures);
    else std::printf("deskclass: all passed\n");
    return failures ? 1 : 0;
}
