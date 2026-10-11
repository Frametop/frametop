// Near hand input on Frametop's own panels (frame-hands/notes/hand-interaction.md, sections 3 and
// 16), within Params::nearFront (about 3 inches): poke a panel with the index finger to click it;
// pinch its border to move it (by a corner: resize it), its content to click; pinch its edge with
// the panel between thumb and index to carry it like a card; make a fist at it to carry it; hold
// it with both hands to stretch and turn it. The pointer helper does everything farther away (the
// hand ray, the gaze); the two meet through the near claim: for each hands frame, ft-screens tells
// the helper which hands are near a panel ("near <capture_ns> <L> <R>" to @ft_pointer_helper: 1
// claims(), 2 near() only), and the helper leaves a pinch or grip that began on a claimed hand.
//
// Gesture -> action, and what wins (2026-10-10):
//   index tip through the surface, from the front     poke: press, release, drag (Poke)
//   pinch, thumb and index either side of the edge     card: rigid carry (Zone::Card)
//   pinch where the fingers meet in the border band    corner/tab: resize; border, bar, title: move
//   pinch on the content, within the zone              click at the cursor, held with the pinch
//   fist (grip) with the palm or knuckles at it        carry and turn with the wrist (Zone::Fist)
//   two grabs (pinches or fists) on one panel          stretch and turn
//   a pinch or fist on a hand "grab that" has          waits: that gesture's, or ours if it lets go
//   pinch or fist beyond the zone (or in its margin)   the pointer helper's (ray, gaze, grip drag)
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
    // the near claim (3.4): the index tip, the thumb-index contact, the knuckles or the palm over the
    // panel (plus this margin, or the grab band's reach outside it if that's wider) and this far in
    // front of it (or this little behind). Within it the hand touches: its ray hides and its pinches
    // and fists are ours (the pointer helper leaves them), the index pokes. Beyond it a pinch is the
    // ray's or the gaze's. 0.075 since the live test of 2026-10-10 (user: "1 foot is way too far, it
    // should be closer to 3 inches"; 0.30 then, 0.15 before), HANDS_NEAR_REACH.
    double nearFront = 0.075, nearBack = -0.05, nearMargin = 0.02;
    double nearLeave = 0.025;   // ...a hand near a panel stays near it until this much farther out,
    double leaveMs = 200;       // ...and leaves the claim this long after it's out of the zone
    // A hand resting low by the body (restBelow, restNear: typing, the lap) is near only this
    // close: its pinches stay the helper's (gaze pinches in the lap) under a low panel within reach.
    double restFront = 0.04;
    double cursor = 0.05;       // the KWin pointer follows the fingertip this close (3.5)
    // ft-hands is asked for its full rate (want-hz) while a hand is this close to a panel, so a poke
    // that comes in from outside the near zone is seen at 30 Hz, not a still hand's 15
    double rateReach = 0.30;
    // the poke (3.5)
    double extended = 1.5;      // index tip-to-wrist over knuckle-to-wrist: the finger is straight
                                // (1.6 until 2026-10-10: a relaxed pointing finger read 1.5-1.6)
    double nearest = 0.005;     // the index is this much nearer the surface than the other fingertips,
    double pointing = 0.2;      // ...or the hand points: the index straighter than each other finger by
                                // this (as `extended`). A pointing hand coming at a panel from below at
                                // an angle has its loosely curled fingertips 1-3 cm nearer the surface
                                // than the index tip (the hand recorder's touch take: index 1.9, the
                                // others 1.3-1.65)
    // At d <= 0 for pressFrames processed frames in a row; or for one frame when it came in
    // decisively: from at least `travel` farther out within travelMs, at quickSpeed or faster. A quick
    // tap at 30 Hz is past the surface for one or two frames, and a tap the tracker has 1 cm short is
    // past it for none: two frames in a row lost most of them (live test, 2026-10-10).
    int pressFrames = 2;
    double travel = 0.025, travelMs = 300, quickSpeed = 0.10;
    // ...within lateFrames of crossing it and no deeper than lateDepth: a finger that has been past
    // the surface longer, or deeper, is reaching through it (a flag replay pressed 9 cm deep, 19
    // frames in, once the finger's angle came right)
    int lateFrames = 5;
    double lateDepth = -0.04;
    // ...coming at the panel's face (user, 2026-10-08: "approach from front"): moving into it at
    // least this fast over the last approachMs, the finger (knuckle to tip: the last bone alone is
    // 2 cm long, and a few mm of noise on its ends turned it 15-20 degrees) within fingerAngle of
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
    double rePressMs = 150;     // no new press this soon after a release,
    double rearm = 0.01;        // ...nor until the tip has been this far out in front since (a noisy
                                // pull-back let go mid-tap and pressed again: a double click)
    double tooDeep = -0.08;     // through the panel: released where it was
    double lostMs = 250;        // a hand unseen this long ends what it holds
    // Pinch grab zones (3.6), tested at the pinch's begin with the thumb-index contact (where the
    // fingers meet; the gestures file's point is the knuckles, 5-8 cm behind it, which a 3 cm deep
    // zone rarely had). A band along the panel's border, bandOut outside it to bandIn inside, within
    // bandDepth of its surface either side: corners (within cornerReach) resize, the rest of it
    // moves; and the bar (barReach) and resize tab. HANDS_GRAB_BAND (bandOut). 2026-10-10: user, "I
    // still can't pinch to grab displays easily" (4 cm, 3 cm, 2.5 out, 1.5 in, 3 deep before).
    double barReach = 0.06, cornerReach = 0.06, bandOut = 0.06, bandIn = 0.025, bandDepth = 0.04;
    double glowReach = 0.05;    // a pinch's glow shows this close to a grab zone
    // A pinch on the content within the near zone (not in a grab zone) is a click where the
    // pointer is (the fingertip's cursor, if it was there in the last pinchCursorMs and is within
    // pinchCursor of the fingers), held while the pinch is, and dragging past `slide`.
    // HANDS_PINCH_CLICK.
    bool pinchClick = true;
    double pinchCursorMs = 500, pinchCursor = 0.04;
    // The card pinch (user, 2026-10-10: "pinch from underneath the display and manipulate it like a
    // card pinched between index finger and thumb"): a pinch whose thumb and index tips were on
    // either side of the panel's plane (each at least cardSide from it, the line between them within
    // cardAngle of its normal) with the hand along the panel (wrist to knuckles at least cardAlong
    // degrees off its normal: from below or the side, not pointing into it) on some frame in the last
    // cardMs before it closed, the fingers within cardBand of the panel's border (inside or out). The
    // panel then goes rigidly with the hand: the knuckles' place and the hand's turn, 1:1, smoothed,
    // past grabDeadzone and cardRotDeadzone degrees. An edge pinch from the front or the side has both
    // tips on one side of the plane, or the hand pointing into it. HANDS_CARD_PINCH.
    bool cardPinch = true;
    double cardSide = 0.003, cardAngle = 60, cardAlong = 45, cardMs = 300, cardBand = 0.05, cardRotDeadzone = 3;
    // The fist grab (user, 2026-10-10: "grab a display and use the fist to expand and rotate it, as
    // well as change its angle"): a grip (ft-hands' closed hand) that begins with the hand near a
    // panel (its palm or knuckles within the near zone, over it) takes it: carried like a pinch on
    // its bar (the knuckles 1:1, the wrist turning it), two on one panel stretch it. HANDS_FIST_GRAB.
    bool fistGrab = true;
    // A fist or a pinch that begins on a hand "grab that" has (held out open, or closing: the thumb
    // passes the index tip as a hand closes, a pinch to ft-hands) waits until that gesture fires (then
    // it's that) or lets the hand go, at most grabThatWaitMs: on the user's flag, with a panel where
    // the gesture brings one, a pinch-click on the closing hand took the hand from it.
    double grabThatWaitMs = 600;
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
    // One hand carrying a panel turns it with the wrist (user, 2026-10-10: yaw, pitch and roll, to
    // line displays up): the hand's turn since the grab, from its frame through the wrist and the
    // index and little knuckles (they hold still while the fingers pinch), taken relative to facing
    // the head across the floor from the hand, and applied relative to the panel facing the head
    // (CarryPose). So carrying a panel around you turns it only to keep facing you, as
    // before, and a turn of the wrist turns it on top of that, 1:1. The two directions of that frame
    // are smoothed (fh_smooth.h's Direction: rotMinCutoff Hz still, rotBeta Hz more per deg/s), and
    // nothing turns until the hand has turned rotDeadzone degrees (the frame's jitter: about 1 deg a
    // frame, up to 6 over a second and a half on a still hand); then it turns from there (no jump).
    // A turned panel snaps level within rollSnap degrees of roll (vr.cpp's kRollSnap). Let go, it
    // stays turned; the next grab keeps that turn. HANDS_GRAB_ROTATE, HANDS_GRAB_ROTATE_DEADZONE.
    bool grabRotate = true;
    double rotDeadzone = 5, rotMinCutoff = 1.0, rotBeta = 0.05, rollSnap = 2.5;
};

// Card: the card pinch; Fist: the fist grab (both carry it).
enum class Zone { None, Bar, Tab, Corner, Edge, Title, Card, Fist };
const char *ZoneName(Zone z);

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
    bool typing[2] = {false, false};    // per side: typing holds this hand back (the helper's "typing"):
                                        // no new poke or grab, and the cursor doesn't follow its fingertip
    bool grabThatArmed[2] = {false, false};   // per side: "grab that" has this hand held out or closing
                                              // (grabthat::Detector::armed): a fist waits (Params::fistGrab)
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
// The grab zone a pinch's contact at h is in (3.6, Params::band*), and for corners which one (sx,
// sy: +1 right, top). excess: how far it is outside that zone (<= 0 inside), for the glow; the
// zone is the nearest one then.
Zone GrabZone(const Panel &p, const Hit &h, const Params &k, int *sx, int *sy, double *excess);
// One hand carrying a panel: the grab point (panel-local, at the start) follows the pinch
// point, and the panel keeps the turn it had from facing the head (its yaw, pitch and roll
// relative to that), so carrying it around you keeps it facing you. turn (optional): the wrist's
// turn since the grab, in the frame facing the head (x right, y up, z toward the head), applied on
// top; rollSnap: then the panel snaps level within this many degrees of roll (0: never).
Mat CarryPose(const Mat &start, const double startHead[3], const double grabLocal[3], const double pinch[3],
              const double head[3], const Mat &previous, const double (*turn)[3] = nullptr, double rollSnap = 0);
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
    // glows, the near state and the log.
    void Update(const Input &in, const std::vector<Panel> &panels);
    const std::vector<Event> &events() const { return events_; }
    // What this Update did and why a poke didn't press, a line each, for ft-screens' log: each
    // press and release (panel, depth), each poke attempt that crossed the surface without a press
    // (the rule that held it back), each grab and its end.
    const std::vector<std::string> &logs() const { return logs_; }
    // "Grab that" fired for this side: a fist or pinch waiting on it (Params::grabThatWaitMs) doesn't
    // grab or click.
    void GrabThatFired(int side);
    const std::vector<Carry> &carries() const { return carries_; }
    // Per panel index: the glows to draw on it (buffer pixels), the controls a hand is near
    // (bit k: Control k), the panels a hand is near (their controls show).
    const std::map<int, std::vector<handcut::Glow>> &glows() const { return glows_; }
    const std::map<int, unsigned> &hovered() const { return hovered_; }
    const std::set<int> &nearPanels() const { return nearPanels_; }
    // The near claim, per side, for the last hands frame (for the helper's "near"): near, the hand's
    // ray stays hidden (with the leave margin and linger, so it doesn't flicker at the zone's edge);
    // claims, its pinches and grips are ours: within the zone itself (or pressing, grabbing, or one
    // waiting on "grab that"), so a pinch in the margin goes to the ray or the gaze.
    bool near(int side) const { return side_[side].state != State::Idle && side_[side].state != State::Off; }
    bool claims(int side) const {
        const Side &sd = side_[side];
        return sd.state == State::Press || sd.state == State::Grab || (sd.state == State::Hover && (sd.inZone || sd.fistWait || sd.pinchWait));
    }
    bool anyNear() const { return near(0) || near(1); }
    // A hand within Params::rateReach of a panel (for want-hz).
    bool approaching() const { return approaching_; }
    // This Update processed a new hands frame, captured at frameNs().
    bool newFrame() const { return newFrame_; }
    int64_t frameNs() const { return frameNs_; }
    // The caller ended this side's press in KWin itself (vr.cpp's ReleaseStuck): forget it.
    void ForgetPress(int side);
    bool pressing(int side) const { return side_[side].state == State::Press; }
    // ...holding KWin's button (not a poked control)
    bool pressingKwin(int side) const { return pressing(side) && side_[side].control == Control::None; }
    bool grabbing(int side) const { return side_[side].state == State::Grab; }
    // ...and turning it with the wrist (past the dead zone)
    bool turning(int side) const { return grabbing(side) && side_[side].turning; }
    int grabPanel(int side) const { return side_[side].state == State::Grab ? side_[side].panel : -1; }
    // "left hover 2 d 0.031, right idle"
    std::string Describe() const;

    struct Stats {
        int frames = 0, nearFrames = 0;
        int presses = 0, releases = 0, lostReleases = 0, deepReleases = 0, controls = 0;
        int grabs = 0, grabEnds = 0, stretches = 0;
        // presses that didn't happen, by the guard that stopped them (frames at d <= 0 over content)
        int heldNotStereo = 0, heldNotExtended = 0, heldNotNearest = 0, heldSlow = 0, heldPinching = 0, heldBusy = 0,
            heldAskew = 0, heldResting = 0, heldTyping = 0;
        int quickPresses = 0;   // pressed on one frame (Params::travel)
        int attempts = 0, refused = 0;   // pokes that crossed the surface, and those that didn't press
        int pinchClicks = 0, cards = 0, fists = 0, fistsWaited = 0, fistsCancelled = 0;   // (and pinches)
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
        bool quick = false;                  // ...decisively (Params::travel): one frame presses
        double inSpeed = 0, travelled = 0;   // ...how fast and from how far
        bool resting = false;                // the hand rests low by the body (Params::restBelow)
        bool inZone = false;                 // within the zone itself on the last frame (not the margin)
        // The fingers over the last 400 ms (room, capture time): the index tip (the poke's approach),
        // and the thumb tip and the contact (the card pinch).
        struct Sample { int64_t t; float tip[3], thumb[3], contact[3], along[3]; };   // along: wrist to knuckles
        std::vector<Sample> hist;
        double lockU = 0, lockV = 0, lastX = -1, lastY = -1, deepest = 0;
        bool dragging = false, spent = false;
        bool rearmed = true;                 // out past Params::rearm since the last release
        int64_t grabbedNs = 0;
        Control control = Control::None;     // pressed a control (no KWin press)
        double hoverX = -1, hoverY = -1, hoverU = 0, hoverV = 0;
        int64_t hoverNs = -1;                // when the cursor last followed the fingertip
        int64_t pressedNs = 0;
        // a pinch on the content (Params::pinchClick): pressed while the pinch is held; the
        // knuckles' place on the panel then (they hold still as the fingers close)
        bool byPinch = false;
        double pinchU0 = 0, pinchV0 = 0;
        // a poke attempt: the tip crossed the surface (d <= 0) while hovering; what held it back
        struct Attempt {
            bool on = false, pressed = false;
            unsigned why = 0;                // bit per Why
            int frames = 0;                  // at d <= 0
            bool quick = false;              // came in decisively (Params::travel)
            double deepest = 0, speed = 0, travel = 0, angle = 0, ext = 0;
        } attempt;
        // the grab
        uint32_t begins = 0;                 // the pinch slot's begins counter last seen
        bool knowBegins = false;
        uint32_t gripBegins = 0;             // ...the grip slot's
        bool knowGrip = false;
        bool byGrip = false;                 // a fist grab: the grip's end ends it
        bool fistWait = false, pinchWait = false;   // a fist or pinch waiting on "grab that"
        int64_t waitSince = 0;
        int waitPanel = -1;
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
        // the wrist's turn (Params::grabRotate): the hand frame's two directions, smoothed; the
        // hand relative to facing the head at the start (moved on by the dead zone once out of it)
        fhsmooth::Direction along, across;
        double rot0[3][3] = {};
        bool haveRot0 = false, turning = false;
    };
    enum Why : unsigned {
        kOneFrame = 1, kOneView = 2, kBent = 4, kNotNearest = 8, kPinching = 16, kBusy = 32, kOtherHand = 64,
        kTooSoon = 128, kSpent = 256, kNotOut = 16384, kLate = 32768, kResting = 512, kTyping = 1024, kAskew = 2048, kSlow = 4096, kOffContent = 8192
    };
    const handcut::Hand *HandFor(int side, const std::vector<handcut::Hand> &hands, uint32_t prefer) const;
    void Poke(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    void PinchPress(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    void Grab(int s, const Input &in, const std::vector<Panel> &panels);
    // A pinch began near a panel (or waited on "grab that": waited): the card pinch, a grab zone, or a
    // click on the content.
    void PinchBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited);
    // A grip began (or waited on "grab that": waited): the fist grab, if the hand is at its panel.
    void FistBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited);
    // Which panel the hand is near (the claim), before the pinch and grip begins are read.
    void Near(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    // Takes the panel by a hand: the carried point (knuckles) and the wrist's turn start afresh.
    void StartGrab(int s, const Input &in, const Panel &P, Zone zone, int sx, int sy, const handcut::Hand *h,
                   const float *fallback, bool byGrip);
    // Whether the thumb and index tips straddled P's plane at its border in the last cardMs.
    bool Straddled(int s, const Panel &P, int64_t now) const;
    // The room turn of a card-pinching hand since the grab (Params::cardRotDeadzone); false within it.
    bool RoomTurn(int s, const Input &in, double out[3][3]);
    void EndAttempt(int s);
    void Log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void Carries(const Input &in, const std::vector<Panel> &panels);
    void Glows(const Input &in, const std::vector<Panel> &panels);
    // A grabbing side's wrist turn since the grab (Params::grabRotate), in the frame facing the head
    // across the floor from the pinch point (CarryPose applies it in the frame the panel faces the
    // head in); false while within the dead zone (or off).
    bool Turn(int s, const Input &in, const double pinch[3], const double head[3], double out[3][3]);
    void SampleTurn(int s, const handcut::Hand &h, const Input &in);
    void EndPress(int s, bool lost, const char *why = nullptr);
    void EndGrab(int s, bool lost);
    // A grabbing side's pinch point: smoothed, at the tick's time, within its dead zone (deadzone).
    bool Carried(int s, const Input &in, double out[3], bool deadzone = true);
    void Emit(Event::Type t, int s, int panel, double x = 0, double y = 0, bool lost = false);
    const Panel *Find(const std::vector<Panel> &panels, int index) const;

    Params p_;
    Side side_[2];
    std::vector<Event> events_;
    std::vector<Carry> carries_;
    std::vector<std::string> logs_;
    bool approaching_ = false;
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
