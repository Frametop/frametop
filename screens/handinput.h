// Near hand input on Frametop's own panels (frame-hands/notes/hand-interaction.md, section 3):
// poke a panel with the index finger to click it, pinch its edges, bar, title bar or corners
// to move or resize it, hold it with both hands to stretch and turn it. The pointer helper
// does everything farther away (the hand ray); the two meet through the near claim: for each
// hands frame, ft-screens tells the helper which hands are near a panel ("near <capture_ns>
// <L> <R>" to @ft_pointer_helper), and the helper ignores a pinch that began on a near hand.
// The helper tells ft-screens which hands are bare and whether hand input is allowed now
// ("hands <L> <R> <allowed>" on @ft_screens; vr.cpp).
//
// Pure logic: no OpenVR or GL calls, so tests (screens/tests/handinput_test.cpp) run it
// against synthetic hands and recorded ones (ft-handreplay --publish-dump). vr.cpp builds the
// panel list each tick, calls Update, and applies the events and carries.
#pragma once

#include "handcut.h"

#include "../hands/include/fh_smooth.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace handinput {

using Mat = handcut::Mat;

// A panel's controls (vr.cpp's ControlOffsets), in the order of Screen::Controls().
enum class Control { Bar, Curve, Roll, Tab, Dock, Close, Reset, None };

// A control on (or by) the panel's surface: its centre u (right) and v (up), metres from
// the panel's centre, and its half size.
struct Spot {
    Control kind;
    double u, v, hw, hh;
};

// A floating window's popup over it: centre and size in metres (u, v as Spot), and its
// rectangle in the buffer.
struct Popup {
    double u, v, w, h;
    int x, y, pw, ph;
};

struct Panel {
    int index = -1;           // vr.cpp's screen index
    Mat pose{};               // its centre in the room now: +x right, +y up, +z out of its front
    double width = 1, height = 1, curve = 0;   // metres; curve: the cylinder's radius (0 flat)
    int pxW = 0, pxH = 0;     // the client buffer (mouse coordinates)
    bool floating = false;    // a floating window: the panel shows the crop of the buffer
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0, titleH = 0;
    bool glows = false;       // its cutout buffer can show glows (screens, not floating windows)
    bool held = false;        // a laser is carrying it: hands don't grab it
    std::vector<Spot> controls;
    std::vector<Popup> popups;
};

struct Params {
    // the near claim (3.4): a fingertip or the pinch point over the panel (plus this margin)
    // and this far in front of it (or this little behind). Within it the hand touches: its ray
    // hides and takes no pinches (the pointer helper), the index pokes. 0.30 since 2026-10-10
    // (user: "near = poke within a foot"; 0.15 before), HANDS_NEAR_REACH.
    double nearFront = 0.30, nearBack = -0.03, nearMargin = 0.02;
    double nearLeave = 0.05;    // ...a hand near a panel stays near it until this much farther out,
    double leaveMs = 250;       // ...and leaves the claim this long after it's out of the zone
    // A hand resting low by the body (restBelow, restNear: typing, the lap) is near only this
    // close: its pinches stay the helper's (gaze pinches in the lap) under a low panel within reach.
    double restFront = 0.15;
    double cursor = 0.05;       // the KWin pointer follows the fingertip this close (3.5)
    // the poke (3.5)
    double extended = 1.6;      // index tip-to-wrist over knuckle-to-wrist: the finger is straight
    double nearest = 0.005;     // the index is this much nearer the surface than the other fingertips,
    double pointing = 0.2;      // ...or the hand points: the index straighter than each other finger by
                                // this (as `extended`). A pointing hand coming at a panel from below at
                                // an angle has its loosely curled fingertips 1-3 cm nearer the surface
                                // than the index tip (the hand recorder's touch take: index 1.9, the
                                // others 1.3-1.65)
    int pressFrames = 2;        // processed frames in a row at d <= 0
    // ...coming at the panel's face (user, 2026-10-08: "approach from front"): moving into it at
    // least this fast over the last approachMs, the finger (its last bone) within fingerAngle of
    // the panel's normal, and not a hand resting low by the body (its palm more than restBelow
    // under the eyes and within restNear of the head, across the floor): typing under a low
    // panel doesn't poke it, a hand reaching out to a low panel does
    double approach = 0.04;     // m/s
    double approachMs = 100;
    double fingerAngle = 50;    // degrees
    // Typing in the hand recorder's desk take (head poses known): palms 0.34-0.40 m under the eyes,
    // 0.23-0.28 m out; a hand on the mouse 0.40 m out. So a little under the decision's "e.g. 0.35,
    // 0.3", with room to leave: resting from restBelow and restNear, until restLeave closer to
    // the eyes' height or further out.
    double restBelow = 0.30, restNear = 0.32, restLeave = 0.05;
    double release = 0.015;     // up at d >= this
    double pullBack = 0.01;     // ...or pulled back this far from the deepest point
    double slide = 0.015;       // the click stays where it was pressed until the tip slides this far
    double rePressMs = 150;     // no new press this soon after a release
    double tooDeep = -0.08;     // through the panel: released where it was
    double lostMs = 250;        // a hand unseen this long ends what it holds
    // grab zones (3.6), for the pinch point
    double barReach = 0.04, zoneDepth = 0.03, cornerReach = 0.03;
    double edgeOut = 0.025, edgeIn = 0.015;
    double glowReach = 0.05;    // the pinch point's glow shows this close to a grab zone
    double stretchDead = 0.01;  // two hands: their distance changes this much before the panel scales
    double minWidth = 0.15, maxWidth = 12;
    // Smoothing what a grab carries (hands/include/fh_smooth.h: One Euro, in continuous time, read
    // each tick): the pinch point as the cameras saw it (not moved ahead, whose speed is noise
    // times the lead), from minCutoff (Hz) still, beta (Hz per m/s) more as it moves; the head
    // the carried panel faces, likewise (a nod or a lean turned a close panel about the hand).
    double grabMinCutoff = 1.0, grabBeta = 15.0, grabHeadMinCutoff = 0.5, grabHeadBeta = 2.0;
    // A grab moves nothing until the pinch point has moved this far from the start (m): the
    // fingers closing on a pinch move it a little. Then it carries from there (no jump).
    double grabDeadzone = 0.01;
};

enum class Zone { None, Bar, Tab, Corner, Edge, Title };

struct Event {
    enum Type { Motion, Press, Release, Control, GrabStart, GrabEnd } type;
    int side = 0;               // 0 left, 1 right
    int panel = -1;
    double x = 0, y = 0;        // buffer pixels (Motion, Press, Release)
    handinput::Control control = Control::None;   // Control: the button poked
    Zone zone = Zone::None;     // GrabStart
    bool lost = false;          // Release, GrabEnd: the hand was lost, or input was taken away
};

// A panel held by hands, this tick.
struct Carry {
    int panel = -1;
    int sides = 0;              // bit 0: the left hand holds it, bit 1: the right
    enum Kind { Move, Resize, Stretch } kind = Move;
    Mat pose{};                 // Move, Stretch: where it goes now
    double width = -1;          // Stretch, Resize (not floating): its width now (m); -1: as it is
    // Resize: the pinch point on the panel (x right, y up, m from its centre), the corner held
    // (+1 right or top), and the pinch point's offset from that corner at the start
    double hx = 0, hy = 0, grabX = 0, grabY = 0;
    int sx = 1, sy = -1;
};

struct Input {
    int64_t nowNs = 0;
    const handcut::Hands *hands = nullptr;
    Mat head{};
    bool haveHead = false;
    bool allowed[2] = {false, false};   // per side: a bare hand, and hand input allowed now
    bool pointerBusy = false;           // a laser or the mouse holds a button in KWin
};

// Where a point (room) is over a panel: u along it (along the arc when curved) and v up,
// metres from its centre, and d out of its front (negative behind it).
struct Hit {
    double u = 0, v = 0, d = 0;
};
Hit Locate(const Panel &p, const double pt[3]);
Hit Locate(const Panel &p, const float pt[3]);
// The point on the surface at u, v, d (the inverse of Locate), in the room.
void FromSurface(const Panel &p, double u, double v, double d, double out[3]);
// Buffer pixels at u, v on the panel's content (a floating window's crop, or a popup over
// it). False off the content.
bool ToBuffer(const Panel &p, double u, double v, double *x, double *y);
// The control under u, v (within `margin`), or Control::None.
Control ControlAt(const Panel &p, double u, double v, double margin);
// The grab zone a pinch point at h is in (3.6), and for corners which one (sx, sy: +1 right,
// top). excess: how far it is outside that zone (<= 0 inside), for the glow; the zone is the
// nearest one then.
Zone GrabZone(const Panel &p, const Hit &h, const Params &k, int *sx, int *sy, double *excess);
// One hand carrying a panel: the grab point (panel-local, at the start) follows the pinch
// point, and the panel keeps the turn it had from facing the head (its yaw, pitch and roll
// relative to that), so carrying it around you keeps it facing you.
Mat CarryPose(const Mat &start, const double startHead[3], const double grabLocal[3], const double pinch[3],
              const double head[3], const Mat &previous);
// Two hands: the panel held at the start (pose, width) with the hands at l0, r0; now at l, r.
// The width scales with their distance (a dead zone of `dead`, clamped), and the panel turns
// about the vertical with the line between them and moves with their midpoint. floating:
// no scaling.
Mat StretchPose(const Mat &start, double width0, const double l0[3], const double r0[3], const double l[3],
                const double r[3], double dead, double minW, double maxW, bool scale, double *width);

class HandInput {
public:
    explicit HandInput(const Params &p = {}) : p_(p) {}
    Params &params() { return p_; }
    const Params &params() const { return p_; }

    // Once per tick, after the hands were read (Hands::Update). Fills events(), carries(),
    // glows and the near state.
    void Update(const Input &in, const std::vector<Panel> &panels);
    const std::vector<Event> &events() const { return events_; }
    const std::vector<Carry> &carries() const { return carries_; }
    // Per panel index: the glows to draw on it (buffer pixels), the controls a hand is near
    // (bit k: Control k), the panels a hand is near (their controls show).
    const std::map<int, std::vector<handcut::Glow>> &glows() const { return glows_; }
    const std::map<int, unsigned> &hovered() const { return hovered_; }
    const std::set<int> &nearPanels() const { return nearPanels_; }
    // The near claim, per side, for the last hands frame (for the helper's "near").
    bool near(int side) const { return side_[side].state != State::Idle && side_[side].state != State::Off; }
    bool anyNear() const { return near(0) || near(1); }
    // This Update processed a new hands frame, captured at frameNs().
    bool newFrame() const { return newFrame_; }
    int64_t frameNs() const { return frameNs_; }
    // The caller ended this side's press in KWin itself (vr.cpp's ReleaseStuck): forget it.
    void ForgetPress(int side);
    bool pressing(int side) const { return side_[side].state == State::Press; }
    // ...holding KWin's button (not a poked control)
    bool pressingKwin(int side) const { return pressing(side) && side_[side].control == Control::None; }
    bool grabbing(int side) const { return side_[side].state == State::Grab; }
    int grabPanel(int side) const { return side_[side].state == State::Grab ? side_[side].panel : -1; }
    // "left hover 2 d 0.031, right idle"
    std::string Describe() const;

    struct Stats {
        int frames = 0, nearFrames = 0;
        int presses = 0, releases = 0, lostReleases = 0, deepReleases = 0, controls = 0;
        int grabs = 0, grabEnds = 0, stretches = 0;
        // presses that didn't happen, by the guard that stopped them (frames at d <= 0 over content)
        int heldNotStereo = 0, heldNotExtended = 0, heldNotNearest = 0, heldSlow = 0, heldPinching = 0, heldBusy = 0,
            heldAskew = 0, heldResting = 0;
    } stats;

private:
    enum class State { Off, Idle, Hover, Press, Grab };
    struct Side {
        State state = State::Idle;
        int panel = -1;
        uint32_t handId = 0;
        int64_t lastSeen = 0, outSince = -1, releasedAt = -1;
        // the poke
        int below = 0;                       // frames in a row at d <= 0
        bool fastIn = false;                 // ...and coming in fast when it crossed the surface
        bool resting = false;                // the hand rests low by the body (Params::restBelow)
        struct Sample { int64_t t; double d; };
        std::vector<Sample> recent;          // the index tip's d over the last frames (about 250 ms)
        double lockU = 0, lockV = 0, lastX = -1, lastY = -1, deepest = 0;
        bool dragging = false, spent = false;
        Control control = Control::None;     // pressed a control (no KWin press)
        double hoverX = -1, hoverY = -1;
        // the grab
        uint32_t begins = 0;                 // the pinch slot's begins counter last seen
        bool knowBegins = false;
        Zone zone = Zone::None;
        int sx = 1, sy = -1;
        double grabLocal[3] = {0, 0, 0}, startHead[3] = {0, 0, 0};
        Mat start{}, last{};
        double width0 = 1, grabX = 0, grabY = 0;
        // the pinch point, smoothed (Params::grab*), where it began, and the dead zone's offset
        // once it's out (grabDeadzone)
        fhsmooth::Point3 point;
        fhsmooth::V3 from, shift;
        bool engaged = false;
    };
    const handcut::Hand *HandFor(int side, const std::vector<handcut::Hand> &hands, uint32_t prefer) const;
    void Poke(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    void Grab(int s, const Input &in, const std::vector<Panel> &panels);
    void Carries(const Input &in, const std::vector<Panel> &panels);
    void Glows(const Input &in, const std::vector<Panel> &panels);
    void EndPress(int s, bool lost);
    void EndGrab(int s, bool lost);
    // A grabbing side's pinch point: smoothed, at the tick's time, within its dead zone (deadzone).
    bool Carried(int s, const Input &in, double out[3], bool deadzone = true);
    void Emit(Event::Type t, int s, int panel, double x = 0, double y = 0, bool lost = false);
    const Panel *Find(const std::vector<Panel> &panels, int index) const;

    Params p_;
    Side side_[2];
    std::vector<Event> events_;
    std::vector<Carry> carries_;
    std::map<int, std::vector<handcut::Glow>> glows_;
    std::map<int, unsigned> hovered_;
    std::set<int> nearPanels_;
    uint64_t frame_ = 0;
    bool newFrame_ = false;
    fhsmooth::Point3 head_;   // the head carried panels face, smoothed
    int64_t frameNs_ = 0;
    // two hands on one panel: what it was when the second hand took it
    struct Stretch {
        int panel = -1;
        Mat start{};
        double width0 = 1, l0[3] = {0, 0, 0}, r0[3] = {0, 0, 0};
    } stretch_;
};

}  // namespace handinput
