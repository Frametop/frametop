// SteamVR's own panels for near hand input (vr.cpp's "SteamVR's panels by hand"): the dashboard, the
// Steam menu, Steam's keyboard, other apps' overlays. OpenVR won't give out where a dashboard panel
// is, nor its size (most report a 0x0 texture), but ComputeOverlayIntersection finds a ray's hit on
// it and that hit's texture coordinates. So for each hands frame vr.cpp casts a small cone of rays
// from the eyes past each hand's index tip, and FitPanel fits the panel to the hits (as
// pointer/common/vrmath.h's ScanPanel does with a whole sphere of them). Near input then treats it as
// one of its panels (handinput::Panel::foreign), and the pointer helper's laser does what the hand
// does there: Touch puts it a little in front of the point, aimed into it, to hover, click and drag;
// GrabPoint gives where a move or a resize presses (the grab bar under a floating panel, the resize
// margin just outside a corner), and Follow keeps the laser where it was on a panel a hand carries.
//
// Pure math: no OpenVR calls (the tests run it, screens/tests/handinput_test.cpp).
#pragma once

#include "handcut.h"

#include <array>
#include <vector>

namespace steampanels {

using Mat = handcut::Mat;

// A ray's hit on a panel: its texture coordinates (u right, v up, 0..1) and the point in the room.
struct Sample {
    double u = 0, v = 0;
    double p[3] = {0, 0, 0};
};

// A panel fitted to hits: its centre and frame (x right, y up, z out of its front) and size, metres.
struct Fit {
    bool ok = false;
    Mat pose{};
    double width = 0, height = 0;
    int hits = 0;
};

// Least squares p = O + u U + v V over the samples; at least minHits, spread over the panel both ways,
// and a size within 3 cm..20 m.
Fit FitPanel(const std::vector<Sample> &samples, int minHits = 8);

// n x n ray directions over a square cone halfDeg each way round the line from `from` to `at`.
std::vector<std::array<double, 3>> Cone(const double from[3], const double at[3], double halfDeg, int n);

// Near input's buffer pixels for a SteamVR panel (handinput::Panel::pxW, pxH), and back to the point
// on it (u right, v up from its centre, metres).
constexpr double kPxPerM = 1000;
void FromPixels(double width, double height, double x, double y, double *u, double *v);

// The laser touching the point (u, v) on a panel: its origin `back` out in front of it, pointing in
// (a device's -z), turned as the panel is (so a carried panel turns with it).
Mat Touch(const Mat &panel, double u, double v, double back);

// Where a hand's move or resize presses the laser, on the panel's plane (u, v from its centre):
// a move, the grab bar barBelow under the bottom edge's middle; a resize, `margin` out past corner
// (sx, sy) both ways (SteamVR's resize margins sit just outside a floating panel).
void GrabPoint(double width, double height, bool resize, int sx, int sy, double barBelow, double margin, double *u,
               double *v);

// The laser that took a panel at `from` and holds it, with the panel now at `to`: to from^-1 laser
// (the dashboard carries a grabbed panel rigidly with the device).
Mat Follow(const Mat &from, const Mat &to, const Mat &laser);

// The pose's rotation as a quaternion (w, x, y, z), for the pointer helper's "touch" lines.
void Quat(const Mat &m, double q[4]);

}  // namespace steampanels
