// Near hand input on Frametop's own panels (frame-hands/notes/hand-interaction.md, sections 3,
// 16 and 17), within Params::nearFront (about 3 inches): poke a panel with the index finger to click
// it; pinch its border to move it (by any corner: resize it, the opposite corner staying put), its
// content to click; hold its edge like a card (the index and middle against the thumb, or the
// fingertips bunched) to carry it; make a fist at it to carry it; hold it with both hands to stretch
// and turn it; flick it away, or throw it as you let go. The pointer helper does everything farther
// away (the hand ray, the gaze); the two meet through the near claim: for each hands frame,
// ft-screens tells the helper which hands are near a panel ("near <capture_ns> <L> <R>" to
// @ft_pointer_helper: 1 claims(), 2 near() only), and the helper leaves a pinch or grip that began
// on a claimed hand.
//
// Gesture -> action, and what wins (2026-10-10, after the third live test):
//   index tip through the surface, from the front     poke: press, release, drag (Poke)
//   pinch at the edge, thumb behind, index and middle  card: rigid carry (Zone::Card); the index
//     together in front (or four tips bunched)          alone is the border's move, a card if the
//                                                       middle joins within cardJoinMs
//   pinch where the fingers meet in the border band    any corner or the tab: resize from it, the
//                                                       opposite corner kept; border, bar, title: move
//   pinch on the content, within the zone              click at the cursor, held with the pinch
//   fist (grip) with the palm or knuckles at it        carry and turn with the wrist (Zone::Fist);
//     (or a closed hand ft-hands calls a pinch)          not a click (Params::fistIndex)
//   two grabs (pinches or fists) on one panel          stretch, and turn on every axis with them; the
//     (the second: anywhere on a panel the other holds)  second hand's pinch or fist joins (joinFront)
//   pinch or poke on a round button (curve, reset...)   presses it (buttonReach: a bigger hit area)
//   a carry by one hand let go moving out fast         thrown 3 m out along that line (cardThrow)
//   an open-ing hand at a panel flicked out fast       flicked 3 m out from the eyes (flick); not
//                                                       a poke, a wave, or "grab that"'s pull back
//   an open hand, palm to it, pushed into it           pushed 3 m out from the eyes (push); not a
//                                                       poke, a rest, a wave or the back of the hand
//   a pinch or fist on a hand "grab that" has          waits: that gesture's, or ours if it lets go
//   pinch or fist beyond the zone (or in its margin)   the pointer helper's (ray, gaze, grip drag)
// A hold (a grab, a poke's or pinch's press) lasts through tracking loss and flickers until a clear
// release (Params::holdLostMs); what it carries ignores frames no hand makes (Params::gate). The
// resize handle a laser or the gaze presses is vr.cpp's (UpdateCorner); corners() tells it which
// corner a hand is at, and guide() what each hand's soft glow round a panel's corner, edge or button
// shows (vr.cpp draws it). The helper tells ft-screens which
// hands are bare and whether hand input is allowed now ("hands <L> <R> <allowed>" on @ft_screens;
// vr.cpp).
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

// The round buttons a poke or a pinch presses (not the bar, the roll knob or the resize tab).
inline bool PressableButton(Control c) { return c == Control::Curve || c == Control::Reset || c == Control::Dock || c == Control::Close; }
const char *ControlName(Control c);

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
    // One of SteamVR's own panels (vr.cpp's SteamPanels: the dashboard, the Steam menu, Steam's keyboard,
    // other apps' overlays), clicked and carried through the pointer helper's laser: pokes, pinch clicks,
    // the border's move and a corner's resize, and the glows. No fist grabs, two-hand stretch, throws,
    // flicks, pushes, two-finger scroll or let-go rewind (the laser can't do them).
    bool foreign = false;
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
    // Stuck presses (user, 2026-10-11): a poke's press ends as the index curls under pressCurl; a pinch's
    // click as the thumb and index tips are seen pinchApart apart; either held still (not dragging) past
    // pressStillMs (0: never). HANDS_PRESS_STILL_MS.
    double pressCurl = 1.2, pinchApart = 0.045, pressStillMs = 1500;
    // Two-finger scroll (user, 2026-10-11: "tap on a display with two fingers, you can swipe to scroll"):
    // the index and middle straight (scrollExtended) and together (tips within scrollTogether), one of
    // them at the surface (scrollTouch in front or past it), on the content: the pointer goes there and
    // moving them along the panel scrolls, a notch per scrollNotch (m), the content following the fingers
    // (up: scroll down). Ends as they come off (release), part, or bend. HANDS_TWO_FINGER_SCROLL.
    bool twoFingerScroll = true;
    // A straight-middle poke looks like two fingers (the second live test: the middle reads nearly as
    // straight, its tip as deep), so two fingers touching don't press: they wait. Moved scrollStart along
    // the panel: a scroll; off again within scrollWaitMs: a click where they touched; still longer: a
    // press there, as a poke's.
    double scrollExtended = 1.4, scrollTogether = 0.035, scrollTouch = 0.006, scrollNotch = 0.012;
    double scrollStart = 0.012, scrollWaitMs = 350, scrollLevel = 0.008, scrollOthersCurl = 1.3, scrollLift = 0.05;
    // A hand's fingertip or palm within rayReach in front of a panel (over it, 10 cm round) hides its
    // ray, and for rayLingerMs after it leaves (user, 2026-10-11: "the laser still comes back at odd
    // moments": the ray came up for a moment as the hand rose to a panel, 10-25 cm before the touch
    // zone, and as it came away). HANDS_RAY_HIDE_REACH (0: only the touch zone and its margin).
    double rayReach = 0.22, rayLingerMs = 800;
    // the poke (3.5)
    double extended = 1.5;      // index tip-to-wrist over knuckle-to-wrist: the finger is straight
                                // (1.6 until 2026-10-10: a relaxed pointing finger read 1.5-1.6)
    // The index is the finger that touches: the other fingertips no deeper than the index tip by more
    // than this (a negative `nearest`: they may be this much deeper; 0.005, the index 5 mm nearer,
    // until 2026-10-10: 14 of 39 refused pokes in the second live test. The replays show why: at
    // 20-30 deg the tracker reads a pointing hand's middle finger nearly as straight as the index
    // (1.65-1.75 against 1.7-1.8) and its tip within -7..+4 mm of the index tip's depth),
    double nearest = -0.006;
    double pointing = 0.2;      // ...or the hand points: the index straighter than each other finger by
                                // this (as `extended`). A pointing hand coming at a panel from below at
                                // an angle has its loosely curled fingertips 1-3 cm nearer the surface
                                // than the index tip (the hand recorder's touch take: index 1.9, the
                                // others 1.3-1.65)
    // A poke just past the content's edge (within edgeSlack, not on a control) presses at the edge:
    // the desktop's panel sits on the bottom edge, and the tracker's 1 cm puts a tap on it outside.
    // Farther out it isn't a poke (a hand reaching for the bar or a corner crosses the plane there),
    // and isn't counted as one.
    double edgeSlack = 0.015;
    // At d <= 0 for pressFrames processed frames in a row; or for one frame when it came in
    // decisively: from at least `travel` farther out within travelMs, at quickSpeed or faster. A quick
    // tap at 30 Hz is past the surface for one or two frames, and a tap the tracker has 1 cm short is
    // past it for none: two frames in a row lost most of them (live test, 2026-10-10). travel 0.017
    // since the second live test (0.025 before: taps on the content came 1.7 and 2.0 cm, at 0.36 and
    // 0.10 m/s, one frame past). A travel under steadyTravel also needs the tip coming in on each of
    // the last steadySteps frames by 2 mm or more. A finger held still 1.5 cm out with 6 mm of noise
    // a frame presses 4 times a minute (2 with the old rule, 8 at 1.5 cm without the steady rule, 5
    // with it); at 2.5 cm out, or with 3 mm of noise, none. HANDS_POKE_TRAVEL.
    int pressFrames = 2;
    double travel = 0.017, travelMs = 300, quickSpeed = 0.10;
    double steadyTravel = 0.025;
    int steadySteps = 3;
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
    double tapMs = 250;         // ...and for this long after the press: a tap is a click where it went down (HANDS_TAP_MS)
    // The aim locks on the way in (user, 2026-10-11, "how can we make tapping more reliable"): the point a
    // tap lands on follows the fingertip until it's lockOut from the surface, then holds there (the tip's
    // jitter and its slide as the finger goes in and curls don't move it), unless the tip slides lockSlip
    // along the panel (a new target). The cursor and the fingertip dot show it. HANDS_TAP_LOCK (0: off).
    double lockOut = 0.015, lockSlip = 0.025;
    double lockPushDepth = 0.01;   // ...and pushed this far in through the panel, it taps there (HANDS_LOCK_PUSH, 0: off)
    double lockPushSpeed = 0.015;  // ...coming in at least this fast (m/s; a hand drifting into a panel isn't a push)
    double lockPushAngle = 60;     // ...the finger within this of the panel's normal (deg; the poke's own limit is fingerAngle)
    double lockPushDeepest = 0.05; int lockPushFrames = 10;   // ...on its way in: within this deep, these frames past the surface
    // Where a tap lands (HandInput::FingerHit; HANDS_TAP_AIM): the tip's path carried on to the surface,
    // at most pathSlope times the tip's distance (and fingerRayMax) off its own point.
    int aimMode = 2;   // HandInput::FingerHit: 2 the tip's path (HANDS_TAP_AIM=path), 1 the finger's line (finger), 0 straight in (straight)
    double fingerRayMax = 0.04, pathMs = 100, pathSpeed = 0.05, pathSlope = 1.2;
    // A press by motion: a tip that came in quickly (motionSpeed m/s on the frame before, motionTravel
    // within 300 ms) and stopped or turned back short of the surface (within motionShort of it) presses
    // there too: the depth is the weakest number, a tap that "touched" often reads 0.5-1.5 cm out.
    // HANDS_MOTION_PRESS (0: off).
    bool motionPress = true;
    double motionSpeed = 0.25, motionStop = 0.05, motionShort = 0.015, motionTravel = 0.03;
    // The fingertip dot (Guide::Touch; HANDS_TOUCH_DOT): its radius from dotFar (at the touch zone's
    // edge) to dotNear (at the surface), metres.
    bool touchDot = true;
    double dotFar = 0.012, dotNear = 0.005;
    double rePressMs = 150;     // no new press this soon after a release,
    double rearm = 0.01;        // ...nor until the tip has been this far out in front since (a noisy
                                // pull-back let go mid-tap and pressed again: a double click)
    double tooDeep = -0.08;     // through the panel: released where it was
    double lostMs = 250;        // a hovering hand unseen this long isn't near any more
    // Holding through tracking loss (user, 2026-10-10: losing the hand dropped what it held). A hand
    // that holds a panel (a pinch, card or fist grab) or KWin's button (a poke or a pinch on the
    // content) keeps it until a clear release: seen again with the gesture ended (ft-hands' pinch or
    // grip up) and its fingers open (thumb to index tip over holdPinchOpen; for a fist, the fingers'
    // mean extension over holdGripOpen) on releaseFrames hands frames in a row (or the gesture ended on
    // releaseMaxFrames, whatever the fingers say). A gesture that ft-hands ended as lost (or that
    // flickered: ended and began again) on a hand still seen closed goes on holding. Unseen for
    // holdLostMs, it ends where it is (HANDS_HOLD_LOST_MS). Meanwhile the carried point goes on with
    // the hand's last velocity (over velocityMs), slowing to a stop over coastMs; when the hand is
    // back, it blends from there to the hand over blendMs (smoothstep), longer when that's more than
    // catchUp m/s on average. A frame whose point jumped more than
    // jumpMax from the last one counts as unseen (a tracker glitch), unless jumpFrames in a row do.
    double holdLostMs = 1750;
    int releaseFrames = 2, releaseMaxFrames = 6;
    double holdPinchOpen = 0.035, holdGripOpen = 1.35;
    double velocityMs = 110, coastMs = 150, blendMs = 100, catchUp = 0.6;   // (blend: longer for more than catchUp m/s)
    double jumpMax = 0.12;
    int jumpFrames = 3;
    // Tracking that can't be a hand (the third live test, 2026-10-10: "grabbing the side of the monitor
    // (e.g. from the left side), my hand moves off to the left and tracking gets lost: the grabbed
    // monitor is jittery"). On its flag (flag-20261010-203635) the left hand, 45-60 deg left of the
    // head's forward, seen by both cameras, read alternately at two places 10-20 cm apart (6-7 m/s
    // back and forth), its frame flipping 100-180 deg between frames; then by one camera at 85 deg.
    // With `gate` (HANDS_TRACK_GATE), a holding hand's frame that is an impossible move from the last
    // one kept is dropped: faster than gateSpeed, or farther than 0.5 gateAccel dt^2 + gateNoise from
    // where its velocity had it (jumpMax's rule when off); unless gateFrames frames in a row agree on
    // the new place (it did move: the blend takes the panel there). The hand's turn likewise: faster
    // than gateTurn deg/s (at least gateTurnMin a frame). A frame one camera saw (its depth 2-6 cm off)
    // or near the edge of view (edgeDeg..edgeLostDeg off the head's forward, the weight falling to
    // monoWeight) is smoothed harder (the cutoffs times its weight) and doesn't turn the panel; past
    // edgeLostDeg it counts as unseen (the panel holds, holdLostMs). A coast goes at most coastSpeed.
    bool gate = true;
    double gateSpeed = 4.0, gateAccel = 100, gateNoise = 0.02;
    int gateFrames = 3;
    double gateTurn = 540, gateTurnMin = 20;
    double monoWeight = 0.35, edgeDeg = 50, edgeLostDeg = 80;
    double coastSpeed = 0.8;
    // Pinch grab zones (3.6), tested at the pinch's begin with the thumb-index contact (where the
    // fingers meet; the gestures file's point is the knuckles, 5-8 cm behind it, which a 3 cm deep
    // zone rarely had). A band along the panel's border, bandOut outside it to bandIn inside, within
    // bandDepth of its surface either side: corners (within cornerReach) resize, the rest of it
    // moves; and the bar (barReach) and resize tab. bandDepth is the touch zone's depth (vr.cpp sets
    // it to nearFront, at least 4 cm), so a pinch in the zone by the border always takes the panel.
    // HANDS_GRAB_BAND (bandOut). 2026-10-10: user, "I still can't pinch to grab displays easily"
    // (4 cm, 3 cm, 2.5 out, 1.5 in, 3 deep before).
    double barReach = 0.06, cornerReach = 0.06, bandOut = 0.06, bandIn = 0.025, bandDepth = 0.075;
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
    // Since the second live test (user, 2026-10-10) a card takes more than the index: the index and
    // middle fingers together against the thumb (thumb to middle tip within cardMiddle, index to middle
    // tip within cardTogether), or all four fingertips bunched at the thumb (each within cardBunch of
    // its tip: the "pinched fingers" hand). A plain thumb-index pinch at the edge is the border's move
    // (or a corner's resize). The middle finger often closes a little after the index: a border move
    // the fingers turn into a card within cardJoinMs of its start becomes one, from there.
    bool cardPinch = true;
    double cardSide = 0.003, cardAngle = 60, cardAlong = 45, cardMs = 300, cardBand = 0.05, cardRotDeadzone = 3;
    double cardMiddle = 0.045, cardTogether = 0.035, cardBunch = 0.05, cardJoinMs = 300;
    // The fist grab (user, 2026-10-10: "grab a display and use the fist to expand and rotate it, as
    // well as change its angle"): a grip (ft-hands' closed hand) that begins with the hand near a
    // panel (its palm or knuckles within the near zone, over it) takes it: carried like a pinch on
    // its bar (the knuckles 1:1, the wrist turning it), two on one panel stretch it. HANDS_FIST_GRAB.
    bool fistGrab = true;
    // ...for resizing only (user, 2026-10-11: "gripping should only be for resizing windows: the
    // tracking is too wonky to pinpoint, refine and move windows with it"): one fist holds the panel
    // still (an anchor), and two hands on it with a fist among them only scale it about its centre,
    // no move or turn. Pinches keep carrying and turning. HANDS_FIST_RESIZE_ONLY (0: the fist carries).
    bool fistResizeOnly = true;
    // Two fists near one panel resize it, by the hands' shape alone (user, 2026-10-11: "really hard to
    // trigger: if there are two fists anywhere near the display, you should be able to resize it"; the
    // fist grab waits on ft-hands' grip, which begins only on a hand seen open just before, at the
    // panel): both hands closed (the index's extension fistPairIndex or less, the other three's mean
    // fistPairCurl or less) with their palms within fistPairFront in front of the same panel (fistPairBack
    // behind), over it or fistPairMargin round it, for fistPairFrames frames: both take it, as fists.
    // Held while each stays closed (fistPairKeep: the curls' threshold to open); a hand lost: as any
    // grab. HANDS_FIST_PAIR (0: off).
    bool fistPair = true;
    double fistPairIndex = 1.3, fistPairCurl = 1.25, fistPairKeep = 1.45;
    double fistPairFront = 0.30, fistPairBack = -0.15, fistPairMargin = 0.30;
    int fistPairFrames = 3;
    // ...and a pinch by a closed hand: the index curled (its extension, as `extended`, fistIndex or
    // less) with the middle, ring and little (their mean fistCurl or less). ft-hands calls a fist with
    // the thumb on the index a pinch: the third live test's two-fist stretch (flag-20261010-203215:
    // index 0.79 and 1.11, the others 0.78 and 0.94) pinch-clicked the content instead, one hand,
    // then "the other hand is pressing". The user's pinches on the ten flags: the index 1.24-1.65 (a
    // few 0.84-1.04 with the others at 1.18-1.32), the others 1.09-1.45; their fists: the index
    // 0.79-1.13, the others 0.71-1.00.
    double fistIndex = 1.15, fistCurl = 1.05;
    // A fist or a pinch that begins on a hand "grab that" has (held out open, or closing: the thumb
    // passes the index tip as a hand closes, a pinch to ft-hands) waits until that gesture fires (then
    // it's that) or lets the hand go, at most grabThatWaitMs: on the user's flag, with a panel where
    // the gesture brings one, a pinch-click on the closing hand took the hand from it.
    double grabThatWaitMs = 600;
    // The fourth live test (2026-10-10, flags 215945 and 215955: "I can't grab using fists to pull and
    // resize and rearrange a display with the two anchor points"). The left fist took the panel; the
    // right fist was a pinch to ft-hands that began with the thumb on the index while the other fingers
    // were still closing (the index 1.07, the others 1.36; a frame later 0.86 and 0.82): a click on the
    // content, held 1.8 s, and no second anchor; later in the live log a right fist went 8.5 cm through
    // the panel ("not at it"), and fists held 11-13 cm in front of it were nobody's. So:
    // a pinch on the content by a hand closing (its middle, ring and little fingers' mean under
    // fistSettleCurl, down by fistSettleDrop or more over the last fistSettleMs) waits up to fistSettleMs
    // to become a fist; if it doesn't, it clicks then (where it began), or as it ends. On the flags 27 of
    // 146 pinches began as a hand closed into a fist (the three fingers 0.94-1.37 then, under 0.95 within
    // 130 ms); a pinch by a still hand (those fingers where they were) clicks at once, as before;
    double fistSettleMs = 150, fistSettleCurl = 1.4, fistSettleDrop = 0.1;
    // a fist is at a panel with its palm, knuckles or curled middle finger up to fistFront in front of
    // it (fistBack behind), over it or by its edge; a hand curled that far (the three fingers' mean
    // under fistClaimCurl) is claimed that far out, so the pointer helper leaves its grip.
    // HANDS_FIST_REACH (fistFront).
    double fistFront = 0.11, fistBack = -0.10, fistClaimCurl = 1.2;
    // A second hand at a panel the other hand holds: within joinFront in front of it (joinBack behind,
    // joinMargin round it) its pinch or fist, anywhere on it, is the second anchor (a stretch: move with
    // their midpoint, scale with their distance, turn with them), never a click or a poke, and it doesn't
    // wait on "grab that". Nor does a push or a flick throw a panel a hand holds. 0: off (the second hand
    // had to find a grab zone like the first). HANDS_JOIN_REACH (joinFront).
    double joinFront = 0.15, joinBack = -0.12, joinMargin = 0.10;
    // Buttons for hands (the fourth live test: "it's extremely difficult to interact with the button to
    // make the curve curve and not"). At arm's reach the curve button is 8 mm across, 3.5 mm past the
    // bar's end; the log's six pokes at it landed 3-5 mm left of its centre and the bar, listed first
    // with a 1 cm margin, took them ("on the bar: not a poke"); a pinch there took the bar's grab zone.
    // Now the nearest control wins, round buttons have a hit area at least buttonReach each way for a
    // fingertip or a pinch, and a pinch on one presses it (pinchButtons, HANDS_PINCH_BUTTONS); a hand at
    // one lights it (hovered) and shows a soft ring round it (guide). HANDS_BUTTON_REACH.
    double buttonReach = 0.025;   // (1.5 cm until 2026-10-11: buttons on the bar were hard to hit right after a move)
    bool pinchButtons = true;
    // The soft white glow round a panel's corner (where a pinch resizes it), along its edge (where a pinch
    // moves it) and round a button, per hand (guide(); vr.cpp draws it, cutouts on or off), instead of the
    // cutout buffer's rings there and the resize handle following the hand. HANDS_EDGE_GLOW.
    bool edgeGlow = true;
    double stretchDead = 0.01;  // two hands: their distance changes this much before the panel scales
    // Two hands turn the panel on every axis with the line between them, and roll it about that line
    // with their wrists (the mean of the two hands' turns about it, past rotDeadzone); off: about the
    // vertical only (before 2026-10-10's third live test: "grabbing the monitor with two fists doesn't
    // let me resize or rotate"). HANDS_STRETCH_TURN.
    bool stretchTurn = true;
    // A pinch claimed by the touch zone whose fingers met a little farther out than the grab band's
    // depth still takes the border (the third live test: "the fingers +9.0 cm from it, out of the
    // touch zone: nothing", the helper having left the pinch to us).
    double pinchSlack = 0.03;
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
    // ...with `gate`, the turn's smoothing at most rotMaxCutoff Hz however fast the hand turns: a
    // flip the gate takes (gateFrames frames in a row of it) is followed over about 40 ms, not at once
    double rotMaxCutoff = 4;
    // ...and a carried panel (one hand, not a card, or two) turns at most maxTurn degrees a tick (540
    // deg/s at 90 Hz) about the hand's point: the fourth live test's fists made more carries, and a
    // fist pulled in toward the face (flag-20261009-201252, replayed) flipped the panel 30-90 deg
    // between ticks where CarryPose's facing has no steady answer. 0: no limit.
    double maxTurn = 6;
    // Throws (user, 2026-10-10). Both send the panel throwDistance from the eyes, facing them, over
    // throwMs (ease-out); "grab that" brings it back.
    // The flick (HANDS_FLICK): a quick outward flick of an open hand at a panel. The palm within
    // flickNear in front of a panel (or flickBack behind it), over it, and moving slower than
    // flickStartSpeed there (a flick starts from the panel; a hand reaching through one passes it at
    // speed: without this the recordings' probe moves threw a panel 0.3-0.45 m ahead 1-3 times in
    // 2-3 minutes), on some frame in the last flickMs; since then moving away from the eyes (within flickAngle of the line from them to the
    // palm) at flickSpeed or more (a fit over the last flickFitMs), flickTravel or more from there; the
    // fingers open (their mean extension flickOpen or more: a pointing hand's is about 1.3, an open
    // one's 1.7-1.9) and opening on the way (by flickOpening or more since the start: a flick, not an
    // open hand reaching out, which the recordings have now and then; 0: an open hand flicks too,
    // HANDS_FLICK_OPENING); no pinch or grip down; the hand not pressing or grabbing. Where to: on the line
    // from the eyes through the panel's centre. An open hand that comes through a panel faster than
    // flickPokeSpeed doesn't poke it. Once a second at most.
    // The lazy susan by hand (user, 2026-10-11: "paw back and forth like pushing branches out of the way,
    // left or right, and lazy susan through windows and displays"): an open hand on its edge (fingers
    // open, sweepOpen or more; the palm's normal within sweepEdge of level, and along the way it moves
    // by sweepLead or more: the palm or the back of the hand leading, never a flat hand), off every
    // panel's surface (not pressing or holding; hovering near one counts: the panel the lazy susan brought
    // sits right there), sweepFront or more in front of the eyes, between sweepBelow under them
    // and sweepAbove over them, sweeps sideways sweepDistance or more within sweepMs, drifting at most
    // sweepDrift up, down, forward or back: one stroke, one panel (Event::Spin: x -1 the hand went left,
    // so the panel on the right comes to the front; +1 right, the one on the left). Every stroke counts,
    // back and forth too. A stroke starts afresh where the hand turns or slows under sweepRest. HANDS_SWEEP.
    // (Easier since the second live test, 2026-10-11: "really difficult": strokes counted came at 30-35 cm,
    // just over the 30 cm asked; was open 1.55, edge 0.55, lead 0.6, front 0.18, 30 cm in 600 ms, drift 0.15.
    // The flags replayed: 22 cm caught 3 strokes in 49 flags of other use (fists and drags), 25 cm one.)
    // (Easier again after the gaze test, 2026-10-11 18:05: "way too difficult": the strokes counted came at
    // 25-30 cm in 230-470 ms, at both limits. Now 22 cm (18 caught 3 strokes of 19-21 cm in 45 flags of other use), quick somewhere along it (sweepPeak, 0.7 m/s, so a
    // slow drift isn't one) within sweepMs 900; open 1.3, edge 0.75, lead 0.4.)
    double sweepPeak = 0.7;
    bool sweep = true;
    // only the palm leading (user, 2026-10-11: "the reverse, moving towards the back of the hand, doesn't do
    // anything; using it will require palming through with both hands": the right hand palming left brings
    // the next, the left hand palming right the one before). HANDS_SWEEP_BACK=1: the back of the hand too.
    bool sweepBack = false;
    // (Easier again 2026-10-13, "still too hard to trigger": the live misses were palm-led strokes with the
    // palm's lead 0.16-0.24 of 0.25, palm-led ones tilted 0.81-0.88 toward flat, and strokes drifting 21-22
    // cm up of 20. Lead 0.3 (one hand 0.12; a flat hand's is 0.05-0.07), edge 0.85 (one hand 0.9: 64 deg off
    // its edge; flat strokes read 0.93-0.98), drift 0.3, 4 frames off shape ridden out.)
    double sweepOpen = 1.3, sweepEdge = 0.85, sweepLead = 0.3, sweepFront = 0.15, sweepBelow = 0.5, sweepAbove = 0.15;
    double sweepDistance = 0.22, sweepMs = 900, sweepDrift = 0.3, sweepRest = 0.15;
    // One hand on its own (user, 2026-10-13: "make it easier to trigger intentionally when one hand is being
    // moved"; the misses were "not an open hand on its edge"): with the other hand unseen or still (its palm
    // under sweepCalm m/s), a looser shape and a shorter stroke count: open sweepSoloOpen, edge sweepSoloEdge,
    // lead sweepSoloLead, sweepSoloDistance, quick at sweepSoloPeak somewhere. Either way a stroke rides out
    // sweepBadFrames frames off its shape (one view's jitter ended strokes).
    // (A little harder again 2026-10-13, "just slightly more difficult": one hand edge 0.88, lead 0.16, 20 cm,
    // quick at 0.75 m/s (every practice stroke in the flags peaked at 0.8 or more; a false one at 0.6), 3 frames
    // off shape. The flags replayed: 47 practice strokes still count, other use 3 strokes, from 4.)
    double sweepCalm = 0.3, sweepSoloOpen = 1.1, sweepSoloEdge = 0.88, sweepSoloLead = 0.16, sweepSoloDistance = 0.2;
    double sweepSoloPeak = 0.75;
    int sweepBadFrames = 3;
    bool flick = true;
    double flickSpeed = 1.2, flickTravel = 0.12, flickMs = 200, flickFitMs = 100, flickNear = 0.12, flickBack = 0.04;
    double flickAngle = 40, flickOpen = 1.5, flickPokeSpeed = 0.8, flickRefractoryMs = 1000, flickStartSpeed = 0.6;
    double flickOpening = 0.15;
    // The card throw (HANDS_THROW): a hand carrying a panel (a card, a fist, a pinch on its border or
    // bar; not a resize or two hands) that lets go moving at throwSpeed or more (its velocity over the
    // throwFitMs, its fastest such stretch in the last throwPeakMs before the fingers opened), within
    // throwAngle of away from the eyes: the panel goes on along that line (vr.cpp: as far out and as big
    // as it was before it was taken, if it remembers). Slower, it stays where it was let go. (Easier since
    // 2026-10-13, "too difficult to throw ... when you're doing the pinch grab": was 0.55 m/s within
    // 60 deg, over the last 100 ms only, when a hand letting go has slowed already.)
    bool cardThrow = true;
    double throwSpeed = 0.45, throwFitMs = 100, throwAngle = 70, throwPeakMs = 150;
    // Letting go without a nudge (user, 2026-10-11: "when I release, it sees a slight movement of the hand
    // as I let go of the two fingers or the full hand"): while a hand holds a panel, the panel stops
    // following it once its fingers open openFreeze past their tightest this hold (a fist's curl
    // openFreezeCurl past its tightest), before ft-hands calls it let go; closed again (within
    // openRelax): it follows on from where it is. Let go without that, slowly (under rewindSpeed m/s):
    // it goes back to where it was rewindMs before (the opening the freeze didn't see). And the same
    // hand doesn't take the panel it let go of again within regrabMs (a let-go's flicker).
    // HANDS_RELEASE_FREEZE (0: off), HANDS_RELEASE_REWIND_MS (0: off).
    double openFreeze = 0.008, openRelax = 0.004, openFreezeCurl = 0.3;
    double rewindMs = 120, rewindSpeed = 0.3, regrabMs = 250;
    double openBackMs = 70;   // ...the freeze puts it back where it was this long before the opening first showed
    double throwDistance = 3.0, throwMs = 400;
    // The push (HANDS_PUSH; user, 2026-10-10: "pushing against the monitor or window does not make it
    // go away or fly back"): an open hand (the four fingers' mean extension pushOpen or more, the
    // middle, ring and little pushCurl or more: not a poke's index alone; a pointing hand's middle
    // reads up to 1.75 straight, its ring and little 1.2-1.4, so about 1.45 for the three), its palm
    // toward the panel (within pushFacing degrees of into it), over it, moving straight into it (the
    // way it came within pushAngle of the panel's normal), pushDepth or more past the
    // surface, from at most pushFrom in front of it (or resting on it) within the last second, open
    // and facing it all the way: moving into it at pushSpeed or more (a fit over flickFitMs) and
    // pushFastTravel or more in since; or pushTravel or more past the surface, at pushSlow or more on
    // average since it crossed it.
    // Thrown like a flick (throwDistance from the eyes on the line through the panel's centre). Not a
    // hand resting or typing, pinching or gripping, pressing or holding, nor within pushAfterMs of a
    // press or a hold (the hand opening as it lets go); once a second at most (flickRefractoryMs,
    // shared). An open hand doesn't poke while push is on (pokeOpen). The flags: "grab
    // that"'s open hand held out to a panel at arm's reach goes 5-7 cm into it at 0.43 m/s, or 8 cm at
    // 0.14 m/s (flag-20261010-203144, flag-20261009-201252): neither is a push. HANDS_PUSH_SPEED.
    // (Arm's reach is 0.55-0.6 m: a panel at 0.45 can't be pushed far in slowly; push it quickly.)
    bool push = true;
    double pushOpen = 1.6, pushCurl = 1.6, pushFacing = 50, pushAngle = 30, pushFrom = -0.005, pushDepth = -0.02, pushSpeed = 0.5;
    double pushFastTravel = 0.05, pushTravel = 0.15, pushSlow = 0.10, pushAfterMs = 600;
    // An open hand doesn't poke while push is on: the middle, ring and little fingers and all four
    // pokeOpen or straighter on average (flat), or pushCurl or straighter coming in at pushSpeed or
    // more (a push on its way). The replays' made-up pokes along the finger (frames where the index
    // points, the other fingers as the tracker had them) pressed 19 of 23 (probe-moves), 4 of 6 (the
    // 2026-09-29 recording) and 31 of 55 (lit) without it; 11, 4 and 30 at 1.8 (17, 2 and 55 at 1.6).
    double pokeOpen = 1.8;
};

// Card: the card pinch; Fist: the fist grab (both carry it).
enum class Zone { None, Bar, Tab, Corner, Edge, Title, Card, Fist };
const char *ZoneName(Zone z);

struct Event {
    // Throw: the panel goes to `to` over `ms` (ease-out): a flick at it, or a carry let go fast
    // Settle: after a GrabEnd, the panel goes back to `to` at once (Params::rewindMs)
    // Spin: the lazy susan, one panel (x: -1 the hand swept left, +1 right; Params::sweep)
    enum Type { Motion, Press, Release, Control, GrabStart, GrabEnd, Throw, Scroll, Settle, Spin } type;   // Scroll: x, y notches (y > 0: down)
    int side = 0;               // 0 left, 1 right
    int panel = -1;
    double x = 0, y = 0;        // buffer pixels (Motion, Press, Release)
    handinput::Control control = Control::None;   // Control: the button poked or pinched
    Zone zone = Zone::None;     // GrabStart
    int sx = 1, sy = -1;        // GrabStart by a corner: which one (+1 right, top)
    Mat to{};                   // Throw: where the panel goes
    double ms = 0;              // ...over this long
    bool flick = false;         // ...by a flick (else a carry let go fast)
    bool push = false;          // ...by a push
    bool lost = false;          // Release, GrabEnd: the hand was lost, or input was taken away
};

// A panel held by hands, this tick.
struct Carry {
    int panel = -1;
    int sides = 0;              // bit 0: the left hand holds it, bit 1: the right
    enum Kind { Move, Resize, Stretch } kind = Move;
    Mat pose{};                 // Move, Stretch, Resize (not floating): where it goes now
    double width = -1;          // Stretch, Resize (not floating): its width now (m); -1: as it is
    // Resize: the pinch point on the panel (x right, y up, m from its centre), the corner held
    // (+1 right or top), and the pinch point's offset from that corner at the start. A screen keeps
    // its aspect and the opposite corner stays put (ResizeFromCorner); a floating window takes the
    // pixels (vr.cpp's ResizeFloat, the opposite corner kept likewise).
    double hx = 0, hy = 0, grabX = 0, grabY = 0;
    int sx = 1, sy = -1;
};

// A corner of a panel a hand is at (the pinch's contact in its corner zone, or near it): the resize
// handle shows there, brighter the closer (1 in the zone), "held" while the hand resizes from it.
struct CornerAim {
    int sx = 1, sy = -1;
    double closeness = 0;
    bool held = false;
};

// What a hand's soft glow shows (Params::edgeGlow): a corner where a pinch resizes the panel (an L
// round it: sx, sy), an edge where a pinch moves it (a short segment centred at u, v on that edge: sx
// (left/right) or sy (bottom/top) nonzero), or a button (centre u, v, half size `size`). strength 0..1:
// brighter the closer the fingers; selected: the hand holds it (resizing from that corner, pressing
// the button).
struct Guide {
    enum Kind { None, Corner, Edge, Button, Touch } kind = None;   // Touch: where a tap would land (u, v; size: the dot's radius)
    int panel = -1;
    int sx = 0, sy = 0;
    double u = 0, v = 0, size = 0;
    double strength = 0;
    bool selected = false;
    handinput::Control control = Control::None;
};

// A hand at a panel, closer than the touch zone and its margin: its tip, the thumb-index contact or
// its palm within `front` in front of a panel (or nearBack behind it), over it with `margin` round it.
// For the palm menu (not by a panel: a card pinch from below has the palm up).
bool HandAtPanel(const handcut::Hand &h, const std::vector<Panel> &panels, const Params &k, double front, double margin);
bool HandAtPanel(const handcut::Hands &hands, int side, const std::vector<Panel> &panels, const Params &k,
                 double front = 0.12, double margin = 0.08);

// Resizing a panel from one corner, the opposite corner staying put, keeping its aspect (height over
// width): the corner (sx, sy: +1 right, top) wanted at cu, cv (m from the centre, on the surface),
// the panel `width` wide now. Its new width (clamped to minW..maxW) and its centre's new place (u, v
// from the old centre, on the surface).
double ResizeFromCorner(double width, double aspect, int sx, int sy, double cu, double cv, double minW, double maxW,
                        double *u, double *v);
// The pose of the point u, v on the panel's surface (turned with a curved one's surface there).
Mat SurfacePose(const Mat &pose, double curve, double u, double v);

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
// The control under u, v: the nearest one (by its own rectangle; a round button before the bar on a
// tie) whose rectangle, round buttons' grown to at least buttonMin each way, is within `margin`; or
// Control::None. (Before the fourth live test: the first in the list within `margin`.)
Control ControlAt(const Panel &p, double u, double v, double margin, double buttonMin = 0);
// The grab zone a pinch's contact at h is in (3.6, Params::band*), and for corners which one (sx,
// sy: +1 right, top). excess: how far it is outside that zone (<= 0 inside), for the glow; the
// zone is the nearest one then.
Zone GrabZone(const Panel &p, const Hit &h, const Params &k, int *sx, int *sy, double *excess);
// What a carried panel faces the head from (CarryPose): its centre; held nearer the eyes than
// kCarryShare times the grab point's distance from the centre, a point between the two, that share
// of the way from the grab point (pose: the panel, grabLocal: the grab point on it, panel-local).
// Facing from the centre turns the panel about the hand, which moves the centre, which turns it:
// held by an edge close to the eyes that loop has no steady answer (the third live test's replays:
// a 0.6 m panel held by its edge at arm's reach spun and flipped from tick to tick).
constexpr double kCarryShare = 2;
void CarryAnchor(const Mat &pose, const double grabLocal[3], const double head[3], double out[3]);
// One hand carrying a panel: the grab point (panel-local, at the start) follows the pinch
// point, and the panel keeps the turn it had from facing the head (its yaw, pitch and roll
// relative to that; from CarryAnchor's point), so carrying it around you keeps it facing you. turn (optional): the wrist's
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
// ...turned on every axis: the shortest turn from the line l0-r0 to l-r, then `twist` (radians) about
// that line (the hands' wrists), about the hands' midpoint; level within rollSnap degrees of roll.
Mat StretchTurn(const Mat &start, double width0, const double l0[3], const double r0[3], const double l[3],
                const double r[3], double twist, double dead, double minW, double maxW, bool scale, double rollSnap,
                double *width);

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
    // The same lines that were decisions about an interaction, with what was tried and how it went
    // (hands/include/fh_events.h's kind and outcome; panel: the screen's number, 0 none), for a
    // testing session's events.
    struct Act {
        const char *kind, *outcome;
        int side, panel;
        std::string text;
    };
    const std::vector<Act> &acts() const { return acts_; }
    // "Grab that" fired for this side: a fist or pinch waiting on it (Params::grabThatWaitMs) doesn't
    // grab or click.
    void GrabThatFired(int side);
    const std::vector<Carry> &carries() const { return carries_; }
    // Per panel index: the glows to draw on it (buffer pixels), the controls a hand is near
    // (bit k: Control k), the panels a hand is near (their controls show).
    const std::map<int, std::vector<handcut::Glow>> &glows() const { return glows_; }
    const std::map<int, unsigned> &hovered() const { return hovered_; }
    const std::map<int, CornerAim> &corners() const { return corners_; }
    // Per panel index: an open hand's palm at it, facing it (a flick or a push would throw it), 0..1
    // nearer (vr.cpp highlights it).
    const std::map<int, double> &throwAims() const { return throwAims_; }
    // Per side: the soft glow at a corner, edge or button (Params::edgeGlow).
    const Guide &guide(int side) const { return guides_[side]; }
    const std::set<int> &nearPanels() const { return nearPanels_; }
    // The near claim, per side, for the last hands frame (for the helper's "near"): near, the hand's
    // ray stays hidden (with the leave margin and linger, so it doesn't flicker at the zone's edge);
    // claims, its pinches and grips are ours: within the zone itself (or pressing, grabbing, or one
    // waiting on "grab that"), so a pinch in the margin goes to the ray or the gaze.
    bool near(int side) const { return side_[side].state != State::Idle && side_[side].state != State::Off; }
    int nearPanel(int side) const { return near(side) ? side_[side].panel : -1; }
    bool claims(int side) const {
        const Side &sd = side_[side];
        return sd.state == State::Press || sd.state == State::Grab ||
               (sd.state == State::Hover && (sd.inZone || sd.fistWait || sd.pinchWait || sd.settleWait));
    }
    bool anyNear() const { return near(0) || near(1); }
    // A hand within Params::rateReach of a panel (for want-hz).
    bool approaching() const { return approaching_; }
    // That hand hides its ray (Params::rayReach): at a panel, or was within rayLingerMs.
    bool rayHidden(int side) const { return near(side) || frameNs_ < rayHideUntil_[side]; }
    // This Update processed a new hands frame, captured at frameNs().
    bool newFrame() const { return newFrame_; }
    int64_t frameNs() const { return frameNs_; }
    // The caller ended this side's press in KWin itself (vr.cpp's ReleaseStuck): forget it.
    void ForgetPress(int side);
    bool pressing(int side) const { return side_[side].state == State::Press; }
    // the control a side is pressing (None: not pressing one): a hold on the reset button
    Control pressControl(int side) const { return pressing(side) ? side_[side].control : Control::None; }
    // ...holding KWin's button (not a poked control)
    bool pressingKwin(int side) const { return pressing(side) && side_[side].control == Control::None; }
    bool grabbing(int side) const { return side_[side].state == State::Grab; }
    // ...and turning it with the wrist (past the dead zone)
    bool turning(int side) const { return grabbing(side) && side_[side].turning; }
    int grabPanel(int side) const { return side_[side].state == State::Grab ? side_[side].panel : -1; }
    // A hand holds this panel.
    bool heldByHand(int panel) const { return grabPanel(0) == panel || grabPanel(1) == panel; }
    // A hand presses KWin's button on this panel (a poke's or a pinch's).
    int pressPanel(int side) const { return pressingKwin(side) ? side_[side].panel : -1; }
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
        int offContent = 0;              // ...and crossings off the content and its controls (not pokes)
        int edgePresses = 0;             // presses just past the content's edge (Params::edgeSlack)
        int pinchClicks = 0, cards = 0, fists = 0, fistsWaited = 0, fistsCancelled = 0;   // (and pinches)
        int coasts = 0, reacquired = 0, glitches = 0, heldThrough = 0, lostEnds = 0;   // holding through loss
        int flicks = 0, throws = 0, heldFlick = 0;   // flicks and card throws; pokes not taken from a flick
        int pushes = 0, heldOpen = 0;                // pushes; pokes not taken from an open hand (Params::push)
        int fistPinches = 0;                         // fist grabs ft-hands called pinches (Params::fistIndex)
        int fistPairs = 0;                           // two fists that took a panel by their shape (Params::fistPair)
        int scrolls = 0, scrollNotches = 0;          // two-finger scrolls, and the notches they sent
        int motionPresses = 0;                       // presses by motion (Params::motionPress)
        int lockPushes = 0;                          // presses pushed through at the frozen aim (Params::lockPushDepth)
        int sweeps = 0, sweepsRefused = 0;           // lazy susan strokes, and strokes the hand's shape or place refused
        int releaseFreezes = 0, rewinds = 0, regrabsRefused = 0;   // letting go (Params::openFreeze, rewindMs, regrabMs)
        int settles = 0, settledFists = 0;           // pinches that waited to become fists (Params::fistSettleMs), and did
        int joins = 0;                               // second hands joining a held panel (Params::joinFront)
        int buttonPinches = 0, heldByOther = 0;      // buttons pressed by a pinch; pokes refused on a panel the other hand holds
        int gated = 0, turnsGated = 0, moved = 0;    // frames the gates dropped; places they took after gateFrames
        int edgeFrames = 0, monoFrames = 0;          // held frames smoothed harder: near the edge of view, one camera
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
        // The fingers over the last second (room, capture time): the index tip (the poke's approach),
        // and the thumb tip and the contact (the card pinch).
        // along: wrist to knuckles; normal: out of the palm; curl3: the middle, ring and little fingers'
        // mean extension
        struct Sample { int64_t t; float tip[3], thumb[3], contact[3], along[3], palm[3], normal[3]; float open, curl3; };
        std::vector<Sample> hist;
        double lockU = 0, lockV = 0, lastX = -1, lastY = -1, deepest = 0;
        bool dragging = false, spent = false;
        bool rearmed = true;                 // out past Params::rearm since the last release
        int64_t grabbedNs = 0;
        Control control = Control::None;     // pressed a control (no KWin press)
        double hoverX = -1, hoverY = -1, hoverU = 0, hoverV = 0;
        // the aim (Params::lockOut): where a tap will land, frozen as the tip comes within lockOut
        double aimU = 0, aimV = 0, tipD = 1;
        bool aimSet = false, aimFrozen = false;
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
            // where it crossed (u, v on the panel), and on its last frame past the surface: the nearest
            // other fingertip's depth over the index tip's (m; < 0: deeper than it), which tip, and the
            // straightest other finger (as `extended`)
            double u = 0, v = 0, outside = 0, otherGap = 0, straightest = 0;
            int otherTip = 12;
            bool over = true;
        } attempt;
        // the grab
        uint32_t begins = 0;                 // the pinch slot's begins counter last seen
        bool knowBegins = false;
        uint32_t gripBegins = 0;             // ...the grip slot's
        bool knowGrip = false;
        bool byGrip = false;                 // a fist grab: the grip's end ends it
        bool byShape = false;                // ...a two-fist pair's (Params::fistPair): the hand opening ends it
        bool scrolling = false;              // two fingers on the panel, scrolling (Params::twoFingerScroll)
        bool scrollPending = false;          // ...touching, not yet moved: a swipe scrolls, a lift clicks
        int64_t pendingNs = 0;
        double pendingX = 0, pendingY = 0;   // ...the buffer pixel it touched
        int pendingPanel = -1;
        double scrollU = 0, scrollV = 0;     // ...where the last notch was counted from (m on the panel)
        int scrollOff = 0;                   // ...frames the fingers haven't looked like two fingers
        bool fistByPinch = false;            // ...that ft-hands called a pinch (Params::fistIndex): either ends it
        bool fistWait = false, pinchWait = false;   // a fist or pinch waiting on "grab that"
        int64_t waitSince = 0;
        // a pinch on the content waiting to become a fist (Params::fistSettleMs): where it began
        bool settleWait = false;
        int64_t settleSince = 0;
        float settleAt[3] = {0, 0, 0};
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
        // holding through tracking loss (Params::holdLostMs): hands frames in a row with the gesture
        // up; the carried point's raw track (capture time, room) since the hand was last (re)acquired;
        // coasting on the last velocity while unseen; the blend back once seen; glitch frames in a row
        int upFrames = 0;
        struct Pt { int64_t t; fhsmooth::V3 p; };
        std::vector<Pt> track;
        bool coasting = false, blending = false;
        int64_t coastNs = 0, blendNs = 0, unseenNs = -1;
        fhsmooth::V3 coastFrom, coastVel, blendOff;
        int jumps = 0;
        // the gates (Params::gate): frames dropped in a row and the last of them (the last kept is the
        // track's newest); the hand's frame likewise (the last kept, dropped in a row, the last dropped)
        fhsmooth::V3 dropped;
        int drops = 0;
        int64_t turnNs = 0;
        int64_t droppedNs = 0;
        fhsmooth::V3 turnAlong, turnAcross, droppedAlong, droppedAcross;
        int turnDrops = 0;
        double weight = 1;                   // the last frame's confidence (Params::monoWeight)
        int reacquired = 0;               // ...times the hand came back mid-hold
        bool cardCandidate = false;       // a border move begun straddling the edge: a card if the fingers join
        int64_t upNs = 0;                 // the capture time of the first hands frame its gesture was up
        int64_t flickNs = INT64_MIN / 2;  // the last flick or push (Params::flickRefractoryMs)
        int64_t busyNs = INT64_MIN / 2;   // the last tick it pressed or held (Params::pushAfterMs)
        // letting go (Params::openFreeze): the fingers' tightest gap this hold (a fist: its tightest curl),
        // and whether the panel has stopped following (they began to open); the panel's recent poses
        // (Params::rewindMs); when and what it last let go of (Params::regrabMs)
        double tightest = 1, tightestCurl = 9, apartEma = -1, curlEma = -1;
        int openRun = 0;
        int64_t openNs = 0;
        bool snapBack = false;
        double apartDev = 0, curlDev = 0;
        bool frozen = false;
        std::vector<std::pair<int64_t, Mat>> poses;
        int64_t letGoNs = INT64_MIN / 2;
        // the stroke under way (Params::sweep): where it began (sideways, up, forward: m in the head's
        // level frame), when, which way, whether it's spun or been refused, and the last frame's sideways place
        double swX0 = 0, swY0 = 0, swZ0 = 0, swLast = 0, swPeak = 0;   // swPeak: the stroke's fastest (m/s)
        int64_t swT0 = 0, swLastT = 0;
        double swPalm[3] = {0, 0, 0}, swSpeed = 0;   // the palm, and its speed (m/s, smoothed): is this hand still?
        int64_t swSeenT = 0;
        int swBad = 0;                               // frames off the shape this stroke
        int swDir = 0;
        bool swDone = false, swHave = false;
        int letGoPanel = -1;
    };
    enum Why : unsigned {
        kOneFrame = 1, kOneView = 2, kBent = 4, kNotNearest = 8, kPinching = 16, kBusy = 32, kOtherHand = 64,
        kTooSoon = 128, kSpent = 256, kNotOut = 16384, kLate = 32768, kResting = 512, kTyping = 1024, kAskew = 2048, kSlow = 4096, kOffContent = 8192,
        kFlick = 65536, kOpenHand = 131072, kHeld = 262144
    };
    const handcut::Hand *HandFor(int side, const std::vector<handcut::Hand> &hands, uint32_t prefer) const;
    void Poke(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    void PinchPress(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    void Grab(int s, const Input &in, const std::vector<Panel> &panels);
    // A pinch began near a panel (or waited on "grab that": waited): the card pinch, a grab zone, or a
    // click on the content.
    void PinchBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited);
    // A grip began (or waited on "grab that": waited): the fist grab, if the hand is at its panel.
    // byPinch: ft-hands called it a pinch (a closed hand, Params::fistIndex).
    void FistBegin(int s, const Input &in, const std::vector<Panel> &panels, bool waited, bool byPinch = false);
    void FistPair(const Input &in, const std::vector<Panel> &panels);
    bool Opening(int s, const Input &in);
    Hit FingerHit(int s, const Panel &P, const handcut::Hand &h) const;
    // A pinch on the content (not a grab zone): a click where the fingers met (at c), held with the pinch.
    void PinchClick(int s, const Input &in, const std::vector<Panel> &panels, const float c[3]);
    // A pinch or a grip that began at a panel the other hand holds: the second anchor (Params::joinFront).
    void Join(int s, const Input &in, const std::vector<Panel> &panels, bool byGrip);
    // A pinch on a round button (Params::pinchButtons): pressed, held with the pinch. False: none there.
    bool PinchButton(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand &h);
    // The hand is a fist (Params::fistIndex, fistCurl).
    bool IsFist(const handcut::Hand &h) const;
    // The other hand holds this side's panel (the join's zone applies).
    bool JoinsOther(int s) const;
    // Which panel the hand is near (the claim), before the pinch and grip begins are read.
    void Near(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand *h);
    // Takes the panel by a hand: the carried point (knuckles) and the wrist's turn start afresh.
    void StartGrab(int s, const Input &in, const Panel &P, Zone zone, int sx, int sy, const handcut::Hand *h,
                   const float *fallback, bool byGrip);
    // Whether the thumb and index tips straddled P's plane at its border in the last cardMs.
    bool Straddled(int s, const Panel &P, int64_t now) const;
    // Whether the hand holds like a card: the index and middle against the thumb, or the four
    // fingertips bunched (Params::cardMiddle); what it is, for the log.
    bool CardFingers(const handcut::Hand &h, const char **how = nullptr) const;
    // The room turn of a card-pinching hand since the grab (Params::cardRotDeadzone); false within it.
    bool RoomTurn(int s, const Input &in, double out[3][3]);
    void EndAttempt(int s);
    void Log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void Did(const char *kind, const char *outcome, int side, int panel, const char *fmt, ...)
        __attribute__((format(printf, 6, 7)));
    void Carries(const Input &in, const std::vector<Panel> &panels);
    void Glows(const Input &in, const std::vector<Panel> &panels);
    // Each hand's soft glow (guides_): a button it's at or pressing, the corner or edge a pinch would take.
    void Guides(const Input &in, const std::vector<Panel> &panels);
    // A flick by this hand at a panel (Params::flick), on a hands frame.
    void Flick(int s, const Input &in, const std::vector<Panel> &panels, const handcut::Hand &h);
    void Sweep(int s, const Input &in, const handcut::Hand &h);
    // A push by this hand into a panel (Params::push), on a hands frame.
    void Push(int s, const Input &in, const std::vector<Panel> &panels);
    // A frame's confidence for a held hand (Params::monoWeight): 1, less for one camera or near the
    // edge of view; 0: past the edge (as unseen).
    double Weight(const Input &in, const handcut::Hand &h);
    // Whether the hand's frame (wrist and knuckles) is a plausible turn from the last one kept
    // (Params::gateTurn); the first is.
    bool TurnOk(int s, const handcut::Hand &h);
    // The two hands' mean turn about the line between them since the stretch began (radians; 0
    // without their frames), past the dead zone.
    double StretchTwist(const Input &in, const double axis[3]);
    // Where a thrown panel goes: the point throwDistance from the head along the line from `from`
    // in `dir` (unit), facing the head. False if `from` is that far already, or the line doesn't get there.
    bool ThrowTarget(const double from[3], const double dir[3], Mat *to) const;
    void EmitThrow(int s, int panel, const Mat &to, bool flick, bool push = false);
    // A grabbing side's wrist turn since the grab (Params::grabRotate), in the frame facing the head
    // across the floor from the pinch point (CarryPose applies it in the frame the panel faces the
    // head in); false while within the dead zone (or off).
    bool Turn(int s, const Input &in, const double pinch[3], const double head[3], double out[3][3]);
    // w: the frame's confidence (Weight): the smoothing's cutoffs times it.
    void SampleTurn(int s, const handcut::Hand &h, const Input &in, double w = 1);
    void EndPress(int s, bool lost, const char *why = nullptr);
    void EndGrab(int s, bool lost);
    // A held gesture (a grab's or a pinch press's) this tick: false once it's clearly let go (see
    // Params::holdLostMs), counting hands frames.
    bool StillHeld(int s, const Input &in, const handcut::Gesture &g, bool isNew, bool grip);
    // The hand's velocity (m/s, room) from the carried point's raw track, over `ms` up to `endNs`
    // (0: its newest sample); false with too few samples.
    bool Velocity(int s, int64_t endNs, double ms, fhsmooth::V3 *out) const;
    // The grabbing side's raw point this hands frame (the knuckles), the hand seen and sane: into the
    // smoothing, or the coast and blend.
    void Track(int s, const Input &in, const handcut::Hand *h);
    // The grabbing side's carried point shown now: smoothed, coasting, or blending back.
    fhsmooth::V3 Shown(int s, double now);
    // A grabbing side's pinch point: smoothed, at the tick's time, within its dead zone (deadzone).
    bool Carried(int s, const Input &in, double out[3], bool deadzone = true);
    void Emit(Event::Type t, int s, int panel, double x = 0, double y = 0, bool lost = false);
    const Panel *Find(const std::vector<Panel> &panels, int index) const;

    Params p_;
    Side side_[2];
    std::vector<Event> events_;
    std::vector<Carry> carries_;
    std::vector<std::string> logs_;
    std::vector<Act> acts_;
    bool approaching_ = false;
    int64_t rayHideUntil_[2] = {0, 0};
    int fistPairRun_ = 0, fistPairPanel_ = -1;
    std::map<int, std::vector<handcut::Glow>> glows_;
    std::map<int, unsigned> hovered_;
    std::map<int, CornerAim> corners_;
    std::map<int, double> throwAims_;
    Guide guides_[2];
    std::set<int> nearPanels_;
    std::set<int> foreign_;   // this tick's SteamVR panels (Panel::foreign)
    uint64_t frame_ = 0;
    bool newFrame_ = false;
    fhsmooth::Point3 head_;   // the head carried panels face, smoothed
    Mat lastHead_{};          // the head as last given (throws aim from the eyes)
    bool haveHead_ = false;
    int64_t frameNs_ = 0;
    // two hands on one panel: what it was when the second hand took it
    struct Stretch {
        int panel = -1;
        Mat start{};
        double width0 = 1, l0[3] = {0, 0, 0}, r0[3] = {0, 0, 0};
        // each hand's frame then (Params::stretchTurn), and whether the twist is past its dead zone
        double hand0[2][3][3] = {};
        bool haveHand0[2] = {false, false}, twisting = false;
        double twistFrom = 0;
        Mat last{};                  // its pose last tick (Params::maxTurn)
        bool haveLast = false;
    } stretch_;
};

}  // namespace handinput
