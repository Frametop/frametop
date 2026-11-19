// ft-screens' OpenVR side, shared by its parts: vr.cpp (the panels, their input, the C API in
// vr.h), vrgsapps.cpp (the gamescope apps' panels) and vrtiles.cpp (the dashboard tiles). The
// panel (Screen) and what each part uses of the others'. Not an interface outside ft-screens' C++: compositor.c sees vr.h only.
#pragma once

#include "vr.h"
#include "barhit.h"
#include "barside.h"
#include "dashanchor.h"
#include "dashtiles.h"
#include "gsforward.h"
#include "handinput.h"
#include "hoverhaptics.h"
#include "levelpin.h"
#include "posemath.h"
#include "resizecorner.h"
#include "vrcmd.h"

#include <openvr.h>

#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ftvr {

using Mat = vr::HmdMatrix34_t;
using Clock = std::chrono::steady_clock;

// The pose math (posemath.h, with its test).
inline Mat Identity() { return posemath::Identity<Mat>(); }
using posemath::Column;
using posemath::Dot3;
using posemath::FaceAbout;
using posemath::FacingPose;
using posemath::FacingTurn;
using posemath::Inverse;
using posemath::Mul;
inline Mat Translation(double x, double y, double z) { return posemath::Translation<Mat>(x, y, z); }
inline Mat PanelPose(double x, double y, double z, double yawDeg, double pitchDeg, double rollDeg) {
    return posemath::PanelPose<Mat>(x, y, z, yawDeg, pitchDeg, rollDeg);
}

enum class Drag { None, Move, Resize, Roll };
enum class Mode { Always, Dashboard, Gesture, Toggle };
enum class Lasers { Always, OutsideGames, Dashboard };
enum class InGames { Visible, Hide };

constexpr double kWristZone = 0.06;  // the laser passing this close to a controller is on its wrist
constexpr double kWristLeave = 0.09; // ...and has left it beyond this (so it doesn't flicker)
constexpr double kDotRange = 0.35;   // the guide dot shows while the laser is this close
constexpr double kMinWidth = 0.15;
constexpr double kFade = 10;         // degrees over which a pinned screen fades out
constexpr double kRollSnap = 2.5;    // degrees from level where rolling snaps level
constexpr double kRollTap = 3;       // degrees: Rotate pressed and turned less than this is a click (levels it),
constexpr double kRollTapM = 0.015;  // ...or less than this far round (metres, at its lever: RollLever)
constexpr float kChromeIdle = 0.55f; // the controls' opacity without a laser on them
constexpr long kControlsLinger = 35; // ticks (~0.4 s) the controls stay after a laser leaves
constexpr long kAimLinger = 25;      // ticks (~0.3 s) a panel keeps the laser on after the aim leaves it

constexpr vr::TrackedDeviceIndex_t kNone = vr::k_unTrackedDeviceIndexInvalid;
// A screen pinned to the SteamVR dashboard rides on this in place of a device (UpdateDashAnchor, PinFrame): no
// device has it, so DevicePose fails for it (vr.cpp's kHandHolder are +1 and +2).
constexpr vr::TrackedDeviceIndex_t kDashboard = vr::k_unMaxTrackedDeviceCount + 3;

// A popup or dialog of a floating window: a small panel over it, cut from the same buffer.
struct Sub {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int x = 0, y = 0, w = 0, h = 0;  // in the output's buffer, pixels
};

// A panel's controls (Screen::Controls): its grab bar and its resize corner. Everything else is a
// row of the bar's menu (panelmenu.h, vrbarmenu.cpp): a tap on the bar opens it (Option C, 2026-11-18).
// Hand input knows both, by their index there (handinput::Control).
constexpr int kBarPart = 0, kResizePart = 1, kControls = 2;
constexpr std::pair<int, handinput::Control> kHandControls[] = {{kBarPart, handinput::Control::Bar},
                                                                {kResizePart, handinput::Control::Tab}};

struct Screen {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid, bar = vr::k_ulOverlayHandleInvalid,
                          handle = vr::k_ulOverlayHandleInvalid;   // (bar and handle: a pooled set's, while lent)
    int width = 0, height = 0;    // current buffer size (mouse scale)
    double metres = 1;
    double curve = 0;             // cylinder radius in metres; 0 = flat
    double tileCurve = 0;         // its own curve while it's tiled flat (tiling::FlatWhileTiled)
    const void *shown = nullptr;  // a frame arrived
    bool visible = false;         // shown in VR right now
    bool alone = false;           // hidden on its own (conceal <screen>), whatever the mode
    float alpha = 1;
    vr::TrackedDeviceIndex_t pinned = kNone;  // riding on this controller
    Mat pinRel = Identity();                  // controller -> screen (a level pin: level frame -> screen)
    levelpin::Mode pinMode = levelpin::Rigid; // on the head, Level or Walk (SCREEN_PIN_LEVEL): FloorFrame, not the headset
    Mat levelAt{};                            // a floor pin: the frame it was last placed with (MoveFramePins)
    // Pinned to the dashboard before it was seen this run (WaitForDash): in the room (pinned is kNone) until the
    // dashboard first opens (AttachDashWaits), then on it at dashRel, or (no dashRel) where it is then.
    bool dashWait = false;
    bool dashHasRel = false;                  // dashRel came with the pin (a profile's); moved meanwhile: dropped
    Mat dashRel = Identity();                 // dashboard -> screen
    Mat pose = Identity();                    // where it is in the room, when not pinned
    Drag drag = Drag::None;
    vr::TrackedDeviceIndex_t dragDevice = kNone;
    Mat dragRel = Identity();                 // device -> screen, while moving
    double grabX = 0, grabY = 0;              // resize: the grab point relative to the corner
    bool dragFacing = false;                  // move: kept facing the head (a hand ray carries it: HandRay)
    // The resize handle (Controls()[kResizePart], "<prefix>.resize"): one overlay, at the corner a laser, the gaze
    // or a hand aims at (UpdateCorner; the bottom right otherwise), brighter the closer the aim, lit
    // while resizing. Any corner resizes, the opposite one staying put.
    int cornerX = 1, cornerY = -1;            // +1 right, top
    float cornerGlow = 0;                     // the aim's closeness, 0..1
    bool cornerLit = false;                   // its texture lit (resizing)
    long cornerUntil = 0;                     // aimed at until this tick (then back to the bottom right)
    int keepX = -1, keepY = 1;                // a floating window's corner that stays put as it changes size
    long keepUntil = 0;                       // ...after a resize from another corner ends, until this tick (late answers)
    bool resizeFacing = false;                // a floating window's resize: its turn from facing the head when
    Mat resizeTurn = Identity();              // grabbed, which it turns back to when let go (FaceAfterResize)
    Mat rollFrom = Identity();                // roll: the pose at the press (pinRel when pinned)
    double rollAngle = 0;                     // roll: the laser's angle around the centre then
    double rollRadius = 0;                    // roll: the lever's length then (m)
    posemath::Lever rollLever;                // roll: the knob held at its top or bottom middle, wherever on the menu it's held (RollLever)
    double rollTurned = 0;                    // roll: the most the laser has gone round since (rad; RollClick)
    bool hover[kControls] = {};               // a laser is on the bar, the resize corner
    // The bar's press, to tell a tap (the menu) from a move when it comes up (panelmenu::IsTap).
    int64_t barPressMs = -1;
    vr::TrackedDeviceIndex_t barPressDevice = kNone;
    Mat barPressFrom{}, barPressPose{};       // the device's laser then, and the panel's pose in the room
    uint64_t barPressUniverse = 0;            // ...and its universe (universefix.h)
    vr::TrackedDeviceIndex_t barPressPinned = kNone;   // ...what it was pinned to, and how
    Mat barPressPinRel{};
    bool barTapped = false;                   // the move ending now was a tap, put back (FinishDrag: nothing moved)
    unsigned handHover = 0;                   // ...a hand's pinch point is near them (bit k: control k)
    bool lasers = true;                       // MakeOverlaysInteractiveIfVisible is set
    float controls = 0;                       // the controls' fade, 0 (hidden) .. 1
    bool controlsUp = false;                  // the controls' overlays are shown
    long nearUntil = 0;                       // a laser was near the controls until this tick
    bool handNear = false;                    // a hand is at it (its buttons grow: ChromeSize)
    long handNearUntil = 0;
    long aimUntil = 0;                        // a hand controller pointed at it until this tick (UpdateAim)
    vr::TrackedDeviceIndex_t pinTarget = kNone;  // moving: rides on this controller when let go
    vr::TrackedDeviceIndex_t onWrist = kNone;    // moving: the laser is in this controller's ring
    bool barLit = false;
    uint64_t barPaintedVersion = 0;
    bool barPaintedLit = false;
    barhit::Raster barPaintedRaster{};
    hoverhaptics::Gate barBuzz;
    uint64_t resizePaintedVersion = 0;
    int resizePaintedX = 0, resizePaintedY = 0;
    int64_t resizeAnnouncedMs = 0;
    overlayregion::Region resizeRegion;
    std::string resizeRegionMessage;
    resizecorner::HoverBuzz resizeBuzz;
    const void *key = nullptr;    // the client buffer on it now, and its dmabuf (for cutouts)
    ft_dmabuf buf{};
    vr::SharedTextureHandle_t plain = 0;  // that buffer's SteamVR import
    uint64_t frames = 0;          // client frames presented (a cutout buffer's "serial")
    bool cutting = false;         // showing a cutout buffer (side by side) instead
    vr::SharedTextureHandle_t cutShown = 0;  // ...this one
    double chrome = 0.3;          // the bar's width; the other controls follow it (ChromeSize)
    barside::Side barSide = barside::Side::Bottom;   // the edge its bar and buttons are on (vrbarmenu.cpp)
    double grip = 0.04;           // the corner tab's and the round buttons' size
    // A floating window's panel (see the top): the window's rectangle in the buffer, its
    // title bar's height there, and the density.
    bool floating = false;        // a spare output's panel
    int slot = 0;                 // ...its number, from 1 (its overlays exist only while a window floats)
    bool remote = false;          // a remote display: another machine's monitor, streamed (remote.c)
    bool gsapp = false;           // a gamescope app's window (the gamescope apps): gamescope draws it
    uint32_t gsWin = 0;           // ...which X window
    bool gsSeen = false;          // ...shown before (mapped again after an unmap: not anchored again)
    bool theater = false;         // ...a Steam game's: a View in Theater button where a floating window has "back to the desktop"
    bool steamDash = false;       // ...Steam's own dashboard window (vrsteamwin.cpp; its input there, its size fixed)
    // Glued flush left of this panel (Steam's menu to its page, steamglue.h): moved, shown and tiled
    // with it, no controls of its own, cropX..cropH the part of its window it shows. -1: not glued.
    int glueTo = -1;
    int gsX = 0, gsY = 0;         // ...where it is on the X screen (its popups go by it, PlaceSubs)
    std::string label;            // its name (MakePanel's: "Screen 1", "host: display", "Floating window 2")
    bool floatOn = false;         // ft-floatd has a window on it ("float" .. "unfloat")
    bool outputOn = false;        // KWin has the spare output turned on
    bool minimized = false;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int titleH = 0;
    double mpp = 0;               // metres per buffer pixel
    bool titleCarry = false;      // carried by its title bar: KWin's pointer stays at carryX, carryY
    double carryX = 0, carryY = 0;
    // A floating window carried onto a screen (UpdateDockAims): it docks there when let go, held at
    // dockGX, dockGY (its pixels) put at dockX, dockY (the screen's); -1: nowhere.
    int dockOn = -1;
    int dockX = 0, dockY = 0, dockGX = 0, dockGY = 0;
    // ...a PC window panel carried onto one of its PC's displays: it lands at dockU, dockV of the display
    // (0 to 1 across and down), held at dockFX, dockFY of the window.
    double dockU = 0.5, dockV = 0.5, dockFX = 0.5, dockFY = 0.5;
    int64_t pushedMs = -1;        // the wheel last pushed it away (ms): pushed into a screen, it lands at once (DockPushed)
    // Carried by "carry toggle" (CarryToggle, UpdateToggleCarry): the 3D mouse's ray holds carryGrab (a
    // point on it, panel-local) carryDist from the eye, facing the head, turned by carryTurn. carryMode 1
    // (Ctrl held): it moves in carryPlane's plane; 2 (Alt held): it turns about its centre, at carryAt.
    bool toggleCarry = false;
    bool layoutHeld = false;      // carried by its layout button: the room's panels go with it (LayoutGrabStart)
    int carryMode = 0;
    double carryDist = 0, carryAt[3] = {};
    double carryEye[3] = {};      // the sphere's centre (no modifier): the eyes when the carry or this mode began
    int carryAims = 0;            // "aimat"s sent while it waits for the cursor (carryWaitUntil)
    Mat carryGrab{}, carryTurn{}, carryPlane{};
    int64_t carryWaitUntil = 0;   // Tab brought it: it waits (till then) for the cursor to come to its centre
    // A resize by a corner or two fists only stretches the panel, to this many pixels at its density,
    // and the window gets them in one resize when it ends (user, 2026-10-27: "it doesn't actually update
    // the pixels ... until I release so we can have the animation be smooth"). Kept until the answer's
    // "float", or until the tick stretchUntil if none comes (kStretchAnswerTicks). 0: the crop's own size.
    // A PC window's panel the same, its stream's size for the crop (ResizeWindowPanel).
    int stretchW = 0, stretchH = 0;
    long stretchUntil = 0;
    // "hold" from ft-floatd, while KWin changes the window (its scale, its size): until holdUntilMs the
    // panel keeps its picture and crop (the compositor keeps new buffers back, ft_vr_screen_held), and a
    // "float" waits in held* to go up with the next buffer. holdCapMs: the latest it can last.
    int64_t holdUntilMs = 0, holdCapMs = 0;
    bool heldFloat = false;
    double heldMpp = 0;
    int heldX = 0, heldY = 0, heldW = 0, heldH = 0, heldTitle = 0;
    std::map<int, Sub> subs;
    uint64_t universe = 0;        // the SteamVR universe it was placed in (0: not known yet; universefix.h)
    // Attention (UpdateAttention): what ft_vr_screen_attention answers, and until when (ms)
    // it stays focused or in view after the last reason for it.
    ft_attention attention = FT_FOCUSED;
    int64_t inputMs = INT64_MIN / 2;  // the last pointer event on it or its controls
    int64_t useMs = INT64_MIN / 2;    // the last click, scroll or drag on it (not a pointer passing over: the front slot's keep)
    int64_t laserMs = INT64_MIN / 2;  // ...from a laser (SteamVR's: a controller's, the 3D mouse's, a hand ray's; FromLaser)
    uint32_t laserDevice = vr::k_unTrackedDeviceIndexInvalid;   // ...whose (for the log)
    // How far out and how big it was before a hand or "grab that" took it (RememberPlace): a throw,
    // flick or push sends it out that far, at that size (SendBack). Forgotten once it's put down out
    // past kPlacedFar (that's its distance now).
    bool hasBefore = false;
    double beforeDist = 0, beforeWidth = 0, beforeCurve = 0;
    int64_t handPointerMs = INT64_MIN / 2;  // a hand last moved the pointer on it (kHandPointerMs)
    int64_t focusUntil = 0, viewUntil = 0;
    double heightMetres() const {
        if (stretchW > 0) return metres * stretchH / stretchW;   // (a floating window or a PC window's panel)
        if ((floating || glueTo >= 0) && cropW > 0) return metres * cropH / cropW;
        return width > 0 ? metres * height / width : metres * 9 / 16;
    }
    // Buffer pixels from OpenVR's mouse position on the panel (its origin is bottom left).
    // A cropped panel too: SteamVR gives the position in the whole texture, not the crop.
    void ToBuffer(double mx, double my, double *x, double *y) const { *x = mx, *y = height - my; }
    std::array<vr::VROverlayHandle_t, kControls> Controls() const { return {bar, handle}; }
    std::array<vr::VROverlayHandle_t, kControls + 1> All() const { return {overlay, bar, handle}; }
    // One of KWin's outputs: a desktop screen (not a floating window's, a remote display or an app's).
    bool Desktop() const { return !floating && !remote && !gsapp; }
};
extern std::map<int, Screen> g_screens;

// A button pressed on a screen belongs to KWin until it comes up, wherever the laser is by
// then: a window move or a drag and drop can end between panels. SteamVR sends the release
// only to an overlay under the laser, so while the pressing laser is on none of our panels,
// an invisible catcher sits on it, at the distance where it last met one, and a release
// there goes to KWin at the pointer's last spot. While a button is held, the laser leaving
// a screen doesn't take KWin's pointer away either, as with a real mouse; crossing onto
// another screen still moves it there.
struct Press {
    uint32_t buttons = 0;                     // held, as bits (1 << (BTN_* - BTN_LEFT))
    vr::TrackedDeviceIndex_t device = kNone;  // the laser that pressed them
    int screen = -1;                          // where KWin's pointer is: the last screen the laser was on
    int origin = -1;                          // the screen the first button went down on
    int remoteOn = -1;                        // pressed on a remote screen: the remote screen nearest along its laser
    double x = 0, y = 0;                      // ...and where on it, in buffer pixels
    double distance = 1;                      // from the laser's start to the last panel it met
    long upAt = -1;                           // the pointer helper saw left come up: release it at this tick
    int64_t idleSince = -1;                   // the pressing controller has held nothing since (ms, ReleaseStuck)
    bool torn = false;                        // KWin has had the release: a window torn off its screen (TearOff)
    uint64_t serial = 0;                      // counts presses (a first button down): which press this is
};
extern Press g_press;

// ---------------------------------------------------------------- vr.cpp's, for the other parts

extern bool g_vr;
extern bool g_ctrl, g_alt, g_meta, g_shift;   // held on a keyboard (ft_vr_key)
extern bool g_manual;   // the hide key's switch
extern bool g_paused;   // Frametop paused for a VR game: everything hidden
int64_t NowMs();
bool DevicePose(vr::TrackedDeviceIndex_t dev, Mat *out);
vr::EVROverlayError PinShow(vr::VROverlayHandle_t overlay);
void SendRelay(const std::string &msg);
void AnnounceOverlay(const char *key);
void SendDashpatch(const std::string &msg);   // to ft-dashpatch (@ft_dashpatch)
bool MakePanel(Screen &s, const char *prefix, const char *label);
void SetAbsolute(Screen &s, const Mat &pose, uint64_t universe = 0);
void ApplyCurve(const Screen &s);
void CapCurve(Screen &s);
void KeepCorner(Screen &s, double oldW, double oldH);
void PlaceChrome(Screen &s);
void ApplyAlpha(const Screen &s);   // its overlays' alpha now (the bar lit while hovered, held or its menu's open)
void PlaceSubs(const Screen &s);
void EndDrag(Screen &s);
void EndDragsBy(vr::TrackedDeviceIndex_t dev);
void UpdateVisibility();
Mode EffectiveMode();
int ScreenIndexOf(const Screen &s);
Screen *Find(int one_based);
bool ScreenPose(const Screen &s, Mat *out);
void PinIn(Screen &s, vr::TrackedDeviceIndex_t dev, const Mat &rel, levelpin::Mode mode);
vr::EVROverlayError PinHide(vr::VROverlayHandle_t overlay);
void SetWidth(Screen &s, double metres);
int PictureW(const Screen &s);
int PictureH(const Screen &s);
void Stretch(Screen &s, int w, int h);
void SendStretch(Screen &s);
const dashanchor::Anchor &Dash();   // Steam's bar's frame, read each tick (dashboard pins ride on it)
Mat DashFrame();
Mat OnSurface(const Screen &s, double u, double v, double dz);   // a point on its surface (along the arc when curved)
double DashWidth();   // the bar's width in the room, metres (0: not known)
double DashHeight();   // ...its height
double SteamPageWidth();   // Steam's page over it, metres wide (0: not known)
uint32_t ButtonBit(uint32_t linuxButton);
uint32_t LinuxButton(uint32_t vrButton);
bool FromLaser(const vr::VREvent_t &ev);
int OurPointer(uint32_t d);
bool ModCarry(Screen &s, int index, vr::TrackedDeviceIndex_t dev, uint32_t button, double x, double y);
bool CarryScroll(const vr::VREvent_t &ev);
bool CarryClick(const vr::VREvent_t &ev);
void ReleaseAwayBy(vr::TrackedDeviceIndex_t dev, uint32_t vrButton, void (*handle)(const struct ft_event *, void *), void *data);
void FillRound(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, int r, const uint8_t c[4]);
double Apart(const Mat &a, const Mat &b);
double BarIn(const Screen &s);    // the bar row's line, metres in from the panel's edge
barside::UV BarSpot(const Screen &s, double along);   // a spot on the bar's line, `along` from its middle
void DoReset(int panel, const char *how);   // the layout reset (ft-layout reset), confirmed `how` (from panel's menu; -1 none)
bool ModeVisible();    // the screens show now (the mode, the manual toggle, not paused)
bool LasersByMode();   // controllers' lasers work our panels now (SCREEN_LASERS, not in a game or Gamepad mode)
extern double g_barPaddingX, g_barPaddingY;   // the grab bar's hit padding (GRAB_BAR_PADDING_X/Y, cm)

// ---------------------------------------------------------------- vrtiles.cpp's

// The DASH_TILE_* numbers in frametop.conf (read at start, and live from Frametop Settings'
// "handsetting"), with their ranges.
struct TileSetting {
    const char *name;
    double *value;
    double lo, hi;
};
std::array<TileSetting, 5> TileSettings();
bool SetTileSetting(const char *name, double value);   // false: not one, or out of its range
extern dashtiles::Memory g_tileMemory;   // each app's or display's last state (its key's)
void LoadTileMemory();
void SaveTileMemoryDue();   // on the tick: written once it's 2 s after a change
void FlushTileMemory();     // stopping: a change not written yet, written now
bool TileAnchored(int i);
bool TileTiled(int i);
void SetTileStayDefault(bool on);   // DASH_TILE_STAY: anchored panels stay shown with the dashboard closed (unless set per panel)
void SetTileStayTheater(bool on);   // DASH_TILE_STAY_THEATER: ...over a game in SteamVR's theater too
bool TileStays(const Screen &s, int i);   // shown with the dashboard closed
void SetTileAtBar(bool on);   // DASH_TILE_AT_BAR: the wall at Steam's bar (dashtiles.h's AtBar), else as set
void RetileDash();
void TileAnchor(int i, dashtiles::Stay stay, const char *by, int leftOf = -1);   // (leftOf tiled: a column just left of its)
void TileUnanchor(int i, const char *by);
void TileKnown(int i, const std::string &key);
void TileGone(int i);
bool TileShown(const Screen &s, int i, bool dashUp);
void TileDrop(Screen &s, int i);
void TileResized(Screen &s, int i);
void UpdateDashTiles();
void PlaceTileBorder();
void TileStay(int i, bool on, const char *by);   // anchored: shown with the dashboard closed too, or not
void TileCommand(vrcmd::Verb verb, vrcmd::Cmd &parsed, const char *cmd, char *reply, int size);

// ---------------------------------------------------------------- vrresetask.cpp's

void ShowResetAsk(int panel);   // "Reset the layout?": a click or a tap on it resets (DoReset)
void HideResetAsk();
void UpdateResetAsk();   // each tick: it follows its panel; a fingertip through it is the tap
void PollResetAsk(void (*handle)(const struct ft_event *, void *), void *data);

// ---------------------------------------------------------------- vrbarside.cpp's

// Each panel's bar edge (barside.h), remembered by its key (TileKnown's) in ~/.config/frametop-bars.txt.
void LoadBarSides();
void BarSideKnown(int i, const std::string &key);   // its key now: its remembered edge, if any
void BarSideGone(int i);
bool SetBarSide(int i, barside::Side side, const char *by);   // false: no such panel

// ---------------------------------------------------------------- vrbarmenu.cpp's

// Its menu (panelmenu.h): a tap on its bar opens the main page, a right click the edges, `along` metres
// from the bar's middle; again on the same bar, it closes.
void OpenBarMenu(int i, double along, bool edges);
void CloseBarMenu(const char *why);   // (if it's open)
bool BarMenuOn(int i);   // its menu is open (its controls stay)
// Hand input leaves the panel to its menu: while the menu's open, and after a fingertip pressed a row or
// closed it until that tip has come back out (so the same poke doesn't go on into the panel's content).
bool BarMenuHasHands(int i);
void UpdateBarMenu();    // each tick: it follows its panel; a fingertip through a row presses it
void PollBarMenu(void (*handle)(const struct ft_event *, void *), void *data);

// ---------------------------------------------------------------- vrgsapps.cpp's

void GsGeometry(Screen &s, int index, const gsforward::Window &w);
void GsStep();
void GsPanelEvents(Screen &s, int index);
void GsResize(int index, int w, int h);
void GsSwitch(bool on);
void GsShutdown();
bool GsTyping();
bool GsKey(uint32_t code, int value, bool elsewhere, gsforward::Mods now);
void GsBlur();
bool GsCanTheater(int index);         // a Steam game's panel, not lent: its grab bar menu offers View in Theater
void GsToTheater(int index);          // View in Theater: its window lent to SteamVR's theater (gstheater.h)
void GsTheaterWord(unsigned app);     // ft-dashpatch: the game SteamVR's theater shows (0: none)
bool GsInTheater();                   // SteamVR's theater shows a lent game (the tiles hide then)

// ---------------------------------------------------------------- vrsteamwin.cpp's

int SteamDashIndex();   // Steam's window's panel index while Steam's windows are ours, else -1
// The window pixels glued left of panel `index` at its density (Steam's menu, steamglue.h): its
// tile is that much wider. 0: none.
int GluedPx(int index);
void SteamDashStep();
void SteamDashEvents(Screen &s, int index);
void SteamDashSwitch(bool on);
// Steam's page in SteamVR's theater or not (ft-dashpatch's "steamvrmode", its fourth number).
void SteamDashTheater(bool on);
void SteamDashToTheater();   // Steam's page into SteamVR's theater (its tile's grab bar menu)
bool SteamDashInTheater();   // SteamVR's theater shows Steam's page (the tiles hide then)
void SteamDashShutdown();

// ---------------------------------------------------------------- vr.cpp's, for the bar's menu

void ToggleCurve(Screen &s);
void LevelScreen(Screen &s, int index, const char *by);
void StartDrag(Screen &s, Drag mode, vr::TrackedDeviceIndex_t dev);
bool RollClick(const Screen &s);   // a roll pressed and let go, not turned: a click (levels it)
void TogglePinView(Screen &s, int index, const char *by);
void LayoutGrabStart(Screen &s, int index, vr::TrackedDeviceIndex_t dev);
void SendFloat(const std::string &msg);   // to ft-floatd
bool HandsFrame();   // hand input had a new hands frame this tick
bool IndexTipIn(int side, const Mat &frame, double out[3]);   // that hand's index fingertip in `frame`'s coordinates, if it's seen
std::string DeviceLabel(vr::TrackedDeviceIndex_t dev);

// ---------------------------------------------------------------- vr.cpp's gamepad pill

// The panel Frametop's Gamepad mode drives now, -1 none. It stays shown while the dashboard is
// diminished (SteamVR 2.19), which otherwise hides the dashboard's panels.
int PadPanel();

}  // namespace ftvr

extern "C" int g_mouseScreen;                       // (vr.cpp's: the panel under the 3D mouse)
extern "C" vr::TrackedDeviceIndex_t g_mouseDevice;  // ...and its device
