// The layout's carry on OpenVR: Move All Windows in a panel's grab bar menu (user, 2026-11-14: "when you
// grab it, it grabs your entire Frametop layout and moves it ... interactable with the laser pinch
// underneath a display, and not just with a key binding": carry_toggle's carry-all, the 3D mouse's, was
// the only way to move the room's panels together; 2026-11-24: "an option to drag all windows which lets
// you drag and move your whole set of tiled or free floating windows at the same time"). Held by a laser
// on any panel, it moves every panel in the room (shown or not) and everything on the dashboard, the
// tiles as one, each in its place in a level frame at that panel (layoutgrab.h): the set turns about you,
// rises and comes nearer with the laser in one piece, and nothing in it tilts. The front slot's home and
// a stopped remote display's place are in it too. What's pinned to the head or a wrist stays on its pin,
// and one held otherwise meanwhile stays where that has it. The panels it moves wear carry-all's fuchsia
// border (UpdateCarryGlow). The held panel arms no wrist pin, docks nowhere and isn't dropped on the
// tiles' grid. It ends when the row is let go. The tiles stay where they're put while you're there
// (tilekeep.h). The policy is layoutgrab.h's; the panels are vrpanel.h's, the tiles vrtiles.cpp's.

#include "vrpanel.h"
#include "layoutgrab.h"

#include <cstdio>

namespace ftvr {

namespace {
layoutgrab::Grab<Mat> g_layoutGrab;
Mat g_layoutFrom = Identity();   // where the held panel was when it started

// What rides on the dashboard, in the carry as one: no panel has this number.
constexpr int kBoard = -1;

// Everything on the dashboard moves with it: not a panel another laser has, nor what's glued to one.
bool OnBoard(const Screen &s) { return s.pinned == kDashboard && s.drag == Drag::None && s.glueTo < 0; }

void LayoutGrabEnd() {
    const int panel = g_layoutGrab.panel;
    const auto it = g_screens.find(panel);
    Mat p, head;
    const bool moved = it == g_screens.end() || !ScreenPose(it->second, &p) || Apart(p, g_layoutFrom) > 0.02;
    const bool board = g_layoutGrab.Moves(kBoard);
    bool screens = false;
    for (const auto &[i, s] : g_screens) screens |= s.Desktop() && LayoutMoves(i);
    if (screens) ArrangeDesktopSoon();   // (KWin's outputs follow where the screens are)
    g_layoutGrab.End();
    if (board) TileGroupPut(moved && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? &head : nullptr);
    std::printf("screen %d: move all let go: the windows stay there\n", panel + 1);
}
}  // namespace

bool LayoutCarrying() { return g_layoutGrab.on(); }
bool LayoutMoves(int i) {
    const auto it = g_screens.find(i);
    return g_layoutGrab.Moves(i) || (it != g_screens.end() && g_layoutGrab.Moves(kBoard) && OnBoard(it->second));
}

void LayoutGrabStart(Screen &s, int index, vr::TrackedDeviceIndex_t dev) {
    layoutgrab::Grab<Mat> &g = g_layoutGrab;
    if (g.on()) return;
    for (const auto &[i, o] : g_screens)
        if (o.toggleCarry) return;   // (the 3D mouse's carry has the room)
    Mat head, p;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;
    StartDrag(s, Drag::Move, dev);
    if (s.drag != Drag::Move || !ScreenPose(s, &p)) return;
    g.Begin(index, p, head);
    g_layoutFrom = p;
    size_t tiles = 0;
    for (auto &[i, o] : g_screens) {
        if (OnBoard(o)) ++tiles;
        if (o.pinned != kNone || o.glueTo >= 0 || !ScreenPose(o, &p)) continue;
        g.Take(i, p);
        SpinForget(i);   // (turning: it goes on from where this leaves it)
    }
    Mat group;
    if (TileGroup(&group)) g.Take(kBoard, group);
    s.layoutHeld = true;
    ApplyAlpha(s);
    std::printf("screen %d: move all: %zu other panel(s) in the room and %zu on the dashboard go with it\n", index + 1,
                g.rel.size() - 1 - size_t(g.Moves(kBoard)), g.Moves(kBoard) ? tiles : 0);
}

void UpdateLayoutGrab() {
    layoutgrab::Grab<Mat> &g = g_layoutGrab;
    if (!g.on()) return;
    const auto c = g_screens.find(g.panel);
    if (c == g_screens.end() || !c->second.layoutHeld) return LayoutGrabEnd();
    Mat dev, head;
    if (!DevicePose(c->second.dragDevice, &dev) || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;   // (they wait)
    const Mat step = g.Move(Mul(dev, c->second.dragRel), head);
    for (auto it = g.rel.begin(); it != g.rel.end();) {
        const int i = it->first;
        const auto sit = g_screens.find(i);
        if (i == kBoard) {
            TileGroupTo(g.Pose(i));
        } else if (sit == g_screens.end() || sit->second.pinned != kNone) {
            it = g.rel.erase(it);
            continue;
        } else if (i == g.panel) {
            SetAbsolute(sit->second, g.Pose(i));
        } else if (HeldElsewhere(sit->second, i)) {
            if (Mat p; ScreenPose(sit->second, &p)) g.Take(i, p);
        } else if (StepOnItsWay(sit->second, i, step)) {
            g.Take(i, sit->second.pose);
        } else {
            SetAbsolute(sit->second, g.Pose(i));
        }
        ++it;
    }
    StepPlaces(step);
}

// The room was shifted under it (ShiftRoom): what goes with it goes on from where that put it.
void LayoutShifted() {
    for (const auto &[i, s] : g_screens)
        if (g_layoutGrab.Moves(i) && s.drag == Drag::None) g_layoutGrab.Take(i, s.pose);
}

}  // namespace ftvr
