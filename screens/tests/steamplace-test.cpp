// Where Steam's popups go (screens/steamplace.h): frames from three points, their inverse, a popup
// moved with its panel, and which panel it sits on.
#include "../steamplace.h"

#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

using namespace steamplace;

static bool Near(V3 a, V3 b, double eps = 1e-6) { return Len(a - b) < eps; }

// A w x h panel `mpp` metres a pixel, centred at `c`, turned `yaw` radians about y.
static Frame Flat(V3 c, double yaw, double w, double h, double mpp) {
    const V3 x = {std::cos(yaw) * mpp, 0, -std::sin(yaw) * mpp}, y = {0, mpp, 0};
    Frame f;
    FromPoints(c, c + x * (w / 2), c + y * (h / 2), w, h, &f);
    return f;
}

int main() {
    const Frame steamMain = Flat({0, 1.2, -2}, 0, 1920, 1080, 0.001);
    {
        check("its centre pixel at its centre", Near(Apply(steamMain, {960, 540, 0}), {0, 1.2, -2}));
        check("its bottom left at the bottom left", Near(Apply(steamMain, {0, 0, 0}), {-0.96, 1.2 - 0.54, -2}));
        check("its normal out of its front, as long as a pixel", Near(steamMain.az, {0, 0, 0.001}));
        Frame inv, id;
        check("an inverse", Inverse(steamMain, &inv));
        id = Mul(steamMain, inv);
        check("times it: none", Near(id.o, {}) && Near(id.ax, {1, 0, 0}) && Near(id.ay, {0, 1, 0}) && Near(id.az, {0, 0, 1}));
        Frame f;
        check("no panel from points on a line", !FromPoints({0, 0, 0}, {1, 0, 0}, {2, 0, 0}, 10, 10, &f));
        check("nor from a 0 size", !FromPoints({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0, 10, &f));
    }
    {   // the footer under Steam's panel: moved with it to our tile, turned and larger
        const Frame footer = Flat({0, 1.2 - 0.54 - 0.04, -1.99}, 0, 900, 60, 0.001);
        const Frame ours = Flat({1, 1.5, -1}, -M_PI / 2, 1920, 1080, 0.0015);
        Frame moved;
        check("remapped", Remap(ours, steamMain, footer, &moved));
        // its centre: 0.58 m under Steam's panel's centre and 1 cm in front, so 0.87 m under ours, 1.5 cm in front (its front: -x)
        check("under our tile as it was under Steam's", Near(Apply(moved, {450, 30, 0}), {1 - 0.015, 1.5 - 0.87, -1}, 1e-6));
        check("as large again as ours is", std::fabs(Len(moved.ax) - 0.0015) < 1e-9);
        check("facing as ours does", Near(moved.az * (1 / Len(moved.az)), {-1, 0, 0}));
        Frame same;
        Remap(steamMain, steamMain, footer, &same);
        check("on Steam's own panel: where Steam has it", Near(Apply(same, {0, 0, 0}), Apply(footer, {0, 0, 0})));
        float m[3][4];
        ToOverlay(moved, 900, 60, m);
        check("its overlay centred on it", std::fabs(m[0][3] - 0.985) < 1e-5 && std::fabs(m[1][3] - (1.5 - 0.87)) < 1e-5);
        check("1 m of overlay = its width", std::fabs(std::hypot(m[0][0], m[2][0]) - 900 * 0.0015) < 1e-5);
    }
    {   // which panel it sits on
        const Frame menu = Flat({-1.3, 1.2, -1.9}, 0.3, 450, 1200, 0.001);
        const std::vector<steamplace::Panel> panels = {{steamMain, 1920, 1080}, {menu, 450, 1200}};
        check("on the main one", SitsOn({0.2, 1.0, -1.99}, panels) == 0);
        check("just under it: still it", SitsOn({0, 1.2 - 0.54 - 0.05, -1.99}, panels) == 0);
        check("on the menu", SitsOn(Apply(menu, {225, 100, 5}), panels) == 1);
        check("far from both: none", SitsOn({0, 3, -2}, panels) == -1);
        check("behind one: none", SitsOn({0, 1.2, -2.5}, panels) == -1);
    }
    return failed ? 1 : 0;
}
