// Which ft_vr_command (vr.cpp) a "@ft_screens" command is, and its arguments: the
// command chain's conditions, in its order, on one Cmd. The order matters (the first
// match wins: "curve 1 on" before "curve 1 1.5", "spindistance" before "spin ", a
// "steamvrmode" with too few numbers is no "steamvr " list), and so do the partial
// scans a failed condition leaves behind, which later branches can read: they land
// in the same Cmd fields as before. Header only, no OpenVR, so its test runs anywhere
// (screens/tests/vrcmd-test.cpp). The commands are listed above ft_vr_command.
#pragma once

#include <cstdio>
#include <cstring>

namespace vrcmd {

enum Verb {
    Unknown,
    Place, Width, CurveOnOff, Curve, Pin, Unpin, Get, Screens, Universe, Head,
    Visibility, Wrist, Gesture, GamepadPill, Concealed, ConcealReveal, HideShowToggle,
    Pause, InGames, SteamvrMode, Controllers, HandSetting, SpinDistance, Hands,
    SteamvrList, Typing, CalPanel, Ray, HandInput, PalmMenu, GrabThat, Cutouts, Wallpaper,
    Float, Hold, Unfloat, Pose, Sub, Minimized, Moving, Torn, Carry, Up, Spin,
    Front, Debug, State,  // keep State last: kLastVerb, and the test walks Place..kLastVerb
};
constexpr Verb kLastVerb = State;

// Every argument any branch scans, named as in ft_vr_command.
struct Cmd {
    int n = 0;
    double x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0, w = 0;
    char word[16] = {}, hand[16] = {};
    float r[12] = {};
    int got = 0;                          // pin: how many it scanned (2, or 14 with a pose)
    unsigned pad = 0, app = 0, mine = 0;  // steamvrmode
    char key[48] = {};                    // handsetting
    double m = 0;                         // spindistance
    char sl[16] = {}, sr[16] = {};        // hands
    int tl = 0, tr = 0;                   // typing
    int calOn = 0;                        // calpanel
    int rl = 0, rr = 0;                   // ray
    int x0 = 0, y0 = 0, w0 = 0, h0 = 0, t0 = 0;  // float (sub reuses x0..h0)
    int ms = 0;                           // hold
    int k0 = 0;                           // sub
    int on = 0;                           // minimized
    int gx = 0, gy = 0;                   // torn
};

// Which of ft_vr_command's handlers (vr.cpp) runs a verb: one per group of commands. A verb's case
// must be in the handler of the group named here, or it answers "error unknown command".
enum Group { NoGroup, PanelGroup, ReadoutGroup, VisibilityGroup, HelperWordGroup, HandFeatureGroup, CutoutsGroup, FloatGroup, SpinGroup };

inline Group GroupOf(Verb v) {
    switch (v) {
    case Place: case Width: case CurveOnOff: case Curve: case Pin: case Unpin: case Get: case Pose: case Front:
        return PanelGroup;
    case Screens: case Universe: case Head: case GamepadPill: case Concealed: case Debug: case State:
        return ReadoutGroup;
    case Visibility: case Wrist: case Gesture: case ConcealReveal: case HideShowToggle: case Pause: case InGames:
    case SteamvrMode: case Controllers:
        return VisibilityGroup;
    case HandSetting: case SpinDistance: case Hands: case SteamvrList: case Typing: case CalPanel: case Ray:
        return HelperWordGroup;
    case HandInput: case PalmMenu: case GrabThat:
        return HandFeatureGroup;
    case Cutouts: case Wallpaper:
        return CutoutsGroup;
    case Float: case Hold: case Unfloat: case Sub: case Minimized: case Moving: case Torn: case Carry: case Up:
        return FloatGroup;
    case Spin:
        return SpinGroup;
    case Unknown:
        break;
    }
    return NoGroup;
}

inline Verb Parse(const char *cmd, Cmd &c) {
    float *r = c.r;
    if (std::sscanf(cmd, "place %d %lf %lf %lf %lf %lf %lf", &c.n, &c.x, &c.y, &c.z, &c.yaw, &c.pitch, &c.roll) == 7)
        return Place;
    if (std::sscanf(cmd, "width %d %lf", &c.n, &c.w) == 2) return Width;
    if (std::sscanf(cmd, "curve %d %7s", &c.n, c.word) == 2 && (!std::strcmp(c.word, "on") || !std::strcmp(c.word, "off")))
        return CurveOnOff;
    if (std::sscanf(cmd, "curve %d %lf", &c.n, &c.w) == 2) return Curve;
    c.got = std::sscanf(cmd, "pin %15s %15s %f %f %f %f %f %f %f %f %f %f %f %f", c.word, c.hand, &r[0], &r[1], &r[2],
                        &r[3], &r[4], &r[5], &r[6], &r[7], &r[8], &r[9], &r[10], &r[11]);
    if (c.got >= 2) return Pin;
    if (std::sscanf(cmd, "unpin %15s", c.word) == 1) return Unpin;
    if (std::sscanf(cmd, "get %d", &c.n) == 1) return Get;
    if (std::strncmp(cmd, "screens", 7) == 0) return Screens;
    if (std::strcmp(cmd, "universe") == 0) return Universe;
    if (std::strncmp(cmd, "head", 4) == 0) return Head;
    if (std::sscanf(cmd, "visibility %15s", c.word) == 1) return Visibility;
    if (std::sscanf(cmd, "wrist %lf", &c.w) == 1) return Wrist;
    if (std::sscanf(cmd, "gesture %15s %lf", c.hand, &c.w) == 2) return Gesture;
    if (std::strcmp(cmd, "gamepadpill") == 0) return GamepadPill;
    if (std::strncmp(cmd, "concealed", 9) == 0) return Concealed;
    if (std::sscanf(cmd, "conceal %15s", c.word) == 1 || std::sscanf(cmd, "reveal %15s", c.word) == 1) return ConcealReveal;
    if (!std::strncmp(cmd, "hide", 4) || !std::strncmp(cmd, "show", 4) || !std::strncmp(cmd, "toggle", 6))
        return HideShowToggle;
    if (std::sscanf(cmd, "pause %15s", c.word) == 1) return Pause;
    if (std::sscanf(cmd, "ingames %15s", c.word) == 1) return InGames;
    if (std::sscanf(cmd, "steamvrmode %u %u %u", &c.pad, &c.app, &c.mine) == 3) return SteamvrMode;
    if (std::sscanf(cmd, "controllers %15s", c.word) == 1) return Controllers;
    if (!std::strncmp(cmd, "handsetting ", 12) && std::sscanf(cmd, "handsetting %47s %15s", c.key, c.word) == 2)
        return HandSetting;
    if (std::sscanf(cmd, "spindistance %lf", &c.m) == 1) return SpinDistance;
    if (std::sscanf(cmd, "hands %15s %15s %d", c.sl, c.sr, &c.n) == 3) return Hands;
    if (!std::strncmp(cmd, "steamvr ", 8)) return SteamvrList;
    if (std::sscanf(cmd, "typing %d %d", &c.tl, &c.tr) == 2) return Typing;
    if (std::sscanf(cmd, "calpanel %d", &c.calOn) == 1) return CalPanel;
    if (std::sscanf(cmd, "ray %d %d", &c.rl, &c.rr) == 2) return Ray;
    if (std::sscanf(cmd, "handinput %15s", c.word) == 1) return HandInput;
    if (std::sscanf(cmd, "palmmenu %15s", c.word) == 1) return PalmMenu;
    if (std::sscanf(cmd, "grabthat %15s", c.word) == 1) return GrabThat;
    if (std::sscanf(cmd, "cutouts %15s", c.word) == 1) return Cutouts;
    if (std::sscanf(cmd, "wallpaper %15s", c.word) == 1) return Wallpaper;
    if (std::sscanf(cmd, "float %d %lf %d %d %d %d %d", &c.n, &c.w, &c.x0, &c.y0, &c.w0, &c.h0, &c.t0) == 7) return Float;
    if (std::sscanf(cmd, "hold %d %d", &c.n, &c.ms) == 2) return Hold;
    if (std::sscanf(cmd, "unfloat %d", &c.n) == 1) return Unfloat;
    if (std::sscanf(cmd, "pose %d %f %f %f %f %f %f %f %f %f %f %f %f", &c.n, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5],
                    &r[6], &r[7], &r[8], &r[9], &r[10], &r[11]) == 13)
        return Pose;
    if (std::sscanf(cmd, "sub %d %d %d %d %d %d", &c.n, &c.k0, &c.x0, &c.y0, &c.w0, &c.h0) == 6 ||
        (std::sscanf(cmd, "sub %d %d %15s", &c.n, &c.k0, c.word) == 3 && !std::strcmp(c.word, "off")))
        return Sub;
    if (std::sscanf(cmd, "minimized %d %d", &c.n, &c.on) == 2) return Minimized;
    if (!std::strncmp(cmd, "moving ", 7)) return Moving;
    if (std::sscanf(cmd, "torn %d %d %d", &c.n, &c.gx, &c.gy) == 3) return Torn;
    if (std::sscanf(cmd, "carry %d", &c.n) == 1) return Carry;
    if (std::strcmp(cmd, "up") == 0) return Up;
    if (!std::strncmp(cmd, "spin ", 5)) return Spin;
    if (std::sscanf(cmd, "front %d", &c.n) == 1) return Front;
    if (std::strcmp(cmd, "debug") == 0) return Debug;
    if (std::strncmp(cmd, "state", 5) == 0) return State;
    return Unknown;
}

}  // namespace vrcmd
