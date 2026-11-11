// The rules of drawing where the others point (together/points): which word of a pointer
// stands and for how long (marks.h), which of ft-screens' panels shows each of the room's
// (panelkeys.h), where a point of a picture is on a flat or a bent panel (surface.h), and the
// mark's picture (dotimage.h). No sockets, no SteamVR.
#include "../points/dotimage.h"
#include "../points/marks.h"
#include "../points/panelkeys.h"
#include "../points/surface.h"

#include <cstdio>

using namespace points;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static bool near(double a, double b, double by = 1e-9) { return std::fabs(a - b) <= by; }

// What ft-together sends one who asked `points`.
static std::vector<uint8_t> told(int place, uint8_t panel, uint16_t x, uint16_t y, uint8_t flags, uint16_t count = 1) {
    proto::Point p;
    p.count = count, p.panel = panel, p.x = x, p.y = y, p.flags = flags;
    std::vector<uint8_t> out = {local::kPointToWatcher, uint8_t(place)};
    const std::vector<uint8_t> body = proto::Pack(p);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static void marks() {
    Marks m;
    check("nobody points anywhere before anyone says so", !m.At(0, 0).shown && !m.At(3, 0).shown);
    std::vector<uint8_t> d = told(1, 9, 16384, 49151, 0);
    check("a pointer's word is taken", m.Take(d.data(), d.size(), 10));
    Mark at = m.At(1, 10.1);
    check("...and its person points there: the panel, and across and down it as parts of the whole", at.shown && at.panel == 9 && near(at.x, 16384 / 65535.0) && near(at.y, 49151 / 65535.0) && !at.pressed);
    check("...and nobody else does", !m.At(0, 10.1).shown && !m.At(2, 10.1).shown);
    check("the far edge is 1", (d = told(1, 9, 65535, 65535, 0), m.Take(d.data(), d.size(), 10.2)) && near(m.At(1, 10.2).x, 1) && near(m.At(1, 10.2).y, 1));
    d = told(1, 9, 100, 200, proto::kPointPressed);
    check("a held button is told", m.Take(d.data(), d.size(), 10.3) && m.At(1, 10.3).pressed);
    check("a pointer stands while it is heard of", m.At(1, 10.3 + Marks::kSilentS).shown);
    check("...and is gone when it hasn't been for too long", !m.At(1, 10.3 + Marks::kSilentS + 0.01).shown);
    d = told(1, 9, 100, 200, 0);
    check("...and back with its next word", m.Take(d.data(), d.size(), 20) && m.At(1, 20).shown);
    d = told(1, proto::kNoPanel, 0, 0, 0);
    check("pointing at no panel is no mark", m.Take(d.data(), d.size(), 20.1) && !m.At(1, 20.1).shown);
    d = told(2, 3, 5, 6, 0);
    Marks two;
    check("each person has their own", two.Take(d.data(), d.size(), 1) && (d = told(0, 4, 7, 8, 0), two.Take(d.data(), d.size(), 1)) && two.At(2, 1).panel == 3 && two.At(0, 1).panel == 4 &&
                                           !two.At(1, 1).shown);
    two.Forget();
    check("forgotten, nobody points", !two.At(2, 1).shown && !two.At(0, 1).shown);

    Marks bad;
    d = told(proto::kPeople, 3, 5, 6, 0);
    check("a place nobody can have is refused", !bad.Take(d.data(), d.size(), 1));
    d = told(1, 3, 5, 6, 0);
    d[0] = local::kPoseToWatcher;
    check("another kind of datagram is refused", !bad.Take(d.data(), d.size(), 1));
    d = told(1, 3, 5, 6, 0);
    d.pop_back();
    check("a short one is refused", !bad.Take(d.data(), d.size(), 1));
    d = told(1, 3, 5, 6, 0);
    d.push_back(0);
    check("a long one is refused", !bad.Take(d.data(), d.size(), 1));
    d = told(1, uint8_t(proto::kPeople * proto::kPanelNumbers), 5, 6, 0);
    check("a panel no room can have is refused", !bad.Take(d.data(), d.size(), 1));
    d = told(1, 3, 5, 6, 2);
    check("a flag nobody knows is refused", !bad.Take(d.data(), d.size(), 1));
    const std::vector<uint8_t> one = {local::kPointToWatcher};
    check("its first byte alone is refused", !bad.Take(one.data(), one.size(), 1));
    check("a text is refused", !bad.Take(reinterpret_cast<const uint8_t *>("ok"), 2, 1) && !bad.Take(nullptr, 0, 1));
    check("...and none of them left a mark", !bad.At(1, 1).shown && !bad.At(0, 1).shown);
    check("asked of a place nobody can have, nobody points", !m.At(-1, 20).shown && !m.At(proto::kPeople, 20).shown);
}

static void keys() {
    PanelKeys k;
    check("no panel is known before ft-roompanels says", !k.of(0).known && !k.of(9).known);
    check("an answer is read", k.Read("ok 3 0:1:3440x1440 2:3:1080x1920 9:116:1408x792"));
    check("a screen of this desktop's is ft-screens' panel of that number", k.of(0).known && k.of(0).key == "frametop.screen.1" && k.of(0).width == 3440 && k.of(0).height == 1440 &&
                                                                              k.of(2).key == "frametop.screen.3" && k.of(2).height == 1920);
    check("a peer's panel is the remote screen it is decoded onto", k.of(9).known && k.of(9).key == "frametop.remote.116" && k.of(9).width == 1408);
    check("the others aren't known", !k.of(1).known && !k.of(8).known && !k.of(-1).known && !k.of(kPanels).known);
    check("the next answer stands in place of the last", k.Read("ok 1 9:101:640x1136") && !k.of(0).known && k.of(9).key == "frametop.remote.101" && k.of(9).height == 1136);
    check("no panels is an answer", k.Read("ok 0") && !k.of(9).known);

    k.Read("ok 1 9:101:640x1136");
    const char *bad[] = {"", "ok", "error not in a room", "ok 2 9:101:640x1136", "ok 1 9:101:640x1136 10:102:640x1136", "ok 1 32:101:640x1136", "ok 1 -1:101:640x1136",
                         "ok 1 9:0:640x1136", "ok 1 9:117:640x1136", "ok 1 9:101:640", "ok 1 9:101:640x1136x2", "ok 1 9:101:640x1136:7", "ok 1 9:101:63x1136", "ok 1 9:101:640x8193",
                         "ok 2 9:101:640x1136 9:102:640x1136", "ok 17", "ok -1", "ok 1 nine:101:640x1136", "ok 1 9:101:640x1136junk", "ko 1 9:101:640x1136"};
    bool all = true;
    for (const char *one : bad) {
        const bool refused = !k.Read(one) && k.of(9).known && k.of(9).key == "frametop.remote.101";
        if (!refused) std::printf("      taken: \"%s\"\n", one);
        all = all && refused;
    }
    check("an answer that isn't one is refused whole, and what was known stands", all);
    k.Forget();
    check("forgotten, none is known", !k.of(9).known);
}

// The place's point, and where a step along each of its own directions leads.
static void at(const Place &p, double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = p.m[i][3];
}

static void surface() {
    Surface flat;
    flat.metres = 2, flat.curvature = 0, flat.width = 1920, flat.height = 1080;
    double a[3];
    at(On(flat, 0.5, 0.5, 0), a);
    check("the middle of a flat panel's picture is its middle", near(a[0], 0) && near(a[1], 0) && near(a[2], 0));
    at(On(flat, 0, 0, 0), a);
    check("its top left is half its width to the left and half its height up", near(a[0], -1) && near(a[1], 0.5625) && near(a[2], 0));
    at(On(flat, 1, 1, 0.003), a);
    check("its bottom right is to the right and down, lifted toward the viewer", near(a[0], 1) && near(a[1], -0.5625) && near(a[2], 0.003));
    at(On(flat, -3, 7, 0), a);
    check("a point off the picture is put on its edge", near(a[0], -1) && near(a[1], -0.5625));
    Surface tall = flat;
    tall.width = 1080, tall.height = 1920;
    at(On(tall, 0.5, 0, 0), a);
    check("a tall picture's panel is taller than wide", near(a[1], 2 * 1920.0 / 1080 / 2));
    const Place f = On(flat, 0.25, 0.25, 0);
    check("on a flat panel the mark faces the way the panel does", near(f.m[0][0], 1) && near(f.m[1][1], 1) && near(f.m[2][2], 1) && near(f.m[0][2], 0) && near(f.m[2][0], 0));

    // A quarter of a cylinder: 2 m across, its radius 2 / (pi / 2).
    Surface bent = flat;
    bent.curvature = 0.25;
    const double r = 2 / (2 * M_PI * 0.25);
    at(On(bent, 0.5, 0.5, 0), a);
    check("a bent panel's middle is where the flat one's is", near(a[0], 0) && near(a[1], 0) && near(a[2], 0));
    at(On(bent, 1, 0.5, 0), a);
    check("its right edge is an eighth of a turn round the cylinder, and nearer the viewer", near(a[0], r * std::sin(M_PI / 4)) && near(a[2], r - r * std::cos(M_PI / 4)) && a[2] > 0.3);
    at(On(bent, 0, 0.5, 0), a);
    check("...and its left edge the same on the other side", near(a[0], -r * std::sin(M_PI / 4)) && near(a[2], r - r * std::cos(M_PI / 4)));
    bool onIt = true, lifted = true, square = true;
    for (double x = 0; x <= 1.0001; x += 0.125) {
        at(On(bent, x, 0.3, 0), a);
        onIt = onIt && near(std::hypot(a[0], a[2] - r), r, 1e-9) && near(a[1], 0.2 * 2 * 1080.0 / 1920);
        double b[3];
        at(On(bent, x, 0.3, 0.01), b);
        lifted = lifted && near(std::hypot(b[0], b[2] - r), r - 0.01, 1e-9);   // toward the cylinder's middle line, where the viewer is
        const Place p = On(bent, x, 0.3, 0.01);
        // its own +z points from the surface to the middle line
        const double toward[3] = {-b[0], 0, r - b[2]}, len = std::hypot(toward[0], toward[2]);
        square = square && near(p.m[0][2], toward[0] / len, 1e-9) && near(p.m[2][2], toward[2] / len, 1e-9) && near(p.m[1][1], 1);
    }
    check("every point of a bent panel's picture is on the cylinder", onIt);
    check("...a lifted one is that much nearer its middle line", lifted);
    check("...and the mark there faces the middle line", square);
    Surface whole = flat;
    whole.curvature = 7;
    at(On(whole, 1, 0.5, 0), a);
    check("a bend past a whole cylinder is taken for one", near(a[0], 0, 1e-9) && near(a[2], 2 * (2 / (2 * M_PI)), 1e-9));
    Surface none = flat;
    none.width = 0;
    at(On(none, 0, 0, 0), a);
    check("a picture with no width divides nothing by nothing", std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]));

    // A panel a metre up and two ahead, turned a quarter to the left about the way up.
    const Place panel = {{{0, 0, 1, 0.5}, {0, 1, 0, 1.0}, {-1, 0, 0, -2.0}}};
    const Place world = Then(panel, On(flat, 1, 0.5, 0.1));   // 1 m along the panel's own x, 0.1 along its z
    check("a point on a panel that stands somewhere is told from where it stands", near(world.m[0][3], 0.5 + 0.1) && near(world.m[1][3], 1.0) && near(world.m[2][3], -2.0 - 1));
    check("...and faces the way the panel does", near(world.m[0][2], 1) && near(world.m[2][0], -1) && near(world.m[1][1], 1) && near(world.m[0][0], 0));

    check("a mark is a small part of its panel's width", near(MarkMetres(2), 0.024));
    check("...never too small to see", near(MarkMetres(0.2), 0.012));
    check("...nor so large it hides what is pointed at", near(MarkMetres(20), 0.04));
}

static const uint8_t *pixel(const std::vector<uint8_t> &px, int x, int y) { return &px[(size_t(y) * kDotPixels + size_t(x)) * 4]; }

static void image() {
    const float teal[3] = {0.16f, 0.66f, 0.72f};
    const std::vector<uint8_t> ring = DotImage(teal, false), disc = DotImage(teal, true);
    const int mid = kDotPixels / 2;
    check("the picture is a square of four bytes a pixel", ring.size() == size_t(kDotPixels) * kDotPixels * 4 && disc.size() == ring.size());
    check("its corners are clear", pixel(ring, 0, 0)[3] == 0 && pixel(ring, kDotPixels - 1, 0)[3] == 0 && pixel(disc, 0, kDotPixels - 1)[3] == 0 && pixel(disc, kDotPixels - 1, kDotPixels - 1)[3] == 0);
    check("a pointer's middle is clear: what is pointed at shows through", pixel(ring, mid, mid)[3] == 0);
    check("...and whole while a button is held", pixel(disc, mid, mid)[3] == 255 && pixel(disc, mid, mid)[0] == 41 && pixel(disc, mid, mid)[1] == 168 && pixel(disc, mid, mid)[2] == 184);
    const uint8_t *band = pixel(ring, mid + 16, mid);   // half way out
    check("the band is its person's colour", band[3] == 255 && band[0] == 41 && band[1] == 168 && band[2] == 184);
    const uint8_t *rim = pixel(ring, mid + 25, mid), *edge = pixel(ring, mid + 29, mid);
    check("a light rim is round it", rim[3] == 255 && rim[0] > 230 && rim[1] > 230 && rim[2] > 230);
    check("...and a dark edge round that", edge[3] > 150 && edge[0] < 30 && edge[1] < 30 && edge[2] < 30);
    bool same = true;
    for (int y = 0; y < kDotPixels; ++y)
        for (int x = 0; x < kDotPixels; ++x) {
            const uint8_t *a = pixel(ring, x, y), *b = pixel(ring, kDotPixels - 1 - x, y), *c = pixel(ring, x, kDotPixels - 1 - y), *d = pixel(ring, y, x);
            for (int k = 0; k < 4; ++k) same = same && a[k] == b[k] && a[k] == c[k] && a[k] == d[k];
        }
    check("it is round: the same from every side, and turned a quarter", same);
    const float orange[3] = {0.94f, 0.58f, 0.20f};
    const std::vector<uint8_t> other = DotImage(orange, false);
    const uint8_t *theirs = pixel(other, mid + 16, mid);
    check("someone else's is their colour", theirs[0] == 240 && theirs[1] == 148 && theirs[2] == 51);
    const float wild[3] = {7, -3, 0.5f};
    const std::vector<uint8_t> held = DotImage(wild, true);
    const uint8_t *w = pixel(held, mid, mid);
    check("a colour out of range is held in it", w[0] == 255 && w[1] == 0 && w[2] == 128);
}

int main() {
    marks();
    keys();
    surface();
    image();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
