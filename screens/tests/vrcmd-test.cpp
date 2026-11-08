// ft_vr_command's parser (screens/vrcmd.h): which command a line is, its arguments, and
// the order the first match wins in, as ft-screens has always read them; and which handler
// group runs each.
#include "../vrcmd.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace vrcmd;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static Verb V(const char *cmd) {
    Cmd c;
    return Parse(cmd, c);
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    Cmd c;
    // Every verb, from a line as its sender writes it.
    struct { const char *cmd; Verb verb; } lines[] = {
        {"place 1 0.5 1.2 -2 10 -5 0", Place}, {"width 2 1.6", Width}, {"curve 1 on", CurveOnOff},
        {"curve 1 off", CurveOnOff}, {"curve 1 2.5", Curve}, {"pin all head", Pin}, {"unpin 3", Unpin},
        {"get 1", Get}, {"screens", Screens}, {"universe", Universe}, {"head", Head},
        {"visibility auto", Visibility}, {"wrist 0.8", Wrist}, {"gesture left 0.5", Gesture},
        {"gamepadpill", GamepadPill}, {"concealed", Concealed}, {"conceal 2", ConcealReveal},
        {"reveal all", ConcealReveal}, {"hide", HideShowToggle}, {"show", HideShowToggle},
        {"toggle", HideShowToggle}, {"pause on", Pause}, {"ingames hide", InGames},
        {"steamvrmode 1 0 1", SteamvrMode}, {"controllers on", Controllers},
        {"handsetting HANDS_PALM_MENU 1", HandSetting}, {"spindistance 1.2", SpinDistance},
        {"hands hand controller 1", Hands}, {"steamvr 12 dashboard", SteamvrList}, {"typing 1 0", Typing},
        {"calpanel 1", CalPanel}, {"ray 0 1", Ray}, {"handinput force", HandInput}, {"palmmenu state", PalmMenu},
        {"grabthat off", GrabThat}, {"cutouts matte mesh", Cutouts}, {"wallpaper on", Wallpaper}, {"float 5 0.0008 0 0 1280 800 30", Float},
        {"hold 5 250", Hold}, {"unfloat 5", Unfloat}, {"pose 5 1 0 0 0 0 1 0 1.6 0 0 1 -1", Pose},
        {"sub 5 1 10 20 300 200", Sub}, {"sub 5 1 off", Sub}, {"minimized 5 1", Minimized},
        {"moving 5 1", Moving}, {"torn 5 100 200", Torn}, {"carry toggle", CarryToggle}, {"carry take 5", CarryTake},
        {"carryturn 1.5 -2", CarryTurn}, {"carry 5", Carry}, {"up", Up},
        {"windowdrag meta move resize none", WindowDrag}, {"pulled 103 0.5 0.02 7", Pulled}, {"spin 30", Spin},
        {"front 2", Front}, {"debug", Debug}, {"state", State},
    };
    bool all = true;
    for (auto &l : lines)
        if (V(l.cmd) != l.verb) all = false, std::printf("      %s -> %d, not %d\n", l.cmd, V(l.cmd), l.verb);
    check("every verb, from a line as its sender writes it", all);
    check("unknown and empty lines are Unknown", V("frobnicate 1") == Unknown && V("") == Unknown);

    // Arguments land in their fields.
    check("place: screen and pose",
          Parse("place 2 0.5 1.2 -2 10 -5 3", c) == Place && c.n == 2 && near(c.x, 0.5) && near(c.y, 1.2) &&
              near(c.z, -2) && near(c.yaw, 10) && near(c.pitch, -5) && near(c.roll, 3));
    c = {};
    { Cmd p; check("pulled: screen, where held and the pull's number", Parse("pulled 103 0.25 0.75 7", p) == Pulled && p.n == 103 &&
          near(p.x, 0.25) && near(p.y, 0.75) && p.seq == 7); }
    { Cmd p; check("pulled without a number: seq 0 (refused)", Parse("pulled 103 0.25 0.75", p) == Pulled && p.seq == 0); }
    check("pin: two words scan 2", Parse("pin 1 left", c) == Pin && c.got == 2 && !std::strcmp(c.word, "1") &&
                                       !std::strcmp(c.hand, "left"));
    c = {};
    check("pin: with a relative pose scans 14",
          Parse("pin all head 1 0 0 0 0 1 0 0 0 0 1 -1.5", c) == Pin && c.got == 14 && near(c.r[11], -1.5));
    c = {};
    check("pose: screen and 12 numbers", Parse("pose 5 1 0 0 0.1 0 1 0 1.6 0 0 1 -1", c) == Pose && c.n == 5 &&
                                             near(c.r[3], 0.1f) && near(c.r[7], 1.6f) && near(c.r[11], -1));
    c = {};
    check("float: its rectangle", Parse("float 6 0.0008 1 2 1280 800 30", c) == Float && c.n == 6 &&
                                      near(c.w, 0.0008) && c.x0 == 1 && c.y0 == 2 && c.w0 == 1280 && c.h0 == 800 &&
                                      c.t0 == 30);
    c = {};
    check("sub off: no rectangle, word off", Parse("sub 6 2 off", c) == Sub && c.n == 6 && c.k0 == 2 &&
                                                 !std::strcmp(c.word, "off") && c.w0 == 0);
    c = {};
    check("hands: both sides and allowed", Parse("hands off hand 0", c) == Hands && !std::strcmp(c.sl, "off") &&
                                               !std::strcmp(c.sr, "hand") && c.n == 0);
    c = {};
    check("handsetting: key and value", Parse("handsetting HANDS_GRAB_THAT 0", c) == HandSetting &&
                                            !std::strcmp(c.key, "HANDS_GRAB_THAT") && !std::strcmp(c.word, "0"));
    c = {};
    check("steamvrmode: three numbers", Parse("steamvrmode 1 2 3", c) == SteamvrMode && c.pad == 1 && c.app == 2 &&
                                            c.mine == 3);

    // The order, and prefixes, as they have always been.
    c = {};
    check("curve 1 1.5: not on/off, a number (word keeps its scan)",
          Parse("curve 1 1.5", c) == Curve && near(c.w, 1.5) && !std::strcmp(c.word, "1.5"));
    check("curve 1 maybe: neither", V("curve 1 maybe") == Unknown);
    check("curve with a long word: %7s stops at 7, so not off", V("curve 1 offoffoff") == Unknown);
    check("pin with one word is no pin", V("pin all") == Unknown);
    check("head is a prefix: headset is head", V("headset") == Head);
    check("screens and state are prefixes", V("screens please") == Screens && V("statex") == State);
    check("concealed before conceal", V("concealed 2") == Concealed);
    check("hide, show, toggle are prefixes", V("hidex") == HideShowToggle && V("toggled") == HideShowToggle);
    check("hold needs its two numbers", V("hold 1 2") == Hold && V("hold") == Unknown);
    check("spindistance is no spin; spin needs its space", V("spindistance x") == Unknown && V("spin") == Unknown);
    check("steamvrmode with too few numbers is no steamvr list", V("steamvrmode 1 2") == Unknown);
    check("handsetting needs both words (and is no \"hands\" either)", V("handsetting KEY") == Unknown);
    check("hands needs three", V("hands hand controller") == Unknown);
    check("handinput is not hands", V("handinput on") == HandInput);
    check("sub with a word other than off is nothing", V("sub 5 1 on") == Unknown);
    check("universe and up are exact", V("universe ") == Unknown && V("up ") == Unknown && V("debug ") == Unknown);
    check("the pointer's pose (6 numbers) is no panel pose", V("pose 1 2 3 4 5") == Unknown);

    // Which handler runs each verb (ft_vr_command's dispatch): every verb has one, Unknown none.
    bool grouped = true;
    for (int v = Place; v <= kLastVerb; ++v)
        if (GroupOf(Verb(v)) == NoGroup) grouped = false, std::printf("      verb %d has no group\n", v);
    check("every verb has a handler group", grouped);
    check("unknown has none", GroupOf(Unknown) == NoGroup);
    check("groups: panels, readouts, visibility",
          GroupOf(Pin) == PanelGroup && GroupOf(Front) == PanelGroup && GroupOf(Get) == PanelGroup &&
              GroupOf(Screens) == ReadoutGroup && GroupOf(State) == ReadoutGroup && GroupOf(Pause) == VisibilityGroup &&
              GroupOf(Controllers) == VisibilityGroup);
    check("groups: helper words, hand features, cutouts, floating, spin",
          GroupOf(Hands) == HelperWordGroup && GroupOf(SpinDistance) == HelperWordGroup &&
              GroupOf(GrabThat) == HandFeatureGroup && GroupOf(Cutouts) == CutoutsGroup && GroupOf(Up) == FloatGroup &&
              GroupOf(Moving) == FloatGroup && GroupOf(Spin) == SpinGroup);
    return failed ? 1 : 0;
}
