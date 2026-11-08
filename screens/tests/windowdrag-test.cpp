// Window drag's rules (screens/windowdrag.h): the setting's words, which presses carry a window,
// and which remote panels a window panel lands on.
#include "../windowdrag.h"

#include <cstdio>

using namespace windowdrag;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

int main() {
    Config c;
    check("default: Super", c.mod == Meta);
    check("default: left moves, right resizes, middle nothing", c.left == Move && c.right == Resize && c.middle == Nothing);
    check("Super+left moves", Moves(c, BTN_LEFT, true, false));
    check("left alone doesn't", !Moves(c, BTN_LEFT, false, false));
    check("Alt+left doesn't, with Super the modifier", !Moves(c, BTN_LEFT, false, true));
    check("Super+right resizes, not moves", !Moves(c, BTN_RIGHT, true, false) && ActionOf(c, BTN_RIGHT) == Resize);
    check("Super+middle: nothing", ActionOf(c, BTN_MIDDLE) == Nothing && !Moves(c, BTN_MIDDLE, true, false));
    check("a side button: nothing", ActionOf(c, BTN_SIDE) == Nothing);

    check("windowdrag alt move none move", ParseCommand("alt", "move", "none", "move", &c));
    check("...Alt now", c.mod == Alt && Held(c, false, true) && !Held(c, true, false));
    check("...middle moves too", Moves(c, BTN_MIDDLE, false, true) && Moves(c, BTN_LEFT, false, true));
    check("super is meta", ParseCommand("super", "move", "resize", "none", &c) && c.mod == Meta);
    check("off: no press carries", ParseCommand("off", "move", "resize", "none", &c) && !Moves(c, BTN_LEFT, true, true));
    const Config before = c;
    check("a bad word changes nothing", !ParseCommand("meta", "move", "throw", "none", &c));
    check("...not even the modifier", c.mod == before.mod);

    check("same PC, other port", SameHost("192.168.1.61:48989", "192.168.1.61"));
    check("same PC, same port", SameHost("192.168.1.61", "192.168.1.61"));
    check("other PC", !SameHost("192.168.1.61:48989", "192.168.1.6"));
    check("a prefix isn't the same", !SameHost("192.168.1.6", "192.168.1.61"));
    check("IPv6 whole", SameHost("fd7a::1", "fd7a::1") && !SameHost("fd7a::1", "fd7a::2"));
    check("empty isn't", !SameHost("", ""));

    check("window onto its PC's display", CanDropOn("192.168.1.61:48989", "window:0x55aa10", "192.168.1.61", "display:DP-2"));
    check("not onto another PC's", !CanDropOn("192.168.1.61:48989", "window:0x55aa10", "192.168.1.20", "display:DP-2"));
    check("not onto another window", !CanDropOn("192.168.1.61:48989", "window:0x55aa10", "192.168.1.61:48989", "window:0x1"));
    check("a display isn't dropped", !CanDropOn("192.168.1.61", "display:DP-2", "192.168.1.61", "display:HDMI-A-1"));
    check("not onto a Desktop app stream", !CanDropOn("192.168.1.61:48989", "window:0x55aa10", "192.168.1.61", "Desktop"));
    return failed ? 1 : 0;
}
