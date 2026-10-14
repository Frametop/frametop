// The OpenVR side of ft-screens: one overlay per screen, client DMA-BUFs imported with
// IVRIPCResourceManagerClient::ImportDmabuf (no copy, no size limit), panel mouse events
// turned into ft_events for the compositor, and the panels' own handling:
//   - a grab bar under each screen: press it with any laser (a controller, or the 3D
//     mouse's virtual controller) and the screen follows that device rigidly until the
//     release, so the 3D mouse's tilt (right button while dragging) turns it; scrolling
//     while dragging pushes it away or pulls it closer (along the line from the head).
//   - a curve button next to the bar: bends the screen into a cylinder around you (its
//     radius: your distance to it when pressed), or flat again.
//   - a roll button next to that: drag it sideways like a knob to roll the screen about
//     its centre (it snaps level within kRollSnap), or scroll on it for kRollStep steps.
//   - a resize handle on a corner: drag it to resize from that corner, the opposite one staying
//     put (a screen's height follows its resolution; a floating window gets the pixels). It sits
//     on the bottom right and goes to whichever corner a laser, the gaze or a hand comes near
//     (UpdateCorner). It draws nothing since 2026-10-13: a soft white glow round the corner, as
//     a hand's, shows it, brighter as the aim comes, brightest while resizing.
//   - a reset button left of the bar: the profile in use opened again, or every screen back
//     in its layout, around where you are now (`ft-layout reset`, like Meta+Shift+R).
//   The controls are translucent, like SteamVR's own, and brighten under a laser. They
//   are invisible until a laser (a controller's, or the 3D mouse's) lands on or passes very close to
//   one of them (UpdateControls).
//   - pin to a wrist: while carrying a screen, sweep the laser (the line from the carrying
//     device to the bar) across your other controller. A ring around each controller
//     shows the target and a dot where the laser passes it; crossing the ring arms the pin
//     (ring and bar turn blue), crossing it again disarms it. Let go while armed and the
//     screen rides on that controller as it is then, at any size and distance, so you can
//     arm it and then turn it the way you want before letting go. Grabbing a pinned
//     screen keeps it armed for its wrist: move it, let go, and it's re-pinned there
//     (sweep across the ring to take it off). A pinned screen shows only while you see
//     its front, within the wrist angle (and fades out over the last kFade degrees).
//   - pin to your head (the pin command, from ft-layout and Frametop Display Settings): the
//     screen rides on the headset as it is then, like a HUD, and shows whenever the
//     screens do. Carrying it works like a wrist pin: let go and it's re-pinned to your
//     head where you put it; sweep across a wrist ring to move it to that wrist, or twice
//     to leave it in the room.
//   - visibility modes: always (the hide hotkey toggles), only with the SteamVR dashboard
//     open, while you look at a chosen controller (the wrist gesture), or toggle only
//     (hidden until the hotkey shows them).
//   - a screen hidden on its own ("conceal <screen>", from ft-layout and profiles) stays
//     hidden whatever the mode or the hotkey says, until "reveal <screen>". (Not "hide
//     <screen>": an older build reads anything starting with "hide" as the hotkey's hide.)
//   - controllers on the screens: while visible, the screens can keep SteamVR's laser mouse
//     on (VROverlayFlags_MakeOverlaysInteractiveIfVisible), so controllers use them with
//     the dashboard closed. That also takes the controllers away from a VR game, so by
//     default it's off while a game (a scene app) runs: the screens stay up over the game,
//     the controllers stay in it, and the 3D mouse (its own laser mode) or the dashboard
//     works the screens. Modes: always, outside_games (default), dashboard (never on its
//     own; also for flatscreen games, which aren't scene apps). Where the mode leaves the
//     controllers to the game, pointing a controller at a panel (a screen, a floating window
//     and its popups, their controls, the keyboard) turns the laser on for it until you
//     point away, like SteamVR's own floating windows (UpdateAim).
//   - during a VR game the screens hide unless the dashboard is open (g_inGames, default),
//     or stay visible over it; the hotkey still shows them.
//   - paused ("pause on", from the input relay when Frametop pauses for a VR game,
//     input/game_pause.py): every screen and floating window hides whatever the mode, the
//     hotkey, or the dashboard says, and compositor.c gives KWin a frame callback once a
//     second, as for any hidden screen, so KWin and its apps hardly draw. "pause off" undoes
//     it.
//   - hand cutouts (handcut.cpp): where ft-hands (hands/) tracks a hand between an eye and a
//     screen, that eye sees through the screen (to Room View). Only then is the screen
//     drawn by us, into a side-by-side buffer (one half per eye); otherwise its client
//     buffer is shown as is. The matte is a round hole per hand (or hand-shaped, or ft-hands'
//     capsules: HANDS_MATTE).
//     Only with Room View on (roomview.h): with it off a cutout would show the void or the game,
//     and ft-handview (handview.cpp) draws the hands instead.
//   - near hand input (handinput.cpp), within about 3 inches of a panel: poke a screen, remote
//     display or floating window with the index finger to click it; pinch its border to move it
//     (by any corner: resize), its content to click; hold its edge like a card (thumb behind, index
//     and middle in front) to carry and turn it rigidly; make a fist at it to carry and turn it;
//     hold it with both hands to stretch and turn it; flick it away, or let go of it moving out
//     fast, and it goes 3 m out (animated, as "grab that" moves). A second hand's pinch or fist
//     anywhere on a panel the other holds joins it (two anchors: move, scale, turn). A poke or a pinch
//     on a round button (curve, reset, dock, close) presses it. What a hand holds stays held
//     through a short tracking loss. A soft white glow shows the corner a pinch would resize from, the
//     edge it would move the panel by, or the button it would press (UpdateHandGuides, cutouts on or
//     off); with cutouts, a glow on the screen also shows where the fingertip is. It takes the pointer helper's word on which hands are bare and whether hand
//     input is allowed ("hands"), and which hands typing on a real keyboard holds back ("typing":
//     no pokes, grabs or "grab that" by them), and tells it which hands are near a panel ("near":
//     1 its gestures are ours, 2 only its ray hides), so a pinch is either ours or the helper's.
//     A hand at a panel owns the pointer there (kHandPointerMs). Its log lines start "hands:".
//   - the palm menu (palmmenu.cpp): a bare hand turned palm up toward your face, while you look
//     at it, gets a small ring above the palm; a pinch tap by that hand opens the Steam menu
//     (steam/ft-steam menu), or closes the dashboard. That hand counts as near meanwhile.
//   - "grab that" (grabthat.cpp): an open hand held out to a screen or floating window, then a
//     fist pulled back, brings it to arm's reach (about 45 cm ahead, facing you). Only a panel the
//     palm faces (its normal's line meets it), farther than 0.8 m, not held (near ones: push,
//     flick or throw them away). A soft white halo round it shows what it would take
//     (UpdateTargetGlow). The closing hand counts as near, and a fist or pinch it makes at a panel
//     waits on it while it aims at one it would take.
//   - the catcher: a button pressed on a screen is released in KWin even when the laser
//     lets go between panels (UpdateCatcher).
//   - floating windows (docs/floating-windows.md): KWin's spare outputs, after the screens,
//     are panels too, for one window each. ft-floatd sizes the output to the window plus a
//     margin and tells us the window's rectangle ("float"): the panel shows only that crop of
//     the buffer (SetOverlayTextureBounds), at the density of the screen it came from, and
//     each popup or dialog gets a small panel of its own over it, cut from the same buffer
//     ("sub"). Pressing its title bar carries the panel like the bar does, while KWin's
//     pointer stays put, so the window doesn't move on its output. The corner tab resizes the
//     window (in pixels, at the same density) instead of scaling the panel, and two more
//     buttons close it and put it back on the desktop (both through ft-floatd).
// OpenVR has no overlay-relative transforms here (openvr v2.15.6), so the bar, button,
// and handle are placed whenever their screen moves.
#include "vr.h"

#include "handcut.h"
#include "grabthat.h"
#include "handinput.h"
#include "softglow.h"
#include "palmmenu.h"
#include "keyboard.h"
#include "roomview.h"
#include "steampanels.h"

#include <drm_fourcc.h>
#include <openvr.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>  // environ (g++ defines _GNU_SOURCE), for posix_spawn

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Mat = vr::HmdMatrix34_t;
using Clock = std::chrono::steady_clock;

Mat Identity() {
    Mat m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    return m;
}
Mat Mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    }
    return r;
}
Mat Inverse(const Mat &a) {  // rigid: R^T, -R^T t
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}
Mat Translation(double x, double y, double z) {
    Mat m = Identity();
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}
double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Column(const Mat &m, int c, double out[3]) { out[0] = m.m[0][c], out[1] = m.m[1][c], out[2] = m.m[2][c]; }

// A panel pose from a centre and the direction its front is seen from (yaw, pitch; see
// layout: the front faces back along that direction), turned by roll.
Mat PanelPose(double x, double y, double z, double yawDeg, double pitchDeg, double rollDeg) {
    const double yw = yawDeg * M_PI / 180, pt = pitchDeg * M_PI / 180, rl = rollDeg * M_PI / 180;
    const double fx = -std::sin(yw) * std::cos(pt), fy = std::sin(pt), fz = -std::cos(yw) * std::cos(pt);
    const double Z[3] = {-fx, -fy, -fz};  // the front
    double X[3] = {Z[2], 0, -Z[0]};       // up x Z: horizontal right
    const double n = std::sqrt(X[0] * X[0] + X[2] * X[2]) + 1e-12;
    X[0] /= n, X[2] /= n;
    const double Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    const double c = std::cos(rl), s = std::sin(rl);
    Mat m{};
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = float(X[i] * c + Y[i] * s);
        m.m[i][1] = float(Y[i] * c - X[i] * s);
        m.m[i][2] = float(Z[i]);
    }
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}

// Device poses, read once per tick (ft_vr_poll) or per command.
vr::TrackedDevicePose_t g_poses[vr::k_unMaxTrackedDeviceCount];
void RefreshPoses() {
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, g_poses,
                                                    vr::k_unMaxTrackedDeviceCount);
}
bool DevicePose(vr::TrackedDeviceIndex_t dev, Mat *out) {
    if (dev >= vr::k_unMaxTrackedDeviceCount || !g_poses[dev].bPoseIsValid) return false;
    *out = g_poses[dev].mDeviceToAbsoluteTracking;
    return true;
}
// Where a device's laser starts and points: SteamVR's laser comes from its render model's
// "tip" component, not the device pose. On the Frame's controllers the tip points 40 degrees
// below the pose's -Z, so rays from the pose missed what the laser was on. Devices without
// a tip (the 3D mouse's virtual controller) aim along their pose. Cached per device; a
// model that isn't loaded yet is asked again a few seconds later.
struct Tip {
    std::string model;
    Mat offset = Identity();
    bool found = false;
    Clock::time_point checked;
};
Mat TipOffset(vr::TrackedDeviceIndex_t dev) {
    static std::map<vr::TrackedDeviceIndex_t, Tip> cache;
    char model[256] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(dev, vr::Prop_RenderModelName_String, model, sizeof model);
    const auto now = Clock::now();
    auto it = cache.find(dev);
    if (it != cache.end() && it->second.model == model &&
        (it->second.found || now - it->second.checked < std::chrono::seconds(5)))
        return it->second.offset;
    Tip tip{model, Identity(), false, now};
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    // GetComponentState, not GetComponentStateForDevicePath: without an input source handle
    // the latter fails for every component while a VR game runs, and the rays came from the
    // pose, 40 degrees above the laser. The tip doesn't move with the buttons.
    vr::VRControllerState_t buttons{};
    if (model[0] && vr::VRRenderModels()->GetComponentState(model, vr::k_pch_Controller_Component_Tip, &buttons, &mode,
                                                            &state))
        tip.offset = state.mTrackingToComponentLocal, tip.found = true;
    cache[dev] = tip;
    return tip.offset;
}
bool LaserPose(vr::TrackedDeviceIndex_t dev, Mat *out) {
    Mat d;
    if (!DevicePose(dev, &d)) return false;
    *out = Mul(d, TipOffset(dev));
    return true;
}

bool IsHandController(vr::TrackedDeviceIndex_t i) {
    if (i >= vr::k_unMaxTrackedDeviceCount) return false;   // not a device (a hand's holder, kHandHolder)
    if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) return false;
    char type[64] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
    return std::strcmp(type, "ft_pointer") != 0;  // not the 3D mouse's virtual controller
}
// "left", "right", or "head" (the headset) -> the device to pin to.
vr::TrackedDeviceIndex_t HandDevice(const char *hand) {
    if (std::strcmp(hand, "head") == 0) return vr::k_unTrackedDeviceIndex_Hmd;
    return vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
        std::strcmp(hand, "right") == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
}
const char *HandName(vr::TrackedDeviceIndex_t i) {
    if (i == vr::k_unTrackedDeviceIndex_Hmd) return "head";
    switch (vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i)) {
        case vr::TrackedControllerRole_LeftHand: return "left";
        case vr::TrackedControllerRole_RightHand: return "right";
        default: return "none";
    }
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
constexpr double kRollStep = 5;      // degrees per scroll notch on the roll button
constexpr float kChromeIdle = 0.55f; // the controls' opacity without a laser on them
constexpr long kControlsLinger = 35; // ticks (~0.4 s) the controls stay after a laser leaves
constexpr long kAimLinger = 25;      // ticks (~0.3 s) a panel keeps the laser on after the aim leaves it
long g_tick = 0;                     // ft_vr_poll calls
bool g_vr = false;                   // connected to SteamVR (ft-screens --no-vr runs without it)
constexpr vr::TrackedDeviceIndex_t kNone = vr::k_unTrackedDeviceIndexInvalid;
// Near hand input (handinput.cpp) holds KWin's button and carries panels as these, in place of
// a device: no device has them, so DevicePose and LaserPose fail for them (no laser to follow or
// catch), and a laser's release never ends what a hand holds.
constexpr vr::TrackedDeviceIndex_t kHandHolder[2] = {vr::k_unMaxTrackedDeviceCount + 1, vr::k_unMaxTrackedDeviceCount + 2};
bool IsHandHolder(vr::TrackedDeviceIndex_t d) { return d == kHandHolder[0] || d == kHandHolder[1]; }
// A hand at a panel owns the pointer (live test 2026-10-10): while a hand holds the button, SteamVR's
// laser moves (the pointer helper's head or gaze pointer, a controller) go nowhere, and on a panel
// whose pointer a fingertip moved within this long, neither do they nor its leaving. Else the
// helper's pointer, a few cm off the fingertip, pulled KWin's or the remote host's pointer away
// between the hand's press and release: a drag, not a click (and on a remote display the host's
// pointer jumped between the two as the finger hovered).
constexpr int64_t kHandPointerMs = 250;

// A popup or dialog of a floating window: a small panel over it, cut from the same buffer.
struct Sub {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int x = 0, y = 0, w = 0, h = 0;  // in the output's buffer, pixels
};

struct Screen {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid, bar = vr::k_ulOverlayHandleInvalid,
                          handle = vr::k_ulOverlayHandleInvalid, curveButton = vr::k_ulOverlayHandleInvalid,
                          rollButton = vr::k_ulOverlayHandleInvalid, dockButton = vr::k_ulOverlayHandleInvalid,
                          closeButton = vr::k_ulOverlayHandleInvalid,  // the last two: floating windows
                          resetButton = vr::k_ulOverlayHandleInvalid;  // desktop screens only
    int width = 0, height = 0;    // current buffer size (mouse scale)
    double metres = 1;
    double curve = 0;             // cylinder radius in metres; 0 = flat
    const void *shown = nullptr;  // a frame arrived
    bool visible = false;         // shown in VR right now
    bool alone = false;           // hidden on its own (conceal <screen>), whatever the mode
    float alpha = 1;
    vr::TrackedDeviceIndex_t pinned = kNone;  // riding on this controller
    Mat pinRel = Identity();                  // controller -> screen
    Mat pose = Identity();                    // where it is in the room, when not pinned
    Drag drag = Drag::None;
    vr::TrackedDeviceIndex_t dragDevice = kNone;
    Mat dragRel = Identity();                 // device -> screen, while moving
    double grabX = 0, grabY = 0;              // resize: the grab point relative to the corner
    bool dragFacing = false;                  // move: kept facing the head (a hand ray carries it: HandRay)
    // The resize handle (Controls()[3], "<prefix>.resize"): one overlay, at the corner a laser, the gaze
    // or a hand aims at (UpdateCorner; the bottom right otherwise), brighter the closer the aim, lit
    // while resizing. Any corner resizes, the opposite one staying put.
    int cornerX = 1, cornerY = -1;            // +1 right, top
    float cornerGlow = 0;                     // the aim's closeness, 0..1
    bool cornerLit = false;                   // its texture lit (resizing)
    long cornerUntil = 0;                     // aimed at until this tick (then back to the bottom right)
    int keepX = -1, keepY = 1;                // a floating window's corner that stays put as it changes size
    long keepUntil = 0;                       // ...after a resize from another corner ends, until this tick (late answers)
    Mat rollFrom = Identity();                // roll: the pose at the press (pinRel when pinned)
    double rollAngle = 0;                     // roll: the laser's angle around the centre then
    bool hover[7] = {};                       // a laser is on the bar, curve, roll, resize, dock, close, reset control
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
    const void *key = nullptr;    // the client buffer on it now, and its dmabuf (for cutouts)
    ft_dmabuf buf{};
    vr::SharedTextureHandle_t plain = 0;  // that buffer's SteamVR import
    uint64_t frames = 0;          // client frames presented (a cutout buffer's "serial")
    bool cutting = false;         // showing a cutout buffer (side by side) instead
    vr::SharedTextureHandle_t cutShown = 0;  // ...this one
    double chrome = 0.3;          // the bar's width; the other controls follow it (ChromeSize)
    double grip = 0.04;           // the corner tab's and the round buttons' size
    // A floating window's panel (see the top): the window's rectangle in the buffer, its
    // title bar's height there, and the density.
    bool floating = false;        // a spare output's panel
    int slot = 0;                 // ...its number, from 1 (its overlays exist only while a window floats)
    bool remote = false;          // a remote display: another machine's monitor, streamed (remote.c)
    std::string label;            // its name (MakePanel's: "Screen 1", "host: display", "Floating window 2")
    bool floatOn = false;         // ft-floatd has a window on it ("float" .. "unfloat")
    bool outputOn = false;        // KWin has the spare output turned on
    bool minimized = false;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int titleH = 0;
    double mpp = 0;               // metres per buffer pixel
    bool titleCarry = false;      // carried by its title bar: KWin's pointer stays at carryX, carryY
    double carryX = 0, carryY = 0;
    long resizeSent = 0;          // g_tick of the last resize request (they're throttled)
    int resizeW = 0, resizeH = 0; // ...and its size
    std::map<int, Sub> subs;
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
        if (floating && cropW > 0) return metres * cropH / cropW;
        return width > 0 ? metres * height / width : metres * 9 / 16;
    }
    // Buffer pixels from OpenVR's mouse position on the panel (its origin is bottom left).
    // A cropped panel too: SteamVR gives the position in the whole texture, not the crop.
    void ToBuffer(double mx, double my, double *x, double *y) const { *x = mx, *y = height - my; }
    std::array<vr::VROverlayHandle_t, 7> Controls() const {
        return {bar, curveButton, rollButton, handle, dockButton, closeButton, resetButton};
    }
    std::array<vr::VROverlayHandle_t, 8> All() const {
        return {overlay, bar, curveButton, rollButton, handle, dockButton, closeButton, resetButton};
    }
};
std::map<int, Screen> g_screens;

// For the log and the debug line (scripts/report.sh): which screen, which drag, which laser.
int ScreenIndexOf(const Screen &s) {
    for (const auto &[index, t] : g_screens)
        if (&t == &s) return index;
    return -1;
}
const char *DragName(Drag d) {
    switch (d) {
        case Drag::Move: return "move";
        case Drag::Resize: return "resize";
        case Drag::Roll: return "roll";
        default: return "none";
    }
}
std::string DeviceLabel(vr::TrackedDeviceIndex_t i) {
    if (i == kNone) return "none";
    if (vr::VRSystem()->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller && !IsHandController(i))
        return "3D mouse";
    const std::string hand = HandName(i);
    return hand == "none" ? "device " + std::to_string(i) : hand + " controller";
}
std::map<const void *, vr::SharedTextureHandle_t> g_imports;

// Hand cutouts (see the top and handcut.h).
bool g_cutouts = true;          // the cutouts command turns them off
handcut::Hands g_hands;
handcut::MatteParams g_matte;   // HANDS_MATTE* settings, the cutouts command
handcut::Matte g_matteNow;      // this tick's
bool g_handsNow = false;        // g_hands has fresh hands this tick
handcut::Renderer g_cutter;
int g_cutterState = 0;          // 0 not tried, 1 ready, -1 unavailable
std::map<const void *, vr::SharedTextureHandle_t> g_cutImports;
roomview::Detector g_roomView;   // no cutouts with Room View off
handinput::HandInput g_handInput;   // near hand input (handinput.h; UpdateHandInput)
grabthat::Detector g_grabThat;      // "grab that" (grabthat.h; UpdateGrabThat)
bool g_constructRoomView = true; // camera/enableConstructRoomView

// Visibility (see the top). g_manual is the hide/show switch: in the always mode it hides
// the screens, in the others it shows them anyway.
Mode g_mode = Mode::Always;
bool g_manual = false;
double g_wristAngle = 60;    // a pinned screen shows while you see its front within this
double g_gestureAngle = 20;  // gesture: look within this of the controller
std::string g_gestureHand = "left";
Lasers g_lasers = Lasers::OutsideGames;  // when controllers' lasers work the screens (see the top)
bool g_gameRunning = false;              // a scene app (VR game) is running
bool g_paused = false;                   // Frametop paused for a VR game: everything hidden (see the top)
InGames g_inGames = InGames::Hide;       // during a VR game, the always mode acts like the dashboard mode

// ---------------------------------------------------------------- chrome (bar, button, handle)

// The controls look like SteamVR's own: a light translucent pill for the bar, dark
// translucent discs with white glyphs for the buttons (the overlay alpha, kChromeIdle,
// dims them further until a laser is on them).
std::vector<uint8_t> PillTexture(int w, int h, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    const double r = h / 2.0 - 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double cx = std::clamp(double(x), r + 1, w - r - 1), cy = h / 2.0;
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = red, p[1] = green, p[2] = blue;
            p[3] = uint8_t(std::clamp(r - d + 0.5, 0.0, 1.0) * alpha);
        }
    return px;
}
const std::vector<uint8_t> &BarTexture(bool lit) {
    static const auto normal = PillTexture(256, 24, 235, 235, 235, 210), glow = PillTexture(256, 24, 90, 170, 255, 240);
    return lit ? glow : normal;
}

// Paint a control: dark translucent inside `inside(u, v)`, white where `glyph(u, v)`, a
// faint light rim where `rim(u, v)`. u, v: -1..1 across the texture, v up.
template <typename In, typename Glyph, typename Rim>
std::vector<uint8_t> ControlTexture(int n, In inside, Glyph glyph, Rim rim) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    const int ss = 3;  // supersampling, for smooth edges
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double in = 0, g = 0, e = 0;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const double u = (x + (i + 0.5) / ss) / n * 2 - 1, v = 1 - (y + (j + 0.5) / ss) / n * 2;
                    if (!inside(u, v)) continue;
                    in += 1;
                    if (glyph(u, v)) g += 1;
                    else if (rim(u, v)) e += 1;
                }
            const double k = ss * ss;
            in /= k, g /= k, e /= k;
            uint8_t *p = &px[(size_t(y) * n + x) * 4];
            const double bg = in - g - e;  // dark part
            const double a = bg * 0.72 + e * 0.6 + g * 1.0;
            if (a <= 0) continue;
            const double shade = (bg * 0.72 * 38 + e * 0.6 * 200 + g * 255) / a;
            p[0] = p[1] = p[2] = uint8_t(std::clamp(shade, 0.0, 255.0));
            p[3] = uint8_t(std::clamp(a * 255, 0.0, 255.0));
        }
    return px;
}
bool InDisc(double u, double v) { return u * u + v * v <= 1; }
bool DiscRim(double u, double v) { return u * u + v * v > 0.86 * 0.86; }

// The resize handle draws nothing (user, 2026-10-13: "remove the radio icon thing on corners for resizing
// and replace them with the glow that is used for grabbing with hand tracking"): it stays as the laser's
// hit area just outside the corner (its mouse scale, not its texture, sizes that), and the corner's soft
// glow (UpdateHandGuides: the laser's guide, as a hand's) shows it.
const std::vector<uint8_t> &HandleTexture(bool) {
    static const std::vector<uint8_t> clear(64 * 64 * 4, 0);
    return clear;
}

std::vector<uint8_t> CurveTexture(int n) {
    // An arc: "curve this screen".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) { return std::fabs(std::hypot(u, -v - 1.9) - 1.7) < 0.11 && std::fabs(u) < 0.6; },
        DiscRim);
}

std::vector<uint8_t> RollTexture(int n) {
    // A circular arrow, counterclockwise: "roll this screen".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            const double r = std::hypot(u, v);
            double ang = std::atan2(v, u) * 180 / M_PI;
            if (ang < 0) ang += 360;
            if (std::fabs(r - 0.48) < 0.085 && ang >= 100) return true;  // the arc, 100..360 degrees
            // The head at 0 degrees, pointing up (the way the arc turns there).
            const double hx = u - 0.48, hy = v + 0.02;
            return hy >= 0 && hy <= 0.3 && std::fabs(hx) <= 0.24 * (1 - hy / 0.3);
        },
        DiscRim);
}

std::vector<uint8_t> CloseTexture(int n) {
    // A cross: "close this window".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            return std::max(std::fabs(u), std::fabs(v)) < 0.42 &&
                   (std::fabs(u - v) < 0.12 || std::fabs(u + v) < 0.12);
        },
        DiscRim);
}

std::vector<uint8_t> DockTexture(int n) {
    // An arrow down onto a line: "back to the desktop".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            if (std::fabs(u) < 0.5 && v > -0.52 && v < -0.38) return true;  // the line
            if (std::fabs(u) < 0.08 && v > -0.1 && v < 0.5) return true;    // the shaft
            return v >= -0.3 && v <= -0.05 && std::fabs(u) <= (v + 0.3) * 1.2;  // the head, point down
        },
        DiscRim);
}

std::vector<uint8_t> ResetTexture(int n) {
    // A reticle: "put the screens back around you" (like a recenter).
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            const double r = std::hypot(u, v);
            if (std::fabs(r - 0.4) < 0.07 || r < 0.13) return true;  // the ring and the centre
            return (std::fabs(u) < 0.06 && std::fabs(v) > 0.47 && std::fabs(v) < 0.72) ||
                   (std::fabs(v) < 0.06 && std::fabs(u) > 0.47 && std::fabs(u) < 0.72);  // the ticks
        },
        DiscRim);
}

vr::VROverlayHandle_t MakeChrome(const char *key, const char *name, const std::vector<uint8_t> &px, int w, int h) {
    vr::VROverlayHandle_t o = vr::k_ulOverlayHandleInvalid;
    if (const auto err = vr::VROverlay()->CreateOverlay(key, name, &o); err != vr::VROverlayError_None) {
        std::fprintf(stderr, "openvr: can't create overlay %s: %s\n", key, vr::VROverlay()->GetOverlayErrorNameFromEnum(err));
        return o;
    }
    vr::VROverlay()->SetOverlayRaw(o, const_cast<uint8_t *>(px.data()), uint32_t(w), uint32_t(h), 4);
    vr::VROverlay()->SetOverlayInputMethod(o, vr::VROverlayInputMethod_Mouse);
    // SteamVR's laser and ComputeOverlayIntersection size the hit area from the mouse scale,
    // not the texture: at the default 1x1 it's a square as tall as the control is wide, so the
    // grab bar (256x24) caught clicks about a bar's half-width up into the screen above it.
    const vr::HmdVector2_t scale = {{float(w), float(h)}};
    vr::VROverlay()->SetOverlayMouseScale(o, &scale);
    vr::VROverlay()->SetOverlaySortOrder(o, 10);
    return o;
}

void LightBar(Screen &s, bool lit) {
    if (s.barLit == lit) return;
    s.barLit = lit;
    const auto &px = BarTexture(lit);
    vr::VROverlay()->SetOverlayRaw(s.bar, const_cast<uint8_t *>(px.data()), 256, 24, 4);
}

// ---------------------------------------------------------------- wrist guides

// While a screen is carried, each other controller gets a ring (its wrist zone, facing
// you) and a dot where the laser passes closest to it. Blue: armed / in the ring.
std::vector<uint8_t> DiscTexture(int n, double stroke, uint8_t red, uint8_t green, uint8_t blue, uint8_t fill,
                                 uint8_t rimShade) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    const double c = n / 2.0, r = n / 2.0 - 1;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
            const double a = std::clamp(r - d + 0.5, 0.0, 1.0);
            uint8_t *p = &px[(size_t(y) * n + x) * 4];
            const bool rim = d > r - stroke;
            const bool edge = d > r - 2 || (rim && d < r - stroke + 2);  // a dark line each side of the rim
            p[0] = edge ? rimShade : red, p[1] = edge ? rimShade : green, p[2] = edge ? rimShade : blue;
            p[3] = uint8_t(a * (rim ? 235 : fill));
        }
    return px;
}
const std::vector<uint8_t> &RingTexture(bool lit) {
    static const auto normal = DiscTexture(128, 9, 240, 240, 240, 40, 60),
                      glow = DiscTexture(128, 12, 90, 170, 255, 110, 30);
    return lit ? glow : normal;
}
const std::vector<uint8_t> &DotTexture(bool lit) {
    static const auto normal = DiscTexture(32, 16, 250, 250, 250, 250, 50),
                      glow = DiscTexture(32, 16, 90, 170, 255, 250, 30);
    return lit ? glow : normal;
}

struct GuidePart {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int lit = -1;  // the texture on it (-1: none yet)
    bool shown = false;
    void Show(bool on) {
        if (on == shown || overlay == vr::k_ulOverlayHandleInvalid) return;
        shown = on;
        if (on) vr::VROverlay()->ShowOverlay(overlay);
        else vr::VROverlay()->HideOverlay(overlay);
    }
    void Light(bool on, const std::vector<uint8_t> &px, int n) {
        if (int(on) == lit) return;
        lit = on;
        vr::VROverlay()->SetOverlayRaw(overlay, const_cast<uint8_t *>(px.data()), uint32_t(n), uint32_t(n), 4);
    }
};
struct Guide { GuidePart ring, dot; };
std::map<vr::TrackedDeviceIndex_t, Guide> g_guides;

Guide &GuideFor(vr::TrackedDeviceIndex_t dev) {
    auto it = g_guides.find(dev);
    if (it != g_guides.end()) return it->second;
    Guide &g = g_guides[dev];
    char key[64];
    std::snprintf(key, sizeof key, "frametop.guide.%u.ring", dev);
    if (vr::VROverlay()->CreateOverlay(key, "Wrist pin target", &g.ring.overlay) == vr::VROverlayError_None) {
        vr::VROverlay()->SetOverlayWidthInMeters(g.ring.overlay, float(2 * kWristZone));
        vr::VROverlay()->SetOverlaySortOrder(g.ring.overlay, 20);
    }
    std::snprintf(key, sizeof key, "frametop.guide.%u.dot", dev);
    if (vr::VROverlay()->CreateOverlay(key, "Wrist pin laser", &g.dot.overlay) == vr::VROverlayError_None) {
        vr::VROverlay()->SetOverlayWidthInMeters(g.dot.overlay, 0.022f);
        vr::VROverlay()->SetOverlaySortOrder(g.dot.overlay, 21);
    }
    return g;
}

// A pose at pt facing the head (upright).
Mat FacingPose(const double pt[3], const Mat &head) {
    double z[3] = {head.m[0][3] - pt[0], head.m[1][3] - pt[1], head.m[2][3] - pt[2]};
    const double zl = std::sqrt(Dot3(z, z)) + 1e-9;
    for (double &v : z) v /= zl;
    double x[3] = {z[2], 0, -z[0]};  // up x z
    const double xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = float(y[i]), m.m[i][2] = float(z[i]), m.m[i][3] = float(pt[i]);
    return m;
}

void ApplyCurve(const Screen &s) {
    // OpenVR's curvature: the fraction of a full cylinder the overlay's width covers.
    const double c = s.curve > 0 ? std::clamp(s.metres / (2 * M_PI * s.curve), 0.0, 1.0) : 0.0;
    vr::VROverlay()->SetOverlayCurvature(s.overlay, float(c));
}

// The screen's pose in the room (a pinned one: its controller's pose times pinRel).
bool ScreenPose(const Screen &s, Mat *out) {
    if (s.pinned != kNone) {
        Mat d;
        if (!DevicePose(s.pinned, &d)) return false;
        *out = Mul(d, s.pinRel);
        return true;
    }
    // Our own copy: reading it back from SteamVR right after setting it could return the
    // old pose, which left a moved screen's controls behind.
    *out = s.pose;
    return true;
}

// The controls' size from both the screen's width and its distance from the head (the
// geometric mean of 12% of the width and 10% of the distance), so a small screen near you
// gets small controls and a big or far one gets big ones, never under about 1.7 degrees.
double g_handButton = 0.024;   // HANDS_BUTTON_SIZE: a hand at a panel makes its round buttons at least this wide (m)

void ChromeSize(Screen &s) {
    Mat head, p;
    double dist = 2;
    if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) && ScreenPose(s, &p)) {
        const double d[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
        dist = std::max(0.2, std::sqrt(Dot3(d, d)));
    }
    const double least = dist * 0.03;
    s.chrome = std::clamp(std::sqrt(0.012 * dist * s.metres), least, std::max(least, s.metres * 0.5));
    s.grip = std::max(s.chrome * 0.13, dist * 0.018);
    // a hand at the panel: its round buttons at least handButton across (user, 2026-10-11: the bar's
    // buttons were hard to hit with a finger right after moving a display; about 1 cm at arm's reach)
    if (s.handNear) s.grip = std::max(s.grip, g_handButton);
}

// A point on the screen's surface, u metres along it from the centre (along the arc when
// curved), v up, dz out of it, facing the way the surface does there. OpenVR curves a
// screen into a cylinder toward its front, with its centre line where the flat one was.
Mat OnSurface(const Screen &s, double u, double v, double dz) {
    if (s.curve <= 0) return Translation(u, v, dz);
    const double r = s.curve, a = u / r, c = std::cos(a), sn = std::sin(a);
    Mat m = Identity();
    m.m[0][0] = float(c), m.m[0][2] = float(-sn);
    m.m[2][0] = float(sn), m.m[2][2] = float(c);
    m.m[0][3] = float(r * sn - dz * sn), m.m[1][3] = float(v), m.m[2][3] = float(r - r * c + dz * c);
    return m;
}

// (the row's middle: under the screen by a gap and the taller of the bar's half and a button's, so grown
// buttons (a hand near: ChromeSize) don't overlap the screen; user, 2026-10-11)
// The resize handle's side: wider than a button (user, 2026-10-13: "make the glow area a little bit wider
// off the edge of the displays so it's easier to trigger"), all of it outside the corner. The laser's
// corner glow is as wide (UpdateHandGuides).
double HandleSize(const Screen &s) { return s.grip * 1.8; }
double BarY(const Screen &s) { return -(s.heightMetres() / 2 + s.chrome * 0.06 + std::max(s.chrome * 12 / 256, s.grip / 2)); }
Mat BarOffset(const Screen &s) { return OnSurface(s, 0, BarY(s), 0.003); }

// Put the bar, the curve button, and the corner tab under the screen (same parent: the
// room or the controller), sized for the screen and its distance, and on its surface.
// Where each control sits, relative to the screen: bar, curve, roll, resize tab, a
// floating window's dock and close buttons (left of the bar), and a desktop screen's reset
// button (left of the bar, where a floating window has its dock button).
// Their centres on the surface: u along it, v up, metres from the screen's centre.
//   Between the bar and a button, and button and button, a button's width or more (user, 2026-10-13:
// "farther apart so it's harder to accidentally click"; was 6% of the bar's width, about a third of a
// button). It grows with them as a hand comes (ChromeSize).
void ControlUV(const Screen &s, double u[7], double v[7]) {
    const double h = s.heightMetres(), bar = s.chrome, button = s.grip, gap = std::max(bar * 0.06, button);
    const double uu[7] = {0, bar / 2 + gap + button / 2, bar / 2 + gap * 2 + button * 1.5,
                          // The handle's inner corner is the screen's corner it's at (bottom right:
                          // its top left corner on the screen's bottom right corner).
                          s.cornerX * (s.metres / 2 + HandleSize(s) / 2), -(bar / 2 + gap + button / 2), -(bar / 2 + gap * 2 + button * 1.5),
                          -(bar / 2 + gap + button / 2)};
    for (int k = 0; k < 7; ++k) u[k] = uu[k], v[k] = k == 3 ? s.cornerY * (h / 2 + HandleSize(s) / 2) : BarY(s);
}
Mat RollZ(double rad);
std::array<Mat, 7> ControlOffsets(const Screen &s) {
    double u[7], v[7];
    ControlUV(s, u, v);
    std::array<Mat, 7> m;
    for (int k = 0; k < 7; ++k) m[k] = OnSurface(s, u[k], v[k], 0.003);
    // the handle's texture is for the bottom right corner: turned for the others
    const double turn = s.cornerX > 0 ? (s.cornerY < 0 ? 0 : M_PI / 2) : (s.cornerY < 0 ? -M_PI / 2 : M_PI);
    if (turn != 0) m[3] = Mul(m[3], RollZ(turn));
    return m;
}

// A floating window's popups and dialogs, a few millimetres in front of it, where they are
// in the buffer relative to the window.
void PlaceSubs(const Screen &s) {
    if (s.subs.empty() || s.cropW <= 0) return;
    Mat p;
    if (s.pinned == kNone && !ScreenPose(s, &p)) return;
    for (const auto &[k, sub] : s.subs) {
        const double u = (sub.x + sub.w / 2.0 - (s.cropX + s.cropW / 2.0)) * s.mpp;
        const double v = -(sub.y + sub.h / 2.0 - (s.cropY + s.cropH / 2.0)) * s.mpp;
        const Mat off = OnSurface(s, u, v, 0.005);
        vr::VROverlay()->SetOverlayWidthInMeters(sub.overlay, float(std::max(0.01, sub.w * s.mpp)));
        if (s.pinned != kNone) {
            const Mat m = Mul(s.pinRel, off);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(sub.overlay, s.pinned, &m);
        } else {
            const Mat m = Mul(p, off);
            vr::VROverlay()->SetOverlayTransformAbsolute(sub.overlay, vr::TrackingUniverseStanding, &m);
        }
    }
}

void PlaceChrome(Screen &s) {
    ChromeSize(s);
    const double bar = s.chrome, button = s.grip;
    const auto offsets = ControlOffsets(s);
    vr::VROverlay()->SetOverlayWidthInMeters(s.bar, float(bar));
    vr::VROverlay()->SetOverlayWidthInMeters(s.curveButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.rollButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.handle, float(HandleSize(s)));
    if (s.floating) {
        vr::VROverlay()->SetOverlayWidthInMeters(s.dockButton, float(button));
        vr::VROverlay()->SetOverlayWidthInMeters(s.closeButton, float(button));
    } else {
        vr::VROverlay()->SetOverlayWidthInMeters(s.resetButton, float(button));
    }
    // Curved, the bar bends with the screen's bottom edge.
    vr::VROverlay()->SetOverlayCurvature(s.bar, s.curve > 0 ? float(std::min(1.0, bar / (2 * M_PI * s.curve))) : 0.f);
    const std::pair<vr::VROverlayHandle_t, Mat> parts[] = {
        {s.bar, offsets[0]},        {s.curveButton, offsets[1]}, {s.rollButton, offsets[2]},
        {s.handle, offsets[3]},     {s.dockButton, offsets[4]},  {s.closeButton, offsets[5]},
        {s.resetButton, offsets[6]}};
    PlaceSubs(s);
    if (s.pinned != kNone) {
        for (const auto &[o, off] : parts) {
            if (o == vr::k_ulOverlayHandleInvalid) continue;
            const Mat m = Mul(s.pinRel, off);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(o, s.pinned, &m);
        }
        return;
    }
    Mat p;
    if (!ScreenPose(s, &p)) return;
    for (const auto &[o, off] : parts) {
        if (o == vr::k_ulOverlayHandleInvalid) continue;
        const Mat m = Mul(p, off);
        vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &m);
    }
}

// Screens you walk up to (or pinned ones you bring close) get their controls resized now
// and then, not every frame.
void RefreshChrome() {
    static int tick = 0;
    if (++tick % 45) return;
    for (auto &[i, s] : g_screens) {
        if (s.drag != Drag::None) continue;
        const double before = s.chrome;
        ChromeSize(s);
        if (std::fabs(s.chrome - before) > before * 0.08) PlaceChrome(s);
        else s.chrome = before;
    }
}

void SetAbsolute(Screen &s, const Mat &pose) {
    s.pinned = kNone;
    s.pose = pose;
    vr::VROverlay()->SetOverlayTransformAbsolute(s.overlay, vr::TrackingUniverseStanding, &pose);
    PlaceChrome(s);
}

void Pin(Screen &s, vr::TrackedDeviceIndex_t dev, const Mat &rel) {
    s.pinned = dev;
    s.pinRel = rel;
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(s.overlay, dev, &s.pinRel);
    PlaceChrome(s);
}

// A curved screen bends at most this far round (radians, SCREEN_CURVE_MAX_DEG): its radius grows to keep
// a wide one under it (user, 2026-10-11: a curved screen far away "curves way too much, sometimes a whole
// 360: a cylinder floating in space": the radius was the head's distance when the curve was turned on,
// and a screen curved up close, then thrown 3 m out at full width, wrapped round).
double g_curveMaxArc = 100 * M_PI / 180;
double g_wideCurveMaxArc = 150 * M_PI / 180;   // an ultrawide's (grabthat::kWideAspect): SCREEN_SPIN_MAX_WIDE_DEG, so it fits round you
void CapCurve(Screen &s) {
    const double pxW = s.floating ? s.cropW : s.width, pxH = s.floating ? s.cropH : s.height;
    const double cap = pxH > 0 && pxW / pxH >= grabthat::kWideAspect ? std::max(g_curveMaxArc, g_wideCurveMaxArc) : g_curveMaxArc;
    if (s.curve > 0 && s.metres / s.curve > cap) s.curve = s.metres / cap;
}

void SetWidth(Screen &s, double metres) {
    s.metres = std::clamp(metres, kMinWidth, 12.0);
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    CapCurve(s);
    ApplyCurve(s);  // same radius, so the curvature fraction changes with the width
    PlaceChrome(s);
}

// Curve toward the head: the radius is the head's distance to the screen now.
void ToggleCurve(Screen &s) {
    Mat head, p;
    if (s.curve > 0 || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) || !ScreenPose(s, &p)) {
        s.curve = 0;
    } else {
        const double dx = p.m[0][3] - head.m[0][3], dy = p.m[1][3] - head.m[1][3], dz = p.m[2][3] - head.m[2][3];
        s.curve = std::max(0.5, std::sqrt(dx * dx + dy * dy + dz * dz));
        CapCurve(s);
    }
    ApplyCurve(s);
    PlaceChrome(s);
}

// After a curved screen moved (a drag or a carry ended, grab that or a throw put it somewhere): its
// radius becomes its distance from the head again, so it stays curved round you wherever it went.
void FitCurve(Screen &s) {
    Mat head, p;
    if (s.curve <= 0 || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) || !ScreenPose(s, &p)) return;
    const double was = s.curve;
    s.curve = std::max(0.5, double(std::hypot(p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3])));
    CapCurve(s);
    if (std::fabs(s.curve - was) < 0.01) return void(s.curve = was);
    ApplyCurve(s);
    PlaceChrome(s);
}

// ---------------------------------------------------------------- visibility

// Angle in degrees between a panel's front and the direction from it to the head.
double FacingAngle(const Mat &p, const Mat &head) {
    double n[3], to[3] = {head.m[0][3] - p.m[0][3], head.m[1][3] - p.m[1][3], head.m[2][3] - p.m[2][3]};
    Column(p, 2, n);
    const double len = std::sqrt(Dot3(to, to)) + 1e-9;
    return std::acos(std::clamp(Dot3(n, to) / len, -1.0, 1.0)) * 180 / M_PI;
}

// The screens' shared visibility for the mode (before a pinned screen's own facing rule).
// The mode in effect: during a VR game (with g_inGames Hide), "always" becomes "only with
// the dashboard open", so the screens stay out of the game until you open the dashboard.
Mode EffectiveMode() {
    return g_gameRunning && g_inGames == InGames::Hide && g_mode == Mode::Always ? Mode::Dashboard : g_mode;
}

// A VR game starting or stopping (checked twice a second) resets the hide/show switch, whose
// meaning depends on the mode in effect.
void UpdateGame() {
    if (g_tick % 45) return;
    const bool running = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
    if (running == g_gameRunning) return;
    g_gameRunning = running;
    g_manual = false;
    std::printf("%s\n", running ? "a VR game started" : "the VR game ended");
}

bool ModeVisible() {
    if (g_paused) return false;
    switch (EffectiveMode()) {
        case Mode::Always: return !g_manual;
        case Mode::Toggle: return g_manual;
        case Mode::Dashboard: return g_manual || vr::VROverlay()->IsDashboardVisible();
        case Mode::Gesture: {
            if (g_manual) return true;
            // Looking at the chosen controller: it's within the gesture angle of the gaze.
            Mat head, c;
            if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ||
                !DevicePose(HandDevice(g_gestureHand.c_str()), &c))
                return false;
            double f[3], to[3] = {c.m[0][3] - head.m[0][3], c.m[1][3] - head.m[1][3], c.m[2][3] - head.m[2][3]};
            Column(head, 2, f);  // the head's +Z points backward
            const double len = std::sqrt(Dot3(to, to)) + 1e-9;
            return std::acos(std::clamp(-Dot3(f, to) / len, -1.0, 1.0)) * 180 / M_PI <= g_gestureAngle;
        }
    }
    return true;
}

// The screen at its alpha; each control dimmer (kChromeIdle) unless a laser is on it or
// it's being dragged.
void ApplyAlpha(const Screen &s) {
    vr::VROverlay()->SetOverlayAlpha(s.overlay, s.alpha);
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->SetOverlayAlpha(sub.overlay, s.alpha);
    bool active[7] = {s.hover[0] || s.drag == Drag::Move, s.hover[1], s.hover[2] || s.drag == Drag::Roll,
                      s.hover[3] || s.drag == Drag::Resize, s.hover[4], s.hover[5], s.hover[6]};
    for (int k = 0; k < 7; ++k) active[k] = active[k] || (s.handHover >> k & 1);
    const auto controls = s.Controls();
    for (int k = 0; k < 7; ++k) {
        if (controls[k] == vr::k_ulOverlayHandleInvalid) continue;
        // the resize handle brightens as an aim comes to its corner (UpdateCorner)
        const float idle = k == 3 ? kChromeIdle + (1 - kChromeIdle) * std::clamp(s.cornerGlow, 0.f, 1.f) : kChromeIdle;
        vr::VROverlay()->SetOverlayAlpha(controls[k], s.alpha * s.controls * (active[k] ? 1.f : idle));
    }
}

void SetVisible(Screen &s, bool visible, float alpha) {
    if (visible && std::fabs(alpha - s.alpha) > 0.01f) {
        s.alpha = alpha;
        ApplyAlpha(s);
    }
    if (visible == s.visible) return;
    s.visible = visible;
    if (visible) {
        vr::VROverlay()->ShowOverlay(s.overlay);
        for (const auto &[k, sub] : s.subs) vr::VROverlay()->ShowOverlay(sub.overlay);
        return;
    }
    // Hidden: the controls go at once (UpdateControls brings them back).
    vr::VROverlay()->HideOverlay(s.overlay);
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->HideOverlay(sub.overlay);
    for (auto o : s.Controls())
        if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(o);
    s.controls = 0, s.controlsUp = false;
}

void UpdateVisibility() {
    const bool shared = ModeVisible();
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    for (auto &[i, s] : g_screens) {
        bool visible = !g_paused && s.shown && (shared || s.drag != Drag::None) && !s.alone;
        // A floating window's panel: while a window floats on it, its output is on, and the
        // window isn't minimized (and once it has a crop).
        if (s.floating) visible = visible && s.floatOn && s.outputOn && !s.minimized && s.cropW > 0;
        float alpha = 1;
        Mat p;
        if (visible && s.pinned != kNone && s.pinned != vr::k_unTrackedDeviceIndex_Hmd && s.drag == Drag::None &&
            haveHead && ScreenPose(s, &p)) {
            // A pinned screen shows while you see its front: fully inside the wrist angle,
            // fading out over the last kFade degrees, gone beyond it (and from behind).
            const double a = FacingAngle(p, head);
            alpha = float(std::clamp((g_wristAngle - a) / kFade, 0.0, 1.0));
            visible = alpha > 0.02f;
        }
        SetVisible(s, visible, alpha);
    }
}

// Attention, for each screen's frame rate (compositor.c gives KWin frame callbacks at a
// rate for each level): focused while you look at the screen (within kFocusAngle of where
// your head points), a laser or the mouse is on it, or it's being carried; in view while
// any of it is within kViewAngle; hidden otherwise, or while it isn't shown. A level stays
// for a moment after its reason goes, so a glance away doesn't make it stutter, and goes up
// at once.
constexpr double kFocusAngle = 12, kViewAngle = 60;  // degrees
constexpr int64_t kFocusLinger = 1500, kViewLinger = 500, kInputFocus = 1500;  // ms

bool RayOnPlane(const Mat &p, const Mat &d, double *x, double *y);

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// The smallest angle between where the head points and the screen: to the point of its
// rectangle nearest where the head's ray meets its plane, its centre, and its corners. A
// curved screen counts as flat; the angles hardly differ.
double AngleToScreen(const Screen &s, const Mat &p, const Mat &head) {
    const double hw = s.metres / 2, hh = s.heightMetres() / 2;
    double f[3];
    Column(head, 2, f);  // the head's +Z points backward
    auto angleTo = [&](double u, double v) {
        double to[3];
        for (int i = 0; i < 3; ++i) to[i] = p.m[i][3] + u * p.m[i][0] + v * p.m[i][1] - head.m[i][3];
        const double len = std::sqrt(Dot3(to, to)) + 1e-9;
        return std::acos(std::clamp(-Dot3(f, to) / len, -1.0, 1.0)) * 180 / M_PI;
    };
    double best = 180, x, y;
    if (RayOnPlane(p, head, &x, &y)) best = angleTo(std::clamp(x, -hw, hw), std::clamp(y, -hh, hh));
    for (double u : {-hw, 0.0, hw})
        for (double v : {-hh, 0.0, hh}) best = std::min(best, angleTo(u, v));
    return best;
}

void UpdateAttention() {
    const int64_t now = NowMs();
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    for (auto &[i, s] : g_screens) {
        if (!s.visible) {
            s.attention = FT_HIDDEN;
            s.focusUntil = s.viewUntil = 0;
            continue;
        }
        bool focus = s.drag != Drag::None || now - s.inputMs < kInputFocus, view = focus;
        Mat p;
        if (!haveHead) {
            view = true;  // nothing to go by
        } else if (ScreenPose(s, &p)) {
            const double a = AngleToScreen(s, p, head);
            focus = focus || a <= kFocusAngle;
            view = view || a <= kViewAngle;
        }
        if (focus) s.focusUntil = now + kFocusLinger;
        if (view) s.viewUntil = now + kViewLinger;
        s.attention = now < s.focusUntil ? FT_FOCUSED : now < s.viewUntil ? FT_IN_VIEW : FT_HIDDEN;
    }
}


// Controllers' lasers on the screens (see the top): the flag follows the mode and whether a
// VR game runs, and where the mode leaves the controllers to the game, whether one points at
// the panel (UpdateAim).
bool LasersByMode() { return g_lasers == Lasers::Always || (g_lasers == Lasers::OutsideGames && !g_gameRunning); }
long g_keyboardAimUntil = 0;

void UpdateLasers() {
    const bool byMode = LasersByMode();
    for (auto &[i, s] : g_screens) {
        const bool want = byMode || (s.visible && g_tick < s.aimUntil);
        if (s.lasers == want) continue;
        s.lasers = want;
        vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, want);
    }
    if (keyboard::Shown()) keyboard::SetLasers(byMode || g_tick < g_keyboardAimUntil);
}

// The distance from a laser's line to a point ahead of it, or -1 when it's behind.
double RayDistance(const Mat &d, const Mat &c) {
    const double o[3] = {d.m[0][3], d.m[1][3], d.m[2][3]}, dir[3] = {-d.m[0][2], -d.m[1][2], -d.m[2][2]};
    const double v[3] = {c.m[0][3] - o[0], c.m[1][3] - o[1], c.m[2][3] - o[2]};
    const double t = Dot3(v, dir);
    if (t <= 0) return -1;
    const double q[3] = {v[0] - dir[0] * t, v[1] - dir[1] * t, v[2] - dir[2] * t};
    return std::sqrt(Dot3(q, q));
}

// Where a laser meets the screen's surface: u along it (along the arc when curved), v up, metres from
// its centre, and how far along the laser.
bool RayOnSurface(const Screen &s, const Mat &p, const Mat &laser, double *u, double *v, double *dist = nullptr);

// The resize handle goes to the corner something aims at, and brightens the closer the aim (user,
// 2026-10-10: Vision Pro-style resize from any corner, for hands, the gaze and lasers): a laser's ray
// (a controller's, or the pointer helper's for the gaze and the hand ray) landing within the corner
// zone (a fifth of the screen's height, 5 to 20 cm) of a corner, or a hand's pinch at one
// (handinput's corners()). Lit while resizing. With no aim for kControlsLinger ticks it goes back
// to the bottom right.
// The corner a laser (or, with HANDS_EDGE_GLOW off, a hand) aims at this tick, the closest on any panel:
// its soft glow (UpdateHandGuides).
handinput::Guide g_laserCorner, g_laserCornerNext;
void UpdateCorner(Screen &s, const Mat &p, const std::vector<Mat> &lasers) {
    const double W = s.metres, H = s.heightMetres();
    const double zone = std::clamp(0.2 * std::min(W, H), 0.05, 0.2);
    int cx = s.cornerX, cy = s.cornerY;
    double best = 0;
    bool held = false;
    const bool handGlow = g_handInput.params().edgeGlow;   // (hands have their own glow at the corner: UpdateHandGuides)
    if (s.drag == Drag::Resize && !(handGlow && IsHandHolder(s.dragDevice))) {
        best = 1, held = true;   // stays where it is, lit
    } else if (s.drag == Drag::Resize) {
        best = 0;
    } else {
        for (const Mat &d : lasers) {
            double u, v;
            if (!RayOnSurface(s, p, d, &u, &v)) continue;
            for (int x : {1, -1})
                for (int y : {-1, 1}) {
                    const double dist = std::hypot(u - x * W / 2, v - y * H / 2);
                    const double close = 1 - std::clamp((dist - HandleSize(s)) / std::max(1e-3, zone - HandleSize(s)), 0.0, 1.0);
                    // (the corner it's at keeps it on a tie, so it doesn't flip between two)
                    if (close > best + (x == s.cornerX && y == s.cornerY ? -1e-3 : 1e-3)) best = close, cx = x, cy = y;
                }
        }
        const auto &hc = g_handInput.corners();
        const auto it = handGlow ? hc.end() : hc.find(ScreenIndexOf(s));
        if (it != hc.end() && it->second.closeness >= best)
            best = it->second.closeness, cx = it->second.sx, cy = it->second.sy, held = it->second.held;
    }
    if (best > 0) {
        s.cornerUntil = g_tick + kControlsLinger;
        s.nearUntil = std::max(s.nearUntil, g_tick + kControlsLinger);   // the controls show
    } else if (g_tick >= s.cornerUntil && !s.hover[3]) {
        cx = 1, cy = -1;   // back to the bottom right
    } else {
        cx = s.cornerX, cy = s.cornerY;   // lingering where it was
    }
    // (not away from under a laser that's on it)
    if ((cx != s.cornerX || cy != s.cornerY) && !s.hover[3]) {
        s.cornerX = cx, s.cornerY = cy;
        PlaceChrome(s);
    }
    const float glow = float(best);
    const bool lit = held || s.drag == Drag::Resize;
    if (best > 0 && best > g_laserCornerNext.strength) {
        g_laserCornerNext.kind = handinput::Guide::Corner, g_laserCornerNext.panel = ScreenIndexOf(s);
        g_laserCornerNext.sx = s.cornerX, g_laserCornerNext.sy = s.cornerY, g_laserCornerNext.strength = best, g_laserCornerNext.selected = lit;
    }
    if (lit != s.cornerLit && s.handle != vr::k_ulOverlayHandleInvalid) {
        s.cornerLit = lit;
        const auto &px = HandleTexture(lit);
        vr::VROverlay()->SetOverlayRaw(s.handle, const_cast<uint8_t *>(px.data()), 64, 64, 4);
    }
    if (std::fabs(glow - s.cornerGlow) > 0.02f || (glow == 0) != (s.cornerGlow == 0)) {
        s.cornerGlow = glow;
        ApplyAlpha(s);
    }
}

// The controls are invisible until a laser is on one of them (SteamVR's hover event) or
// passes very close (within `reach`, about 1.5 times a button's size); they stay
// kControlsLinger ticks after it leaves, and while in use.
void UpdateControls() {
    std::vector<Mat> lasers;
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat d;
        if (vr::VRSystem()->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller && LaserPose(i, &d))
            lasers.push_back(d);
    }
    g_laserCorner = g_laserCornerNext, g_laserCornerNext = {};   // (last tick's: UpdateCorner fills the next)
    for (auto &[i, s] : g_screens) {
        Mat p;
        if (s.visible && ScreenPose(s, &p)) {
            // Points along the bar and at each button and the tab; a laser passing within
            // `reach` of one of them is close.
            std::vector<Mat> spots;
            const auto offsets = ControlOffsets(s);
            for (double f : {-0.5, -0.25, 0.0, 0.25, 0.5})
                spots.push_back(Mul(p, Mul(offsets[0], Translation(f * s.chrome, 0, 0))));
            const auto controls = s.Controls();
            for (int k = 1; k < 7; ++k)
                if (controls[k] != vr::k_ulOverlayHandleInvalid) spots.push_back(Mul(p, offsets[k]));
            UpdateCorner(s, p, lasers);
            const double reach = std::max(s.grip * 1.5, s.chrome * 0.12);
            for (const Mat &d : lasers) {
                bool close = false;
                for (const Mat &c : spots) {
                    const double r = RayDistance(d, c);
                    if (r >= 0 && r <= reach) close = true;
                }
                if (close) {
                    s.nearUntil = g_tick + kControlsLinger;
                    break;
                }
            }
        }
        const bool inUse = s.drag != Drag::None || std::any_of(std::begin(s.hover), std::end(s.hover), [](bool h) { return h; });
        const bool want = s.visible && (inUse || g_tick < s.nearUntil);
        // The controls stay shown while their screen is, just fully transparent when not
        // wanted: SteamVR's laser still hits them, and the hover event brings them in, for
        // any device's laser, whatever its shape.
        if (s.visible && !s.controlsUp) {
            for (auto o : s.Controls())
                if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->ShowOverlay(o);
            s.controlsUp = true;
            ApplyAlpha(s);
        }
        const float before = s.controls;
        s.controls = std::clamp(s.controls + (want ? 0.2f : -0.1f), 0.f, 1.f);
        if (s.controls != before) ApplyAlpha(s);
    }
}

// ---------------------------------------------------------------- moving, resizing, pinning

// To ft-floatd (@frametop_float), for floating windows: dock, close, resize. From an unbound
// socket, so its replies go nowhere.
bool g_beside = false;  // a second ft-screens next to the desktop (ft_vr_beside)

void SendFloat(const std::string &msg) {
    if (g_beside) return;
    static const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const char name[] = "frametop_float";
    std::memcpy(addr.sun_path + 1, name, sizeof name - 1);
    sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&addr),
           socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1));
    if (msg.rfind("resize ", 0) != 0)  // an edge drag sends many resizes a second
        std::printf("to ft-floatd: %s\n", msg.c_str());
}

// To the pointer helper (@ft_pointer_helper), from an unbound socket.
void SendPointer(const std::string &msg) {
    static const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const char name[] = "ft_pointer_helper";
    std::memcpy(addr.sun_path + 1, name, sizeof name - 1);
    sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&addr),
           socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1));
}

// A new panel: the pointer helper reads SteamVR's list of panels only every 20 s, so it's told
// at once ("overlay <key>"), or the mouse couldn't click a menu until then.
void AnnounceOverlay(const char *key) { SendPointer(std::string("overlay ") + key); }

// Where a device's ray meets the screen's plane, in the screen's x (right) and y (up),
// metres from its centre.
bool RayOnPlane(const Mat &p, const Mat &d, double *x, double *y) {
    const double o[3] = {d.m[0][3], d.m[1][3], d.m[2][3]}, dir[3] = {-d.m[0][2], -d.m[1][2], -d.m[2][2]};
    const double c[3] = {p.m[0][3], p.m[1][3], p.m[2][3]};
    double n[3], ax[3], ay[3];
    Column(p, 2, n), Column(p, 0, ax), Column(p, 1, ay);
    const double denom = Dot3(dir, n);
    if (std::fabs(denom) < 1e-4) return false;
    const double co[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const double t = Dot3(co, n) / denom;
    if (t <= 0) return false;
    const double rel[3] = {o[0] + dir[0] * t - c[0], o[1] + dir[1] * t - c[1], o[2] + dir[2] * t - c[2]};
    *x = Dot3(rel, ax), *y = Dot3(rel, ay);
    return true;
}

// Roll: a rotation about the screen's own front axis (counterclockwise as you see it).
Mat RollZ(double rad) {
    Mat m = Identity();
    m.m[0][0] = m.m[1][1] = float(std::cos(rad));
    m.m[1][0] = float(std::sin(rad)), m.m[0][1] = float(-std::sin(rad));
    return m;
}

// Roll the screen to `rad` from its pose at the press, snapping level within kRollSnap.
void ApplyRoll(Screen &s, double rad) {
    const bool pinned = s.pinned != kNone;
    Mat c = Identity();
    if (pinned && !DevicePose(s.pinned, &c)) return;
    const Mat base = pinned ? Mul(c, s.rollFrom) : s.rollFrom;
    const Mat p = Mul(base, RollZ(rad));
    const double tilt = std::asin(std::clamp(double(p.m[1][0]), -1.0, 1.0));  // the right edge's slope
    if (std::fabs(tilt) < kRollSnap * M_PI / 180) rad -= tilt;
    if (pinned) Pin(s, s.pinned, Mul(s.rollFrom, RollZ(rad)));
    else SetAbsolute(s, Mul(base, RollZ(rad)));
}

// The laser's angle around the screen's centre, in the frame of its pose at the press.
bool RollLaserAngle(const Screen &s, const Mat &d, double *rad) {
    Mat c = Identity();
    if (s.pinned != kNone && !DevicePose(s.pinned, &c)) return false;
    const Mat base = s.pinned != kNone ? Mul(c, s.rollFrom) : s.rollFrom;
    double hx, hy;
    if (!RayOnPlane(base, d, &hx, &hy)) return false;
    *rad = std::atan2(hy, hx);
    return true;
}

// The laser while moving a screen: from the carrying device to the bar.
void Laser(const Screen &s, const Mat &d, const Mat &p, double a[3], double b[3]) {
    const Mat bar = Mul(p, BarOffset(s));
    for (int k = 0; k < 3; ++k) a[k] = d.m[k][3], b[k] = bar.m[k][3];
}

// The point q on the segment a-b closest to pt, and its distance.
double SegmentClosest(const double pt[3], const double a[3], const double b[3], double q[3]) {
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {pt[0] - a[0], pt[1] - a[1], pt[2] - a[2]};
    const double t = std::clamp(Dot3(ap, ab) / (Dot3(ab, ab) + 1e-12), 0.0, 1.0);
    for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * t;
    const double v[3] = {q[0] - pt[0], q[1] - pt[1], q[2] - pt[2]};
    return std::sqrt(Dot3(v, v));
}
double LaserDistance(const Screen &s, const Mat &d, const Mat &p, vr::TrackedDeviceIndex_t dev, double q[3]) {
    Mat c;
    if (!DevicePose(dev, &c)) return 1e9;
    double a[3], b[3];
    Laser(s, d, p, a, b);
    const double pt[3] = {c.m[0][3], c.m[1][3], c.m[2][3]};
    return SegmentClosest(pt, a, b, q);
}

// The hand controller (not the carrying device) whose ring the laser is in, or kNone.
vr::TrackedDeviceIndex_t WristOnLaser(const Screen &s, const Mat &d, const Mat &p) {
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        double q[3];
        if (i != s.dragDevice && IsHandController(i) && LaserDistance(s, d, p, i, q) <= kWristZone) return i;
    }
    return kNone;
}

bool HandRay(uint32_t d);
void StartDrag(Screen &s, Drag mode, vr::TrackedDeviceIndex_t dev) {
    Mat d, p;
    if (dev == kNone || !DevicePose(dev, &d) || !ScreenPose(s, &p)) {
        std::printf("screen %d: %s not started: no pose for the %s or the screen\n", ScreenIndexOf(s) + 1,
                    DragName(mode), DeviceLabel(dev).c_str());
        return;
    }
    s.pinTarget = kNone;
    if (s.pinned != kNone && mode == Drag::Move) {
        // Carried freely; let go, it goes back on the same wrist (unless disarmed).
        s.pinTarget = s.pinned;
        SetAbsolute(s, p);
    }
    s.drag = mode;
    s.dragDevice = dev;
    // A hand ray carrying it by the bar keeps it facing you (user, 2026-10-13: "the display should remain
    // centered facing the user since we have no way to tilt displays up and down without grabbing them"):
    // the ray places it, the wrist doesn't turn it. Controllers and the mouse carry it rigidly.
    s.dragFacing = mode == Drag::Move && HandRay(dev);
    s.dragRel = Mul(Inverse(d), p);
    // Already in a ring when grabbed: that doesn't count as crossing it.
    s.onWrist = mode == Drag::Move ? WristOnLaser(s, d, p) : kNone;
    LightBar(s, s.pinTarget != kNone);
    if (mode == Drag::Resize) {
        // from the corner the handle is at; the opposite one stays put
        double hx, hy;
        Mat l;
        if (LaserPose(dev, &l) && RayOnSurface(s, p, l, &hx, &hy))
            s.grabX = hx - s.cornerX * s.metres / 2, s.grabY = hy - s.cornerY * s.heightMetres() / 2;
        else s.grabX = s.grabY = 0;
        s.keepX = -s.cornerX, s.keepY = -s.cornerY;
    }
    if (mode == Drag::Roll) {
        s.rollFrom = s.pinned != kNone ? s.pinRel : p;
        Mat l;
        if (!LaserPose(dev, &l) || !RollLaserAngle(s, l, &s.rollAngle)) s.drag = Drag::None, s.dragDevice = kNone;
    }
    if (s.drag != Drag::None)
        std::printf("screen %d: %s by the %s%s\n", ScreenIndexOf(s) + 1, DragName(mode), DeviceLabel(dev).c_str(),
                    vr::VROverlay()->IsDashboardVisible() ? " (Steam menu open)" : "");
    ApplyAlpha(s);
}

// Stop moving where it is (a command took over).
void EndDrag(Screen &s) {
    if (s.drag != Drag::None) std::printf("screen %d: %s ended\n", ScreenIndexOf(s) + 1, DragName(s.drag));
    const bool moved = s.drag != Drag::None;
    // (a floating window's last resize may be answered after the release: its corner stays kept a while)
    if (s.drag == Drag::Resize) s.keepUntil = g_tick + 45;
    else s.keepX = -1, s.keepY = 1;
    s.drag = Drag::None;
    s.dragDevice = kNone;
    s.dragFacing = false;
    s.pinTarget = s.onWrist = kNone;
    LightBar(s, false);
    ApplyAlpha(s);
    if (moved) FitCurve(s);
}

// Run one of Frametop's tools, `<repo>/<tool> <arg>`, in the background. log: its output goes
// there, in the host's runtime directory (XDG_RUNTIME_DIR, not the nested desktop's; else /tmp
// with O_NOFOLLOW: a symlink there must not be followed); null: to ours.
void RunTool(const char *tool, const char *arg, const char *log) {
    if (g_beside) return;
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe)) return;
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string logname = log ? std::string(runtime && *runtime ? runtime : "/tmp") + "/" + log : "ours";
    std::string path(exe);  // <repo>/screens/build/ft-screens -> <repo>/<tool>
    for (int up = 0; up < 3 && path.rfind('/') != std::string::npos; ++up) path.resize(path.rfind('/'));
    path += std::string("/") + tool;
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_addopen(&io, 0, "/dev/null", O_RDONLY, 0);
    if (log) {
        posix_spawn_file_actions_addopen(&io, 1, logname.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
        posix_spawn_file_actions_adddup2(&io, 1, 2);
    }
    std::string a(arg);
    char *argv[] = {path.data(), a.data(), nullptr};
    pid_t pid;  // reaped by the compositor's SIGCHLD handler
    // A log that can't be opened (a symlink or a directory in its place) fails the spawn too.
    const int rc = posix_spawn(&pid, path.c_str(), &io, nullptr, argv, environ);
    if (rc != 0)
        std::printf("can't run %s (log %s): %s\n", path.c_str(), logname.c_str(), std::strerror(rc));
    posix_spawn_file_actions_destroy(&io);
}

// `ft-layout <cmd>`, logging to frametop-layout.log. The desktop start truncates that log.
void RunLayout(const char *cmd) { RunTool("layout/ft-layout", cmd, "frametop-layout.log"); }

// KWin's outputs follow where the screens are, so the pointer and dragged windows cross
// to the screen you see next to this one: `ft-layout scale` runs once a move has settled.
long g_arrangeAt = -1;  // g_tick to run it at, -1 = not pending

void ArrangeDesktopSoon() { g_arrangeAt = g_tick + 45; }  // about half a second

void UpdateArrange() {
    if (g_arrangeAt < 0 || g_tick < g_arrangeAt) return;
    g_arrangeAt = -1;
    RunLayout("scale");
}

// Let go: pin to the armed wrist, as the screen is now.
void FinishDrag(Screen &s, int index) {
    const bool moved = s.drag == Drag::Move;
    const vr::TrackedDeviceIndex_t target = s.pinTarget;
    EndDrag(s);
    Mat c, p;
    if (!moved) return;
    if (!s.floating && !s.remote) ArrangeDesktopSoon();
    if (target != kNone && DevicePose(target, &c) && ScreenPose(s, &p)) {
        Pin(s, target, Mul(Inverse(c), p));
        if (target == vr::k_unTrackedDeviceIndex_Hmd) std::printf("screen %d: pinned to the head\n", index + 1);
        else std::printf("screen %d: pinned to the %s controller\n", index + 1, HandName(target));
    }
}

void RememberPlace(int i, Screen &s);       // (how far out a sent-away panel goes: by SendBack)
void ForgetIfPlacedFar(int i, Screen &s);

// A button release on any of our panels ends that device's drags (it may be over another
// screen by then).
void EndDragsBy(vr::TrackedDeviceIndex_t dev) {
    keyboard::EndDragBy(dev);
    for (auto &[index, s] : g_screens)
        if (s.drag != Drag::None && s.dragDevice == dev) FinishDrag(s, index), ForgetIfPlacedFar(index, s);
}

// While moving: the laser entering a controller's ring flips whether the screen pins to
// it when let go (so sweeping across arms it, sweeping back disarms it).
void CheckWristAim(Screen &s, const Mat &d, const Mat &p) {
    double q[3];
    if (s.onWrist != kNone && LaserDistance(s, d, p, s.onWrist, q) > kWristLeave) s.onWrist = kNone;
    if (s.onWrist == kNone) {
        s.onWrist = WristOnLaser(s, d, p);
        if (s.onWrist != kNone) s.pinTarget = s.pinTarget == s.onWrist ? kNone : s.onWrist;
    }
    LightBar(s, s.pinTarget != kNone);
}

// Show the rings and dots for the screen being carried (hide them otherwise).
void UpdateGuides() {
    const Screen *carried = nullptr;
    Mat d, p, head;
    for (auto &[i, s] : g_screens)
        if (s.drag == Drag::Move && DevicePose(s.dragDevice, &d) && ScreenPose(s, &p)) {
            carried = &s;
            break;
        }
    if (!carried || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        for (auto &[dev, g] : g_guides) g.ring.Show(false), g.dot.Show(false);
        return;
    }
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat c;
        const bool want = i != carried->dragDevice && IsHandController(i) && DevicePose(i, &c);
        if (!want) {
            auto it = g_guides.find(i);
            if (it != g_guides.end()) it->second.ring.Show(false), it->second.dot.Show(false);
            continue;
        }
        Guide &g = GuideFor(i);
        const double pt[3] = {c.m[0][3], c.m[1][3], c.m[2][3]};
        Mat m = FacingPose(pt, head);
        vr::VROverlay()->SetOverlayTransformAbsolute(g.ring.overlay, vr::TrackingUniverseStanding, &m);
        g.ring.Light(carried->pinTarget == i, RingTexture(carried->pinTarget == i), 128);
        g.ring.Show(true);
        double q[3];
        const double dist = LaserDistance(*carried, d, p, i, q);
        if (dist <= kDotRange) {
            m = FacingPose(q, head);
            vr::VROverlay()->SetOverlayTransformAbsolute(g.dot.overlay, vr::TrackingUniverseStanding, &m);
            g.dot.Light(dist <= kWristZone, DotTexture(dist <= kWristZone), 32);
        }
        g.dot.Show(dist <= kDotRange);
    }
}

// A floating window's resize: the corner goes where the laser (or a hand's pinch) is, at hx,
// hy on the panel, in both directions, and the window gets that many pixels at the same
// density (ft-floatd resizes it, and the new crop comes back as "float", with the top left
// corner kept where it is).
// From any corner (sx, sy: +1 right, top): the opposite one stays put (keepX, keepY: SetFloat moves
// the panel so).
void ResizeFloat(Screen &s, int index, double hx, double hy, double grabX, double grabY, int sx = 1, int sy = -1) {
    s.keepX = -sx, s.keepY = -sy;
    if (s.mpp <= 0 || g_tick - s.resizeSent < 4) return;  // about 20 a second
    const int w = std::max(320, int(std::lround((sx * (hx - grabX) + s.metres / 2) / s.mpp)));
    const int h = std::max(200, int(std::lround((sy * (hy - grabY) + s.heightMetres() / 2) / s.mpp)));
    if (w == s.resizeW && h == s.resizeH) return;
    s.resizeW = w, s.resizeH = h, s.resizeSent = g_tick;
    SendFloat("resize " + std::to_string(index + 1) + " " + std::to_string(w) + " " + std::to_string(h));
}

// A screen (or remote display) resized from the handle's corner to cu, cv (m from its centre, on its
// surface): its width along the diagonal, the opposite corner where it was.
void ResizeScreen(Screen &s, double cu, double cv) {
    Mat p;
    if (!ScreenPose(s, &p)) return;
    const double a = s.width > 0 ? double(s.height) / s.width : 9.0 / 16;
    double u, v;
    const double w = handinput::ResizeFromCorner(s.metres, a, s.cornerX, s.cornerY, cu, cv, kMinWidth, 12.0, &u, &v);
    if (std::fabs(w - s.metres) < 1e-5) return;
    const Mat moved = OnSurface(s, u, v, 0);
    s.metres = w;
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    ApplyCurve(s);
    if (s.pinned != kNone) Pin(s, s.pinned, Mul(s.pinRel, moved));
    else SetAbsolute(s, Mul(p, moved));
}

void UpdateDrag(Screen &s, int index) {
    Mat d;
    if (!DevicePose(s.dragDevice, &d)) return;
    if (s.drag == Drag::Move) {
        Mat p = Mul(d, s.dragRel), head;
        if (s.dragFacing && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
            const double at[3] = {p.m[0][3], p.m[1][3], p.m[2][3]};
            p = FacingPose(at, head);
        }
        SetAbsolute(s, p);
        CheckWristAim(s, d, p);
        return;
    }
    if (s.drag == Drag::Roll) {
        // Like turning a knob: the screen turns as far as the laser has gone around its centre.
        double a;
        Mat l;
        if (!LaserPose(s.dragDevice, &l) || !RollLaserAngle(s, l, &a)) return;
        ApplyRoll(s, std::remainder(a - s.rollAngle, 2 * M_PI));
        return;
    }
    // Resize: the corner follows the ray along the screen's diagonal (so it shrinks and
    // grows from any direction), keeping where on the handle it was grabbed, and the opposite
    // corner stays put.
    double hx, hy;
    Mat l, p;
    if (!LaserPose(s.dragDevice, &l) || !ScreenPose(s, &p) || !RayOnSurface(s, p, l, &hx, &hy)) return;
    if (s.floating) return ResizeFloat(s, index, hx, hy, s.grabX, s.grabY, s.cornerX, s.cornerY);
    ResizeScreen(s, hx - s.grabX, hy - s.grabY);
}

// Scroll while moving: push the screen away (up) or pull it closer, along the line from
// the head (not from the carrying device: the 3D mouse's device sits just in front of the
// bar, below the screen's centre, so that line points mostly up).
void Push(Screen &s, double notches) {
    Mat d, head;
    if (!DevicePose(s.dragDevice, &d) || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;
    Mat p = Mul(d, s.dragRel);
    const double to[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
    const double len = std::sqrt(Dot3(to, to));
    const double next = std::clamp(len * (1 + 0.08 * notches), 0.3, 10.0);
    for (int k = 0; k < 3; ++k) p.m[k][3] = float(head.m[k][3] + to[k] / (len + 1e-9) * next);
    s.dragRel = Mul(Inverse(d), p);
}

// ---------------------------------------------------------------- floating windows

// Show the window's rectangle of the buffer. Texture bounds are fractions of the buffer, v
// from the top. SteamVR reports mouse positions in the whole texture (the bounds applied), so
// the mouse scale is the buffer's size, as on a screen (see ToBuffer).
void CropOverlay(vr::VROverlayHandle_t o, const Screen &s, int x, int y, int w, int h) {
    if (s.width <= 0 || s.height <= 0 || w <= 0 || h <= 0) return;
    vr::VRTextureBounds_t b = {float(x) / s.width, float(y) / s.height, float(x + w) / s.width,
                               float(y + h) / s.height};
    vr::VROverlay()->SetOverlayTextureBounds(o, &b);
    vr::HmdVector2_t scale = {float(s.width), float(s.height)};
    vr::VROverlay()->SetOverlayMouseScale(o, &scale);
}

void ApplyCrop(Screen &s) {
    CropOverlay(s.overlay, s, s.cropX, s.cropY, s.cropW, s.cropH);
    for (const auto &[k, sub] : s.subs) CropOverlay(sub.overlay, s, sub.x, sub.y, sub.w, sub.h);
}

// ft-floatd's "float": the window's rectangle, its title bar, and the density. The panel's
// top left corner stays where it is when the window changes size.
// A floating window's panel and its controls exist only while a window floats on it: SteamVR
// allows k_unMaxOverlayCount (128) overlays in all, SteamVR's own included, and each panel
// with its controls takes seven, so eight idle slots held 56 (2026-10-07: with three screens,
// a third remote screen didn't fit). Everything the overlays show is kept in the Screen, so
// a new panel picks up where the old one was.
bool MakePanel(Screen &s, const char *prefix, const char *label);
void SetScreenTexture(const Screen &s, vr::SharedTextureHandle_t handle);

bool EnsureFloatPanel(Screen &s) {
    if (s.overlay != vr::k_ulOverlayHandleInvalid) return true;
    char prefix[64], label[64];
    std::snprintf(prefix, sizeof prefix, "frametop.float.%d", s.slot);
    std::snprintf(label, sizeof label, "Floating window %d", s.slot);
    if (!MakePanel(s, prefix, label)) return false;
    AnnounceOverlay(prefix);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
    if (s.key) SetScreenTexture(s, s.plain);
    ApplyCurve(s);
    if (s.pinned != kNone) Pin(s, s.pinned, s.pinRel);
    else SetAbsolute(s, s.pose);
    return true;
}

void DropFloatPanel(Screen &s) {
    for (auto o : s.All())
        if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(o);
    s.overlay = s.bar = s.handle = s.curveButton = s.rollButton = s.dockButton = s.closeButton = s.resetButton =
        vr::k_ulOverlayHandleInvalid;
    s.visible = s.controlsUp = s.barLit = s.cornerLit = false;
    s.controls = 0;
    std::fill(std::begin(s.hover), std::end(s.hover), false);
}

void SetFloat(Screen &s, double mpp, int x, int y, int w, int h, int title) {
    if (!EnsureFloatPanel(s)) return;
    const bool first = !s.floatOn || s.cropW <= 0;
    const double oldW = s.metres, oldH = s.heightMetres();
    s.floatOn = true;
    s.mpp = mpp;
    s.cropX = x, s.cropY = y, s.cropW = w, s.cropH = h, s.titleH = title;
    s.metres = w * mpp;
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    ApplyCurve(s);
    ApplyCrop(s);
    // the corner keepX, keepY (the top left, or the one opposite a corner being dragged) stays put
    const bool keep = s.drag == Drag::Resize || g_tick < s.keepUntil;
    const int kx = keep ? s.keepX : -1, ky = keep ? s.keepY : 1;
    const double dx = -kx * (s.metres - oldW) / 2, dy = -ky * (s.heightMetres() - oldH) / 2;
    if (!first && (std::fabs(dx) > 1e-6 || std::fabs(dy) > 1e-6)) {
        if (s.pinned != kNone) Pin(s, s.pinned, Mul(s.pinRel, Translation(dx, dy, 0)));
        else SetAbsolute(s, Mul(s.pose, Translation(dx, dy, 0)));
    } else {
        PlaceChrome(s);
    }
}

void Unfloat(Screen &s) {
    if (s.drag != Drag::None) EndDrag(s);
    for (auto &[k, sub] : s.subs) vr::VROverlay()->DestroyOverlay(sub.overlay);
    s.subs.clear();
    s.floatOn = s.minimized = s.titleCarry = false;
    s.cropW = s.cropH = 0;
    s.resizeW = s.resizeH = 0;
    DropFloatPanel(s);
}

// A popup or dialog (number k) at x, y, w, h in the buffer; w = 0 takes it away.
void SetSub(Screen &s, int index, int k, int x, int y, int w, int h) {
    auto it = s.subs.find(k);
    if (w <= 0 || h <= 0) {
        if (it != s.subs.end()) {
            vr::VROverlay()->DestroyOverlay(it->second.overlay);
            s.subs.erase(it);
        }
        return;
    }
    if (it == s.subs.end()) {
        Sub sub;
        char key[80], name[64];
        std::snprintf(key, sizeof key, "frametop.float.%d.sub.%d", index + 1, k);
        std::snprintf(name, sizeof name, "Floating window menu %d", k);
        if (vr::VROverlay()->CreateOverlay(key, name, &sub.overlay) != vr::VROverlayError_None) return;
        vr::VROverlay()->SetOverlayInputMethod(sub.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
        vr::VROverlay()->SetOverlaySortOrder(sub.overlay, 5);
        AnnounceOverlay(key);
        it = s.subs.emplace(k, sub).first;
        if (s.shown) {
            auto imp = g_imports.find(s.shown);
            if (imp != g_imports.end()) {
                vr::SharedTextureHandle_t handle = imp->second;
                vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(sub.overlay, &tex);
            }
        }
        vr::VROverlay()->SetOverlayAlpha(sub.overlay, s.alpha);
        if (s.visible) vr::VROverlay()->ShowOverlay(sub.overlay);
    }
    it->second.x = x, it->second.y = y, it->second.w = w, it->second.h = h;
    CropOverlay(it->second.overlay, s, x, y, w, h);
    PlaceSubs(s);
}

// ---------------------------------------------------------------- the catcher

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
};
Press g_press;
vr::VROverlayHandle_t g_catcher = vr::k_ulOverlayHandleInvalid;
bool g_catcherShown = false;

uint32_t ButtonBit(uint32_t linuxButton) { return 1u << (linuxButton - BTN_LEFT); }

void PressDown(vr::TrackedDeviceIndex_t dev, uint32_t button, int screen, double x, double y) {
    if (!g_press.buttons) g_press.device = dev, g_press.origin = screen, g_press.remoteOn = screen;
    g_press.buttons |= ButtonBit(button);
    g_press.screen = screen, g_press.x = x, g_press.y = y;
}

// A button came up somewhere that isn't a screen (the catcher, a control, or the helper's
// word): release it in KWin where its pointer is, and once nothing is held, the laser is
// off the screens, so KWin's pointer leaves.
void ReleaseAway(uint32_t button, void (*handle)(const struct ft_event *, void *), void *data) {
    if (!(g_press.buttons & ButtonBit(button))) return;
    g_press.buttons &= ~ButtonBit(button);
    if (!g_press.buttons) g_press.upAt = -1;
    if (g_press.screen < 0) return;
    ft_event e{};
    e.type = FT_BUTTON;
    e.screen = g_press.screen;
    e.button = button;
    e.pressed = false;
    e.x = g_press.x, e.y = g_press.y;
    handle(&e, data);
    std::printf("caught a release off the screens (button %u)\n", button);
    if (g_press.buttons) return;
    e = ft_event{};
    e.type = FT_LEAVE;
    e.screen = g_press.screen;
    handle(&e, data);
}

// Whether a hand controller holds anything (a button down): 1 yes, 0 no, -1 unknown. SteamVR
// answers overlay apps only while a VR game runs (checked 2026-10-04); outside games it can't
// say. The Frame controller's axes have no types, so the buttons are all there is.
int ControllerHolds(vr::TrackedDeviceIndex_t dev) {
    vr::VRControllerState_t st{};
    if (!vr::VRSystem()->GetControllerState(dev, &st, sizeof st)) return -1;
    return st.ulButtonPressed ? 1 : 0;
}

// Releases SteamVR never sends. Pausing, or hiding the screen a button went down on, takes the
// laser off it mid-click (the pause gesture's second thumbstick click does that), and SteamVR's
// laser mouse then forgets the button: no release comes, the catcher stayed up, and the game
// never got its controllers back (2026-10-04). So a held button is released here when it has
// no visible screen to come up on, or when its hand controller has held nothing for kStuckMs
// (only known during VR games, where the lost release took the game's controllers).
// The pointer helper's virtual controller has its own word for that ("up").
constexpr int64_t kStuckMs = 1000;
void ReleaseStuck(void (*handle)(const struct ft_event *, void *), void *data) {
    if (!g_press.buttons) {
        g_press.idleSince = -1;
        return;
    }
    const char *why = nullptr;
    const auto it = g_screens.find(g_press.screen);
    if (g_paused) why = "paused";
    else if (g_press.screen >= 0 && (it == g_screens.end() || !it->second.visible)) why = "its screen hid";
    else if (IsHandController(g_press.device)) {
        const int64_t now = NowMs();
        if (ControllerHolds(g_press.device) != 0) g_press.idleSince = -1;
        else if (g_press.idleSince < 0) g_press.idleSince = now;
        else if (now - g_press.idleSince >= kStuckMs) why = "the controller holds nothing";
    }
    if (!why) return;
    std::printf("a held button can't come up on a screen (%s): released\n", why);
    const vr::TrackedDeviceIndex_t dev = g_press.device;
    for (uint32_t b = 0; b < 32; ++b)
        if (g_press.buttons & (1u << b)) ReleaseAway(BTN_LEFT + b, handle, data);
    g_press.buttons = 0, g_press.upAt = -1, g_press.idleSince = -1;
    EndDragsBy(dev);
}

void ShowCatcher(bool on) {
    if (on == g_catcherShown || g_catcher == vr::k_ulOverlayHandleInvalid) return;
    g_catcherShown = on;
    if (on) vr::VROverlay()->ShowOverlay(g_catcher);
    else vr::VROverlay()->HideOverlay(g_catcher);
}

// Every tick: while a button is held, find what the pressing laser is on. On one of our
// panels or controls, note how far away; on none, put the catcher across it there.
void UpdateCatcher() {
    Mat l;
    if (!g_press.buttons || g_catcher == vr::k_ulOverlayHandleInvalid || !LaserPose(g_press.device, &l)) {
        ShowCatcher(false);
        return;
    }
    vr::VROverlayIntersectionParams_t params{};
    params.eOrigin = vr::TrackingUniverseStanding;
    for (int k = 0; k < 3; ++k) params.vSource.v[k] = l.m[k][3], params.vDirection.v[k] = -l.m[k][2];
    for (auto &[i, s] : g_screens) {
        if (!s.visible) continue;
        const auto all = s.All();  // one copy: two calls give two temporaries, not one range
        std::vector<vr::VROverlayHandle_t> parts(all.begin(), all.end());
        for (const auto &[k, sub] : s.subs) parts.push_back(sub.overlay);
        for (auto o : parts) {
            vr::VROverlayIntersectionResults_t hit;
            if (o != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->ComputeOverlayIntersection(o, &params, &hit)) {
                g_press.distance = std::max(0.05, double(hit.fDistance));
                ShowCatcher(false);
                return;
            }
        }
    }
    const double d = g_press.distance;
    const double pt[3] = {l.m[0][3] - l.m[0][2] * d, l.m[1][3] - l.m[1][2] * d, l.m[2][3] - l.m[2][2] * d};
    const Mat m = FacingPose(pt, l);  // across the laser, facing its start
    vr::VROverlay()->SetOverlayTransformAbsolute(g_catcher, vr::TrackingUniverseStanding, &m);
    vr::VROverlay()->SetOverlayWidthInMeters(g_catcher, float(std::max(0.5, 2 * d)));
    ShowCatcher(true);
}

bool OnBuffer(const Screen &s, double x, double y) { return x >= 0 && y >= 0 && x < s.width && y < s.height; }

Screen *Find(int one_based) {
    auto it = g_screens.find(one_based - 1);
    return it == g_screens.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- the lazy susan
// "spin next|prev|<degrees>": the panels in the room (screens and floating windows, not
// pinned ones) turn together about a vertical axis through your head, so the next panel to
// your right (next) or left (prev) glides to straight ahead, or the ring turns by that many
// degrees (positive turns it left, like next). Their arrangement stays as it is: the room
// turns instead of you. A spin that arrives during one adds to it, from where the panels are
// headed, so quick taps carry on smoothly. Grabbing a panel, or a command that places it,
// takes it out of the spin where it is. The 3D mouse's pointer goes to straight ahead.
// "spin to N" turns the ring the short way so panel N comes to straight ahead (spotwatch).
constexpr double kSpinSeconds = 0.3;  // how long a spin takes
constexpr double kSpinAhead = 8;      // degrees: a panel this near straight ahead is the current one
constexpr double kSpinFocus = 30;     // degrees: when a spin settles, the panel this near ahead gets typing

struct Spin {
    bool on = false;
    double cx = 0, cz = 0;    // the axis
    double from = 0, to = 0;  // radians, turned from the poses in base (positive: to the left)
    Clock::time_point start;
    std::map<int, Mat> base;  // index -> its pose before the spin
    int front = -1;           // settled: this panel came to the front (ft_vr_poll reports it)
    int target = -1;          // "spin to": this panel is the one coming to the front
} g_spin;

// p turned a radians about the vertical axis through (cx, cz); positive turns it to the left.
Mat Turned(const Mat &p, double a, double cx, double cz) {
    const double c = std::cos(a), s = std::sin(a);
    Mat r = Identity();
    r.m[0][0] = float(c), r.m[0][2] = float(s);
    r.m[2][0] = float(-s), r.m[2][2] = float(c);
    r.m[0][3] = float(cx - c * cx - s * cz);
    r.m[2][3] = float(cz + s * cx - c * cz);
    return Mul(r, p);
}

bool Spinnable(const Screen &s) { return s.pinned == kNone && (!s.floating || s.floatOn) && s.drag == Drag::None; }

double SpinNow() {
    if (!g_spin.on) return g_spin.to;
    const double t = std::min(1.0, std::chrono::duration<double>(Clock::now() - g_spin.start).count() / kSpinSeconds);
    return g_spin.from + (g_spin.to - g_spin.from) * t * t * (3 - 2 * t);
}

void UpdateSpin() {
    if (!g_spin.on) return;
    const double a = SpinNow();
    const bool done = Clock::now() - g_spin.start >= std::chrono::duration<double>(kSpinSeconds);
    for (auto it = g_spin.base.begin(); it != g_spin.base.end();) {
        auto s = g_screens.find(it->first);
        if (s == g_screens.end() || !Spinnable(s->second)) {
            it = g_spin.base.erase(it);  // grabbed, pinned, or gone: it stays where it is now
            continue;
        }
        SetAbsolute(s->second, Turned(it->second, a, g_spin.cx, g_spin.cz));
        ++it;
    }
    if (done) {
        // The panel now nearest straight ahead (of where you face) gets typing and the active window.
        Mat head;
        double nearest = kSpinFocus;
        if (g_spin.target >= 0 && g_spin.base.count(g_spin.target)) {
            g_spin.front = g_spin.target;  // what "spin to" asked for, wherever it settled
        } else if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
            for (const auto &[i, p] : g_spin.base) {
                const auto it = g_screens.find(i);
                Mat q;
                if (it == g_screens.end() || !it->second.visible || !ScreenPose(it->second, &q)) continue;
                double f[3] = {-head.m[0][2], 0, -head.m[2][2]};
                double d[3] = {q.m[0][3] - head.m[0][3], 0, q.m[2][3] - head.m[2][3]};
                const double fl = std::sqrt(Dot3(f, f)), dl = std::sqrt(Dot3(d, d));
                if (fl < 1e-6 || dl < 1e-6) continue;
                const double a = std::acos(std::clamp(Dot3(f, d) / (fl * dl), -1.0, 1.0)) * 180 / M_PI;
                if (a < nearest) nearest = a, g_spin.front = i;
            }
        }
        g_spin.on = false;
        g_spin.target = -1;
        g_spin.base.clear();
        ArrangeDesktopSoon();  // KWin's outputs follow where the screens are now
    }
}

extern bool g_susanBring;
void SusanCommand(bool next, char *reply, int size);
void SusanBring(int i, const Mat &head, char *reply, int size);

void SpinCommand(const char *arg, char *reply, int size) {
    if (g_susanBring && (!std::strcmp(arg, "next") || !std::strcmp(arg, "prev")))
        return SusanCommand(!std::strcmp(arg, "next"), reply, size);   // (bring the next one: SusanCommand)
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head))
        return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
    int target = -1;  // "spin to N": that panel (0-based)
    if (!std::strncmp(arg, "to ", 3)) {
        char *end;
        const long n = std::strtol(arg + 3, &end, 10);
        const Screen *s = end == arg + 3 || *end ? nullptr : Find(int(n));
        if (!s) return (void)std::snprintf(reply, size, "error no panel %s", arg + 3);
        if (s->pinned != kNone) return (void)std::snprintf(reply, size, "error pinned");
        if (!Spinnable(*s)) return (void)std::snprintf(reply, size, "error not in the room");
        target = int(n) - 1;
        if (g_susanBring) return SusanBring(target, head, reply, size);   // the front slot's way, as next and prev
    }
    if (g_spin.on) {
        g_spin.from = SpinNow();  // carry on from where the panels are now
    } else {
        g_spin.base.clear();
        for (auto &[i, s] : g_screens) {
            Mat p;
            if (Spinnable(s) && ScreenPose(s, &p)) g_spin.base[i] = p;
        }
        g_spin.cx = head.m[0][3], g_spin.cz = head.m[2][3];
        g_spin.from = g_spin.to = 0;
    }
    if (g_spin.base.empty()) return (void)std::snprintf(reply, size, "error nothing to spin");
    double turn;  // degrees, positive to the left
    const bool next = !std::strcmp(arg, "next");
    // Each panel's bearing from where you face, to the right positive, as it will be when the
    // spin so far ends.
    double fx = -head.m[0][2], fz = -head.m[2][2];
    const double fn = std::sqrt(fx * fx + fz * fz) + 1e-9;
    fx /= fn, fz /= fn;
    const double rx = -fz, rz = fx;
    auto bearing = [&](const Mat &p) {
        const Mat q = Turned(p, g_spin.to, g_spin.cx, g_spin.cz);
        const double dx = q.m[0][3] - head.m[0][3], dz = q.m[2][3] - head.m[2][3];
        return std::atan2(dx * rx + dz * rz, dx * fx + dz * fz) * 180 / M_PI;
    };
    if (target >= 0) {
        const auto it = g_spin.base.find(target);
        if (it == g_spin.base.end()) {  // (Spinnable but no pose yet)
            if (!g_spin.on) g_spin.base.clear();
            return (void)std::snprintf(reply, size, "error panel %d has no pose", target + 1);
        }
        turn = bearing(it->second);  // already within +-180: the short way round
        g_spin.target = target;
    } else if (next || !std::strcmp(arg, "prev")) {
        // The nearest visible panel past straight ahead comes to the front.
        double best = 0;
        bool found = false;
        for (const auto &[i, p] : g_spin.base) {
            const auto s = g_screens.find(i);
            if (s == g_screens.end() || !s->second.visible) continue;
            double a = bearing(p);
            if (!next) a = -a;
            if (a <= kSpinAhead) a += 360;
            if (!found || a < best) best = a, found = true;
        }
        if (!found || best >= 360 - kSpinAhead) {
            if (!g_spin.on) g_spin.base.clear();
            return (void)std::snprintf(reply, size, "ok 0 (no other panel)");
        }
        if (best > 180) best -= 360;  // the short way round
        turn = next ? best : -best;
        g_spin.target = -1;
    } else {
        char *end;
        turn = std::strtod(arg, &end);
        if (end == arg || *end) return (void)std::snprintf(reply, size, "error spin next|prev|to <N>|<degrees>");
        g_spin.target = -1;  // turned on by hand: the nearest panel takes the front again
    }
    g_spin.to += turn * M_PI / 180;
    g_spin.start = Clock::now();
    g_spin.on = true;
    SendPointer("recenter");  // the 3D mouse's pointer stays in front of you, on what comes there
    std::snprintf(reply, size, "ok %.1f", turn);
}

uint32_t LinuxButton(uint32_t vrButton) {
    switch (vrButton) {
        case vr::VRMouseButton_Right: return BTN_RIGHT;
        case vr::VRMouseButton_Middle: return BTN_MIDDLE;
        default: return BTN_LEFT;
    }
}

// Any of the holding laser's buttons coming up on one of our controls or the catcher.
void ReleaseAwayBy(vr::TrackedDeviceIndex_t dev, uint32_t vrButton, void (*handle)(const struct ft_event *, void *),
                   void *data) {
    if (g_press.buttons && dev == g_press.device) ReleaseAway(LinuxButton(vrButton), handle, data);
}

// Where a laser meets a panel's surface, in the panel's u (metres along it from the centre,
// along the arc when curved) and v (up). OpenVR curves a screen into a cylinder toward its
// front, centred `curve` metres in front of it (see OnSurface).
bool RayOnSurface(const Screen &s, const Mat &p, const Mat &laser, double *u, double *v, double *dist) {
    const Mat inv = Inverse(p);
    const double o[3] = {inv.m[0][0] * laser.m[0][3] + inv.m[0][1] * laser.m[1][3] + inv.m[0][2] * laser.m[2][3] + inv.m[0][3],
                         inv.m[1][0] * laser.m[0][3] + inv.m[1][1] * laser.m[1][3] + inv.m[1][2] * laser.m[2][3] + inv.m[1][3],
                         inv.m[2][0] * laser.m[0][3] + inv.m[2][1] * laser.m[1][3] + inv.m[2][2] * laser.m[2][3] + inv.m[2][3]};
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = -(inv.m[i][0] * laser.m[0][2] + inv.m[i][1] * laser.m[1][2] + inv.m[i][2] * laser.m[2][2]);
    if (s.curve <= 0) {
        if (std::fabs(d[2]) < 1e-6) return false;
        const double t = -o[2] / d[2];
        if (t <= 0) return false;
        *u = o[0] + d[0] * t, *v = o[1] + d[1] * t;
        if (dist) *dist = t;
        return true;
    }
    // x^2 + (z - r)^2 = r^2, on the screen's side of the axis (z < r).
    const double r = s.curve, oz = o[2] - r;
    const double a = d[0] * d[0] + d[2] * d[2], b = 2 * (o[0] * d[0] + oz * d[2]), c = o[0] * o[0] + oz * oz - r * r;
    const double disc = b * b - 4 * a * c;
    if (a < 1e-9 || disc < 0) return false;
    for (double t : {(-b - std::sqrt(disc)) / (2 * a), (-b + std::sqrt(disc)) / (2 * a)}) {
        const double x = o[0] + d[0] * t, z = oz + d[2] * t;
        if (t <= 0 || z >= 0) continue;
        *u = r * std::atan2(x, -z), *v = o[1] + d[1] * t;
        if (dist) *dist = t;
        return true;
    }
    return false;
}

// A press on a remote screen, dragged onto another remote screen (a window carried from one of
// the host's displays to another): SteamVR keeps sending the moves to the panel the press
// began on, as if its surface went on past its edges, and nothing to the one under the laser.
// Each remote screen is its own stream to one of the host's displays, so the host's pointer
// goes there only through that screen: the pressing laser is hit-tested against all the remote
// screens here, and the nearest it meets takes the moves. On the one the press began on,
// SteamVR's own moves do (panelEvent drops them while the laser is elsewhere: that panel's
// surface, carried on, can pass behind or in front of another one, and its moves pulled the
// host's pointer back, 2026-10-07). The release follows (see panelEvent).
void UpdateRemoteDrag(void (*handle)(const struct ft_event *, void *), void *data) {
    if (!g_press.buttons) return;
    const auto origin = g_screens.find(g_press.origin);
    Mat l;
    if (origin == g_screens.end() || !origin->second.remote || !LaserPose(g_press.device, &l)) return;
    int on = -1;
    double best = 1e9, bx = 0, by = 0;
    for (auto &[i, s] : g_screens) {
        Mat p;
        double u, v, t;
        if (!s.remote || !s.visible || s.width <= 0 || !ScreenPose(s, &p) || !RayOnSurface(s, p, l, &u, &v, &t) ||
            t >= best)
            continue;
        const double x = (u / s.metres + 0.5) * s.width, y = (0.5 - v / s.heightMetres()) * s.height;
        if (!OnBuffer(s, x, y)) continue;
        on = i, best = t, bx = x, by = y;
    }
    if (on != g_press.remoteOn)
        std::printf("drag from remote screen %d: on %d (laser of device %u at %.2f %.2f %.2f toward %.2f %.2f %.2f)\n",
                    g_press.origin + 1, on < 0 ? 0 : on + 1, g_press.device, l.m[0][3], l.m[1][3], l.m[2][3],
                    -l.m[0][2], -l.m[1][2], -l.m[2][2]);
    g_press.remoteOn = on;
    if (on < 0 || on == g_press.origin) return;
    if (g_press.screen == on && std::fabs(g_press.x - bx) < 0.5 && std::fabs(g_press.y - by) < 0.5) return;
    g_press.screen = on, g_press.x = bx, g_press.y = by;
    g_screens[on].inputMs = NowMs();
    ft_event e{};
    e.type = FT_MOTION;
    e.screen = on;
    e.x = bx, e.y = by;
    handle(&e, data);
}

// Where the mode leaves the controllers to a VR game (see the top): a hand controller
// pointing at a panel, its controls, or a floating window's popups keeps that panel's laser
// on (UpdateLasers) until kAimLinger ticks after it points away, like SteamVR's own floating
// windows. Leaving takes a wider margin than arriving, and a drag or a held button keeps it
// on. The keyboard is one overlay, so SteamVR's own intersection test does there.
void UpdateAim() {
    if (LasersByMode()) return;
    std::vector<Mat> lasers;
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat d;
        if (IsHandController(i) && LaserPose(i, &d)) lasers.push_back(d);
    }
    for (auto &[index, s] : g_screens) {
        Mat p;
        if (!s.visible || !ScreenPose(s, &p)) continue;
        if (s.drag != Drag::None || (g_press.buttons && g_press.screen == index)) {
            s.aimUntil = g_tick + kAimLinger;
            continue;
        }
        const double m = s.grip * (g_tick < s.aimUntil ? 2.0 : 0.25), h = s.heightMetres();
        // The panel and its controls: the bar row under it, the resize tab off its corner.
        const double halfW = std::max(s.metres / 2 + HandleSize(s), s.chrome / 2 + s.chrome * 0.12 + s.grip * 2) + m;
        const double top = h / 2 + HandleSize(s) + m, bottom = std::min(BarY(s) - s.grip, -(h / 2 + HandleSize(s))) - m;
        for (const Mat &l : lasers) {
            double u, v;
            if (!RayOnSurface(s, p, l, &u, &v)) continue;
            bool on = std::fabs(u) <= halfW && v <= top && v >= bottom;
            for (const auto &[k, sub] : s.subs) {
                if (on || s.cropW <= 0) break;
                const double su = (sub.x + sub.w / 2.0 - (s.cropX + s.cropW / 2.0)) * s.mpp;
                const double sv = -(sub.y + sub.h / 2.0 - (s.cropY + s.cropH / 2.0)) * s.mpp;
                on = std::fabs(u - su) <= sub.w * s.mpp / 2 + m && std::fabs(v - sv) <= sub.h * s.mpp / 2 + m;
            }
            if (on) {
                s.aimUntil = g_tick + kAimLinger;
                break;
            }
        }
    }
    if (keyboard::Shown())
        for (const Mat &l : lasers)
            if (keyboard::Aimed(l)) g_keyboardAimUntil = g_tick + kAimLinger;
}

const char *LasersName() {
    switch (g_lasers) {
        case Lasers::Always: return "always";
        case Lasers::Dashboard: return "dashboard";
        default: return "outside_games";
    }
}

const char *ModeName() {
    switch (g_mode) {
        case Mode::Dashboard: return "dashboard";
        case Mode::Gesture: return "gesture";
        case Mode::Toggle: return "toggle";
        default: return "always";
    }
}

// ---------------------------------------------------------------- hand cutouts

void SetScreenTexture(const Screen &s, vr::SharedTextureHandle_t handle) {
    vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(s.overlay, &tex);
}

// The cutout buffers' renderer, set up the first time a hand is in front of a screen.
bool CutterReady() {
    if (g_cutterState) return g_cutterState > 0;
    uint64_t mods[64];
    const int n = ft_vr_modifiers(DRM_FORMAT_ABGR8888, mods, 64);
    const bool ok = g_cutter.Init(std::vector<uint64_t>(mods, mods + n), [](const handcut::Output *o) {
        auto it = g_cutImports.find(o);
        if (it == g_cutImports.end()) return;
        vr::VRIPCResourceManager()->UnrefResource(it->second);
        g_cutImports.erase(it);
    });
    g_cutterState = ok ? 1 : -1;
    std::printf(ok ? "hand cutouts ready\n" : "hand cutouts unavailable (see above)\n");
    return ok;
}

vr::SharedTextureHandle_t ImportCutout(const handcut::Output *o) {
    auto it = g_cutImports.find(o);
    if (it != g_cutImports.end()) return it->second;
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(o->buf.width);
    a.unHeight = uint32_t(o->buf.height);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = o->buf.format;
    a.ulModifier = o->buf.modifier;
    a.unPlaneCount = uint32_t(o->buf.n_planes);
    for (int i = 0; i < o->buf.n_planes && i < int(vr::MaxDmabufPlaneCount); ++i) {
        a.plane[i].unOffset = o->buf.offset[i];
        a.plane[i].unStride = o->buf.stride[i];
        a.plane[i].nFd = o->buf.fd[i];
    }
    vr::SharedTextureHandle_t h = 0;
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &h)) {
        std::fprintf(stderr, "openvr: ImportDmabuf failed for a cutout buffer\n");
        h = 0;
    }
    g_cutImports.emplace(o, h);
    return h;
}

void StopCutting(Screen &s) {
    if (!s.cutting) return;
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SideBySide_Parallel, false);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    if (s.plain) SetScreenTexture(s, s.plain);
    s.cutting = false, s.cutShown = 0;
}

// Each tick: for each visible screen with a hand in front of it (for either eye), or a glow
// of hand input on it, draw its client buffer with the hands cut out and the glows on top, and
// show that; else show the client buffer.
// Floating windows don't get cutouts yet: their panel and popups show crops of the client
// buffer (texture bounds), which a side-by-side buffer doesn't match.

void UpdateCutouts() {
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    if (g_tick % 450 == 0) {
        vr::EVRSettingsError se;
        const bool v = vr::VRSettings()->GetBool("camera", "enableConstructRoomView", &se);
        g_constructRoomView = se == vr::VRSettingsError_None ? v : true;
    }
    const bool room = g_roomView.Update(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(),
                                        g_gameRunning, g_constructRoomView);
    const bool hands = g_cutouts && haveHead && g_handsNow && room;
    double eyes[2][3];
    if (haveHead) handcut::EyePositions(head, eyes);
    if (hands) {
        const double at[3] = {head.m[0][3], head.m[1][3], head.m[2][3]};
        handcut::BuildMatte(g_hands.shown(), g_hands.capsules(), at, g_matte, g_matteNow);
    }
    const auto &glows = g_handInput.glows();
    for (auto &[i, s] : g_screens) {
        handcut::Spots spots[2];
        Mat p;
        bool cut = false;
        if (g_cutouts && haveHead && s.visible && !s.floating && s.key && s.width > 0 && ScreenPose(s, &p)) {
            if (hands)
                cut = handcut::Project({p, s.metres, s.heightMetres(), s.curve, s.width, s.height}, g_matteNow, eyes, spots);
            const auto g = glows.find(i);
            if (g != glows.end() && !g->second.empty()) {
                spots[0].glows = spots[1].glows = g->second;   // on the surface: the same texture point for both eyes
                cut = true;
            }
        }
        const handcut::Output *out = cut && CutterReady() ? g_cutter.Composite(i, s.key, s.frames, s.buf, spots) : nullptr;
        const vr::SharedTextureHandle_t h = out ? ImportCutout(out) : 0;
        if (!h) {
            StopCutting(s);
            continue;
        }
        if (!s.cutting) {
            vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
            vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SideBySide_Parallel, true);
            s.cutting = true;
        }
        if (h != s.cutShown) SetScreenTexture(s, h), s.cutShown = h;   // the same buffer stays: no new frame for SteamVR
    }
}

// ---------------------------------------------------------------- near hand input

// The pointer helper's word on the hands ("hands <L> <R> <allowed>", on change and every
// second): each side a bare hand ("hand"), a controller's ("controller"), or neither ("off"),
// and whether hand input is allowed now (the headset worn, and no VR game, or its dashboard
// or Frametop's panels up during one). Older than 3 s, it lapses: no near input.
struct HandsWord {
    bool hand[2] = {false, false};
    bool controller[2] = {false, false};
    bool allowed = false;
    int64_t atMs = INT64_MIN / 2;
    int64_t handMs[2] = {INT64_MIN / 2, INT64_MIN / 2};   // when each side's word last said "hand"
} g_handsWord;
// The pointer helper's word on typing ("typing <L 0|1> <R 0|1>", on change and every second): the
// hands typing on a real keyboard holds back (pointer/helper/typing.h): no pokes, near grabs or
// "grab that" by them. Older than 3 s, it lapses.
struct TypingWord {
    bool side[2] = {false, false};
    int64_t atMs = INT64_MIN / 2;
} g_typingWord;
// ...and on its hand rays ("ray <L 0|1> <R 0|1>", the same way): our laser (ft_pointer's) is a hand
// pointing only while one is up; else it's the mouse's or the gaze's (FromLaser).
TypingWord g_rayWord;
// HANDS_NEAR: 1 (default) follows the helper's word, 0 is off, force is on without it (for
// testing without the helper; the "handinput" command switches it too).
enum class NearMode { Off, Helper, Force } g_nearMode = NearMode::Helper;

// Hand input on this side now: the helper's word (or force). The palm menu goes by this alone:
// paused for a VR game, the helper allows hands while the dashboard is open, and the ring isn't a
// panel that hides.
bool HandsAllowed(int side) {
    if (g_nearMode == NearMode::Force) return true;
    return NowMs() - g_handsWord.atMs < 3000 && g_handsWord.allowed && g_handsWord.hand[side];
}

// Near input: that, with near input on and Frametop's panels not paused away.
bool SideAllowed(int side) { return g_nearMode != NearMode::Off && !g_paused && HandsAllowed(side); }

bool HandInputOn() { return g_nearMode != NearMode::Off; }

// A hand holding a panel (a grab, or a press in KWin) holds on while the helper's word says its side
// is "off" (not a controller's) for up to handinput's holdLostMs: the third live test's log has the
// word going "left off, right off" for a moment 24 times in 7 minutes, and three holds dropped there
// (after 0.23, 4.51 and 9.80 s). The hands, if ft-hands still has them, go on as ever; if not, the
// hold's own tracking-loss rule ends it.
bool HoldsThroughWord(int side) {
    if (g_nearMode != NearMode::Helper || g_paused) return false;
    if (NowMs() - g_handsWord.atMs >= 3000 || !g_handsWord.allowed || g_handsWord.controller[side] || g_handsWord.hand[side]) return false;
    return NowMs() - g_handsWord.handMs[side] < int64_t(g_handInput.params().holdLostMs);
}

// The panel as hand input sees it: its place, size, buffer, controls and popups.
handinput::Panel InputPanel(const Screen &s, int index, const Mat &p) {
    handinput::Panel P;
    P.index = index, P.pose = p, P.width = s.metres, P.height = s.heightMetres(), P.curve = s.curve;
    P.pxW = s.width, P.pxH = s.height;
    P.floating = s.floating;
    P.cropX = s.cropX, P.cropY = s.cropY, P.cropW = s.cropW, P.cropH = s.cropH, P.titleH = s.titleH;
    P.glows = !s.floating;
    P.held = s.drag != Drag::None && !IsHandHolder(s.dragDevice);
    double u[7], v[7];
    ControlUV(s, u, v);
    const auto controls = s.Controls();
    static const handinput::Control kinds[7] = {handinput::Control::Bar, handinput::Control::Curve,
                                                handinput::Control::Roll, handinput::Control::Tab,
                                                handinput::Control::Dock, handinput::Control::Close,
                                                handinput::Control::Reset};
    for (int k = 0; k < 7; ++k) {
        if (controls[k] == vr::k_ulOverlayHandleInvalid) continue;
        const double c = k == 3 ? HandleSize(s) : s.grip;
        const double hw = k == 0 ? s.chrome / 2 : c / 2, hh = k == 0 ? s.chrome * 12 / 256 : c / 2;
        P.controls.push_back({kinds[k], u[k], v[k], hw, hh});
    }
    if (s.floating && s.mpp > 0)
        for (const auto &[k, sub] : s.subs)
            P.popups.push_back({(sub.x + sub.w / 2.0 - (s.cropX + s.cropW / 2.0)) * s.mpp,
                                -(sub.y + sub.h / 2.0 - (s.cropY + s.cropH / 2.0)) * s.mpp, sub.w * s.mpp, sub.h * s.mpp,
                                sub.x, sub.y, sub.w, sub.h});
    return P;
}

// A hand took a panel by a grab zone: like StartDrag, held by the hand (a pinned screen is
// carried freely and goes back on its wrist when let go). A second hand on a panel the first
// already holds joins it (two hands: handinput's stretch).
void StartHandDrag(Screen &s, int side, handinput::Zone zone) {
    if (s.drag != Drag::None) return;
    Mat p;
    if (!ScreenPose(s, &p)) return;
    const Drag mode = zone == handinput::Zone::Tab || zone == handinput::Zone::Corner ? Drag::Resize : Drag::Move;
    s.pinTarget = kNone;
    if (s.pinned != kNone && mode == Drag::Move) {
        s.pinTarget = s.pinned;
        SetAbsolute(s, p);
    }
    s.drag = mode;
    s.dragDevice = kHandHolder[side];
    s.onWrist = kNone;
    LightBar(s, s.pinTarget != kNone);
    ApplyAlpha(s);
}

// Writes want-hz for ft-hands (hands/track/main.cpp) while a hand is near a panel, or within
// handinput's rateReach (0.30 m) of one: a still hand is tracked at 15 Hz, which doubles a poke's
// latency, and a tap from outside the 7.5 cm touch zone crosses it in a frame or two. Kept fresh
// twice a second.
void WantHz(bool near) {
    static int64_t last = 0;
    const int64_t now = NowMs();
    if (!near || now - last < 500) return;
    last = now;
    const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/want-hz";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const ssize_t n = write(fd, "30\n", 3);
    (void)n;
    close(fd);
}

// ---------------------------------------------------------------- SteamVR's panels by hand

// Near input on SteamVR's own panels (user, 2026-10-13: "work on the native SteamVR overlay windows"):
// the dashboard, the Steam menu, Steam's keyboard, other apps' overlays. A poke or a pinch on one
// clicks it. Grabs there (a pinch on its border or like a card to move it, at a corner to resize) are
// off: handinput's GrabZone gives SteamVR's panels none (user, 2026-10-13: out of scope; the hand ray's
// pinch on a window's grab bar moves it). The grab path below stays for a later try. ft-screens can't
// click them itself, so the pointer helper's laser (our virtual controller) does what the hand does:
// "touch <left|right> <verb> [x y z qw qx qy qz] [key]" on @ft_pointer_helper, the laser's pose in
// the standing universe (pointer/helper/ft-pointer.cpp's "Touch"). Its origin kSteamBack in front
// of the point, aimed in: hover (the hand near, the laser there), press (a click: its key goes to
// ft-screens' typing as a click there would), move (held: dragging), up, grab (go there, hover,
// then press: a move or resize), end (the hand has left: the laser goes back to whoever had it).
// OpenVR won't say where a dashboard panel is or how big (most report a 0x0 texture), so for each hands
// frame a cone of rays from the eyes past each index tip finds the panels near the hands, and
// steampanels::FitPanel fits each to its hits (steampanels.h). The keys come from the helper's overlay
// list ("steamvr <seq> <part> <parts> key...", which it keeps while hands are allowed); ours (frametop.*)
// and POINTER_IGNORE's aren't on it, and panels the rays can't hit (SteamVR's scene-graph controls)
// never fit. A move or a resize first hovers just inside the panel's edge: only a floating panel shows
// its window controls then (valve.steam.gamepadui.floatingfooter, under it), and only then does the
// laser press its grab bar (kSteamBar under the bottom edge, as the helper's layout placement does) or
// its resize margin (kSteamMargin past the corner), and follow the hand rigidly (steampanels::Follow).
// A gamescope app window (Spotifast's) shows no footer but has the grab bar and resize handle all the
// same, only past the bottom right corner (headset probing, 2026-10-13), so it counts as floating
// (AppWindow). A docked dashboard or another app's overlay has none: the pinch does nothing, and the
// log says so.
// No fist grabs, stretch, throws, flicks, pushes or two-finger scroll there (handinput's
// Panel::foreign). Only while ft-screens runs (its desktop).
constexpr int kSteamIndex = 1000;      // near input's panel indices from here on are SteamVR's
constexpr double kSteamBack = 0.10;    // the laser starts this far in front (its small controls float a few cm out)
constexpr double kSteamBar = 0.075;    // a floating panel's grab bar under its bottom edge (LAYOUT_GRAB_OFFSET)
constexpr double kSteamMargin = 0.01;  // ...its resize margin, past a corner each way
constexpr double kSteamAppMargin = 0.03;  // ...an app window's (its handle is 1-5 cm out)
constexpr double kSteamCone = 12;      // the rays, degrees each way round the line from the eyes to the index tip
constexpr int kSteamRays = 7;          // ...7 x 7 of them
constexpr double kSteamReach = 0.40;   // ...hitting within this of the tip
constexpr int64_t kSteamFitMs = 500;   // a panel the rays haven't found this long isn't near input's (unless a hand is at it)

struct SteamPanel {
    std::string key;
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    int index = -1;
    bool visible = false;
    steampanels::Fit fit;
    int64_t fitMs = INT64_MIN / 2;
};
std::vector<SteamPanel> g_steamPanels;
std::map<std::string, int> g_steamIndex;   // a key keeps its index
std::vector<std::string> g_steamParts;     // the list as it comes, part by part
long g_steamSeq = -1;
int g_steamPart = 0;
int64_t g_steamListMs = INT64_MIN / 2, g_steamLookMs = INT64_MIN / 2;

// What each hand is doing with the laser on a SteamVR panel.
struct SteamTouch {
    enum class St { None, Hover, Down, GrabWait, Grab, Refused } st = St::None;
    int panel = -1;
    std::string key;
    Mat panel0{}, laser0{};   // the panel as the grab began, and the laser that holds it
    double w0 = 0, h0 = 0;
    bool resize = false;
    int sx = 1, sy = -1;
    int64_t sinceMs = 0, leftMs = 0;
} g_steamTouch[2];

const SteamPanel *FindSteam(int index) {
    for (const SteamPanel &sp : g_steamPanels)
        if (sp.index == index) return &sp;
    return nullptr;
}

// "steamvr <seq> <part> <parts> key key...": the helper's overlay list, in parts that fit a datagram.
void SteamListPart(const char *cmd) {
    long seq;
    int part, parts, at = 0;
    if (std::sscanf(cmd, "steamvr %ld %d %d%n", &seq, &part, &parts, &at) < 3 || parts < 1) return;
    if (part == 0) g_steamParts.clear(), g_steamSeq = seq, g_steamPart = 0;
    if (seq != g_steamSeq || part != g_steamPart) return;   // (a part lost: the next list, a few seconds on)
    ++g_steamPart;
    std::istringstream words(cmd + at);
    for (std::string k; words >> k;)
        if (k.rfind("frametop.", 0) != 0) g_steamParts.push_back(k);
    if (part != parts - 1) return;
    g_steamListMs = NowMs();
    std::vector<SteamPanel> next;
    for (const std::string &k : g_steamParts) {
        SteamPanel sp;
        for (const SteamPanel &o : g_steamPanels)
            if (o.key == k) sp = o;
        if (sp.key.empty()) {
            sp.key = k;
            auto it = g_steamIndex.find(k);
            if (it == g_steamIndex.end()) it = g_steamIndex.emplace(k, kSteamIndex + int(g_steamIndex.size())).first;
            sp.index = it->second;
        }
        next.push_back(sp);
    }
    g_steamPanels.swap(next);
    g_steamLookMs = INT64_MIN / 2;   // look them up now
}

// For each new hands frame: the panels near the hands, fitted to the rays' hits. Their handles and
// whether they show, four times a second.
void ScanSteamPanels(const handinput::Input &in) {
    static uint64_t lastFrame = 0;
    const int64_t now = NowMs();
    if (now - g_steamListMs > 60000) g_steamPanels.clear();   // (the helper's list stopped: it's gone)
    if (g_hands.frame() == lastFrame || !in.haveHead || g_steamPanels.empty()) return;
    lastFrame = g_hands.frame();
    if (now - g_steamLookMs >= 250) {
        g_steamLookMs = now;
        for (SteamPanel &sp : g_steamPanels) {
            if (sp.h == vr::k_ulOverlayHandleInvalid || !sp.visible)   // (Steam makes some of them again now and then)
                if (vr::VROverlay()->FindOverlay(sp.key.c_str(), &sp.h) != vr::VROverlayError_None) sp.h = vr::k_ulOverlayHandleInvalid;
            sp.visible = sp.h != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->IsOverlayVisible(sp.h);
        }
    }
    bool any = false;
    for (const SteamPanel &sp : g_steamPanels) any = any || sp.visible;
    if (!any) return;
    const double eye[3] = {in.head.m[0][3], in.head.m[1][3], in.head.m[2][3]};
    std::vector<std::vector<steampanels::Sample>> hits(g_steamPanels.size());
    for (const handcut::Hand &h : g_hands.seen()) {
        if (!in.allowed[h.right ? 1 : 0]) continue;
        const double tip[3] = {h.p[8][0], h.p[8][1], h.p[8][2]};
        for (const auto &d : steampanels::Cone(eye, tip, kSteamCone, kSteamRays))
            for (size_t i = 0; i < g_steamPanels.size(); ++i) {
                const SteamPanel &sp = g_steamPanels[i];
                if (!sp.visible) continue;
                vr::VROverlayIntersectionParams_t params{};
                params.vSource = {float(eye[0]), float(eye[1]), float(eye[2])};
                params.vDirection = {float(d[0]), float(d[1]), float(d[2])};
                params.eOrigin = vr::TrackingUniverseStanding;
                vr::VROverlayIntersectionResults_t r{};
                if (!vr::VROverlay()->ComputeOverlayIntersection(sp.h, &params, &r)) continue;
                const double off = std::hypot(r.vPoint.v[0] - tip[0], r.vPoint.v[1] - tip[1], r.vPoint.v[2] - tip[2]);
                if (off > kSteamReach) continue;
                steampanels::Sample q;
                q.u = r.vUVs.v[0], q.v = r.vUVs.v[1];
                for (int k = 0; k < 3; ++k) q.p[k] = r.vPoint.v[k];
                hits[i].push_back(q);
            }
    }
    for (size_t i = 0; i < g_steamPanels.size(); ++i) {
        steampanels::Fit f = steampanels::FitPanel(hits[i]);
        if (!f.ok) continue;
        // its front toward the eyes (a texture upside down fits facing away: turned about x, it's the same panel)
        const double to[3] = {eye[0] - f.pose.m[0][3], eye[1] - f.pose.m[1][3], eye[2] - f.pose.m[2][3]};
        if (to[0] * f.pose.m[0][2] + to[1] * f.pose.m[1][2] + to[2] * f.pose.m[2][2] < 0)
            for (int k = 0; k < 3; ++k) f.pose.m[k][1] = -f.pose.m[k][1], f.pose.m[k][2] = -f.pose.m[k][2];
        g_steamPanels[i].fit = f, g_steamPanels[i].fitMs = now;
    }
}

// SteamVR's panels for near input this tick: those the rays found lately, and those a hand is at.
void SteamInputPanels(std::vector<handinput::Panel> &panels) {
    const int64_t now = NowMs();
    for (const SteamPanel &sp : g_steamPanels) {
        if (!sp.visible || !sp.fit.ok) continue;
        const bool at = g_handInput.nearPanel(0) == sp.index || g_handInput.nearPanel(1) == sp.index ||
                        g_steamTouch[0].panel == sp.index || g_steamTouch[1].panel == sp.index;
        if (now - sp.fitMs > kSteamFitMs && !at) continue;
        handinput::Panel P;
        P.index = sp.index, P.pose = sp.fit.pose, P.width = sp.fit.width, P.height = sp.fit.height, P.curve = 0;
        P.pxW = int(std::lround(sp.fit.width * steampanels::kPxPerM)), P.pxH = int(std::lround(sp.fit.height * steampanels::kPxPerM));
        P.foreign = true;
        panels.push_back(P);
    }
}

void SendTouch(int side, const char *verb, const Mat *laser, const std::string &key = "") {
    std::string msg = std::string("touch ") + (side ? "right " : "left ") + verb;
    if (laser) {
        double q[4];
        steampanels::Quat(*laser, q);
        char b[160];
        std::snprintf(b, sizeof b, " %.4f %.4f %.4f %.5f %.5f %.5f %.5f", laser->m[0][3], laser->m[1][3], laser->m[2][3], q[0], q[1],
                      q[2], q[3]);
        msg += b;
    }
    if (!key.empty() && key.size() < 100) msg += " " + key;
    SendPointer(msg);
}

// A gamescope app window's overlay (gamescope.gamescope-0.window.N): floating, footer or not.
bool AppWindow(const std::string &key) {
    return key.rfind("gamescope.gamescope-", 0) == 0 && key.find(".window.") != std::string::npos;
}

// A floating panel's window controls showing under it (valve.steam.gamepadui.floatingfooter): it's one
// the dashboard lets a laser move and resize.
bool FooterUnder(const Mat &panel, double w, double h) {
    vr::VROverlayHandle_t f = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->FindOverlay("valve.steam.gamepadui.floatingfooter", &f) != vr::VROverlayError_None ||
        !vr::VROverlay()->IsOverlayVisible(f))
        return false;
    vr::ETrackingUniverseOrigin o;
    Mat m;
    if (vr::VROverlay()->GetOverlayTransformAbsolute(f, &o, &m) != vr::VROverlayError_None) return false;
    const Mat l = Mul(Inverse(panel), m);
    const double x = l.m[0][3], y = l.m[1][3], z = l.m[2][3];
    return std::fabs(x) < w / 2 + 0.10 && y < -h / 2 + 0.05 && y > -h / 2 - 0.35 && std::fabs(z) < 0.15;
}

// Near input's event on a SteamVR panel: the laser does it. False: not one of theirs.
bool SteamEvent(const handinput::Event &e) {
    if (e.panel < kSteamIndex) return false;
    const SteamPanel *sp = FindSteam(e.panel);
    if (!sp || !sp->fit.ok) return true;
    SteamTouch &t = g_steamTouch[e.side];
    const steampanels::Fit &f = sp->fit;
    const char *hand = e.side ? "right" : "left";
    double u, v;
    switch (e.type) {
        case handinput::Event::Motion:
        case handinput::Event::Press:
        case handinput::Event::Release: {
            steampanels::FromPixels(f.width, f.height, e.x, e.y, &u, &v);
            const Mat l = steampanels::Touch(f.pose, u, v, kSteamBack);
            if (t.panel != e.panel) t = SteamTouch{}, t.panel = e.panel, t.key = sp->key;
            if (e.type == handinput::Event::Press) {
                SendTouch(e.side, "press", &l, sp->key);
                t.st = SteamTouch::St::Down;
                std::printf("SteamVR panel %s: pressed (%s hand)\n", sp->key.c_str(), hand);
            } else if (e.type == handinput::Event::Release) {
                SendTouch(e.side, "up", &l);
                t.st = SteamTouch::St::Hover;
            } else {
                SendTouch(e.side, t.st == SteamTouch::St::Down ? "move" : "hover", &l);
                if (t.st == SteamTouch::St::None) t.st = SteamTouch::St::Hover;
            }
            t.leftMs = 0;
            break;
        }
        case handinput::Event::GrabStart: {
            t = SteamTouch{};
            t.panel = e.panel, t.key = sp->key, t.panel0 = f.pose, t.w0 = f.width, t.h0 = f.height;
            t.resize = e.zone == handinput::Zone::Corner || e.zone == handinput::Zone::Tab;
            t.sx = e.sx, t.sy = e.sy;
            // first a hover just inside the edge (the corner): a floating panel's window controls come up
            u = t.resize ? t.sx * std::max(0.0, f.width / 2 - 0.02) : 0;
            v = t.resize ? t.sy * std::max(0.0, f.height / 2 - 0.02) : -std::max(0.0, f.height / 2 - 0.02);
            const Mat l = steampanels::Touch(f.pose, u, v, kSteamBack);
            SendTouch(e.side, "hover", &l);
            t.st = SteamTouch::St::GrabWait, t.sinceMs = NowMs();
            break;
        }
        case handinput::Event::GrabEnd:
            if (t.panel == e.panel && t.st == SteamTouch::St::Grab) {
                SendTouch(e.side, "up", nullptr);
                std::printf("SteamVR panel %s: let go (%s hand)\n", t.key.c_str(), hand);
            }
            if (t.panel == e.panel) t.st = SteamTouch::St::Hover, t.leftMs = 0;
            break;
        default: break;   // (no throws, flicks, pushes, rewinds or scrolls on them: Panel::foreign)
    }
    return true;
}

// Hand input taken away (paused, off): every laser touch ends.
void EndSteamTouches() {
    for (int k = 0; k < 2; ++k)
        if (g_steamTouch[k].st != SteamTouch::St::None) {
            if (g_steamTouch[k].st == SteamTouch::St::Down || g_steamTouch[k].st == SteamTouch::St::Grab) SendTouch(k, "up", nullptr);
            SendTouch(k, "end", nullptr);
            g_steamTouch[k] = SteamTouch{};
        }
}

// Each tick: a grab's hover, then its press; carries; and a hand that has left lets the laser go.
void UpdateSteamTouches(const std::vector<handinput::Carry> &carries) {
    if (!HandInputOn()) return EndSteamTouches();
    const int64_t now = NowMs();
    for (int k = 0; k < 2; ++k) {
        SteamTouch &t = g_steamTouch[k];
        const char *hand = k ? "right" : "left";
        if (t.st == SteamTouch::St::None) continue;
        if (t.st == SteamTouch::St::GrabWait) {
            if (g_handInput.grabPanel(k) != t.panel) {
                t.st = SteamTouch::St::Hover;
            } else if (now - t.sinceMs >= 150 && (AppWindow(t.key) || FooterUnder(t.panel0, t.w0, t.h0))) {
                double u, v;
                steampanels::GrabPoint(t.w0, t.h0, t.resize, t.sx, t.sy, kSteamBar,
                                       AppWindow(t.key) ? kSteamAppMargin : kSteamMargin, &u, &v);
                t.laser0 = steampanels::Touch(t.panel0, u, v, kSteamBack);
                SendTouch(k, "grab", &t.laser0);
                t.st = SteamTouch::St::Grab;
                std::printf("SteamVR panel %s: %s by the %s hand\n", t.key.c_str(), t.resize ? "resized" : "moved", hand);
            } else if (now - t.sinceMs >= 600) {
                t.st = SteamTouch::St::Refused;
                std::printf("SteamVR panel %s: no window controls under it (not a floating panel): the %s hand can't %s it\n",
                            t.key.c_str(), hand, t.resize ? "resize" : "move");
            }
        }
        if (t.st == SteamTouch::St::Down && !g_handInput.pressing(k)) {   // (a press that ended without its release)
            SendTouch(k, "up", nullptr);
            t.st = SteamTouch::St::Hover;
        }
        if (t.st == SteamTouch::St::Refused && g_handInput.grabPanel(k) != t.panel) t.st = SteamTouch::St::Hover;
        if (t.st == SteamTouch::St::Hover) {
            if (g_handInput.nearPanel(k) == t.panel) {
                t.leftMs = 0;
            } else if (!t.leftMs) {
                t.leftMs = now;
            } else if (now - t.leftMs >= 300) {
                SendTouch(k, "end", nullptr);
                t = SteamTouch{};
            }
        }
    }
    for (const handinput::Carry &c : carries) {
        if (c.panel < kSteamIndex) continue;
        for (int k = 0; k < 2; ++k) {
            SteamTouch &t = g_steamTouch[k];
            if (t.panel != c.panel || t.st != SteamTouch::St::Grab) continue;
            Mat l;
            if (c.kind == handinput::Carry::Resize) {
                const double w = c.width > 0 ? c.width : t.w0, h = w * t.h0 / std::max(1e-6, t.w0);
                double u, v;
                steampanels::GrabPoint(w, h, true, t.sx, t.sy, 0, kSteamMargin, &u, &v);
                l = steampanels::Touch(c.pose, u, v, kSteamBack);
            } else {
                l = steampanels::Follow(t.panel0, c.pose, t.laser0);
            }
            SendTouch(k, "move", &l);
        }
    }
}

// The palm menu (palmmenu.h): before near input, which leaves it the hand it has. Its ring is
// a guide ("frametop.guide.palm.*"), so the pointer helper's cursor and rays pass through it.
palmmenu::PalmMenu g_palm;
GuidePart g_palmRing[2];

void UpdatePalmMenu(const handinput::Input &in, const std::vector<handinput::Panel> &panels) {
    palmmenu::Input pin;
    pin.nowNs = in.nowNs, pin.hands = &g_hands, pin.head = in.head, pin.haveHead = in.haveHead;
    // busy: near input presses or holds with that hand, or "grab that" has it; at a panel: near input
    // has it near one, or it's within the touch zone and a margin of one (a card pinch from below has
    // the palm up toward the face)
    for (int k = 0; k < 2; ++k) {
        pin.allowed[k] = in.haveHead && !g_beside && HandsAllowed(k);
        pin.busy[k] = g_handInput.pressing(k) || g_handInput.grabbing(k) || g_grabThat.armed(k) ||
                      g_grabThat.claims(k, g_handInput.frameNs());
        pin.atPanel[k] = g_handInput.near(k) || handinput::HandAtPanel(g_hands, k, panels, g_handInput.params());
    }
    g_palm.Update(pin);
    if (g_palm.toggled() >= 0) {
        std::printf("palm menu (%s hand): the Steam menu\n", g_palm.toggled() ? "right" : "left");
        RunTool("steam/ft-steam", "menu", nullptr);   // opens it, or closes the dashboard
    }
    for (int k = 0; k < 2; ++k) {
        GuidePart &r = g_palmRing[k];
        double pt[3];
        if (!in.haveHead || !g_palm.Ring(k, g_hands.shown(), pt)) {
            r.Show(false);
            continue;
        }
        if (r.overlay == vr::k_ulOverlayHandleInvalid) {
            const char *key = k ? "frametop.guide.palm.right" : "frametop.guide.palm.left";
            if (vr::VROverlay()->CreateOverlay(key, "Frametop: palm menu", &r.overlay) != vr::VROverlayError_None) continue;
            vr::VROverlay()->SetOverlayWidthInMeters(r.overlay, float(g_palm.params().size));
            vr::VROverlay()->SetOverlaySortOrder(r.overlay, 22);
        }
        const Mat m = FacingPose(pt, in.head);
        vr::VROverlay()->SetOverlayTransformAbsolute(r.overlay, vr::TrackingUniverseStanding, &m);
        r.Light(g_palm.pinched(k), RingTexture(g_palm.pinched(k)), 128);
        r.Show(true);
    }
}

// "Grab that" (grabthat.h): the detector reads each hands frame after near input (which leaves it
// a hand it's not pressing or grabbing with, nor one typing holds back); a fired gesture moves the
// panel it means over animMs, and remembers where it was (g_grabThat, with near input's globals).
struct GrabMove {
    Mat from, to, lastSet;
    double w0 = 1, w1 = 1;
    int64_t startMs = 0;
    double ms = 0;          // how long (0: grab that's animMs)
    bool easeOut = false;   // a throw: fast, then slowing (else grab that's smoothstep)
};
std::map<int, GrabMove> g_grabMoves;   // panels on their way
double Apart(const Mat &a, const Mat &b) {
    return std::hypot(a.m[0][3] - b.m[0][3], a.m[1][3] - b.m[1][3], a.m[2][3] - b.m[2][3]);
}

void StartGrabMove(int index, Screen &s, const Mat &to, double width, double ms = 0, bool easeOut = false) {
    Mat p;
    if (!ScreenPose(s, &p)) return;
    g_grabMoves[index] = {p, to, p, s.metres, s.floating ? s.metres : width, NowMs(), ms, easeOut};
}

// ---------------------------------------------------------------- the front slot
// One place in front of you for the lazy susan (user, 2026-10-11: "make the system a bit more logical
// with the lazy susan"; the proposals they took): a stroke (or Meta+Alt+Tab) brings the next screen
// there, 1 m ahead (g_frontReading), as big as its resolution needs for its text to read
// (grabthat::ReadableSize), one at a time: the one there goes home first. Turned away from, it goes
// home, unless you used it a while (g_frontKeepMs of input on it: kept). Resized there, it comes that
// size next time (g_frontDeg). Its home: where it lived before it came.
//   Manual controls take it (user, 2026-10-11, later: "if the primary slot has a window moved out of it
// through any gesture, pull, push, or drag with the pinch or laser, the lazy susan should release the
// display ... give up from manual controls but restore the monitor or window when it is next
// triggered"): grab that, a push, flick or throw, or a carry by hand or laser releases it: it's yours,
// it moves like any other screen and stays where you put it, until the next stroke puts it home.
// Grab that brings a screen to arm's reach as big as it reads (the same rule), outside the slot; the
// slot's one goes home first, unless it's released. SCREEN_SPIN_BRING=0: the lazy susan turns the room.
struct Front {
    int panel = -1;          // the screen in the slot (-1: empty)
    bool released = false;   // a manual control took it: it's yours until the next stroke puts it home
    Mat home{}, to{};        // where it lives, and where it is in the slot
    double homeWidth = 0, homeCurve = 0;
    double width = 0;        // its width in the slot (as brought, or as resized there)
    int64_t awaySinceMs = -1, lastTickMs = 0;
    double usedMs = 0;       // input on it while in the slot
    bool kept = false;       // used long enough: it stays when you look away
} g_front;
bool g_susanBring = true;
double g_frontReading = 1.0, g_frontDrop = 0.08;   // reading distance (m), and how far under the eyes its centre sits
double g_frontKeepMs = 3000;                       // SCREEN_FRONT_KEEP_MS: this much use keeps it (0: never)
// Turned away from, it goes home (SCREEN_FRONT_AWAY=1). Off since the user's 2026-10-11 call: "the lazy susan can
// be considered in use and remain active until I move it out of its spot manually" (a gaze-pinch drag, a hand grab).
bool g_frontAway = false;
grabthat::ReadParams g_frontRead;
std::map<int, double> g_frontDeg;   // a screen resized in the slot: the view it spans (deg wide), next time too
std::map<int, double> g_uiScale;    // KWin's scale for each screen (ft-layout's "scale", via the compositor)
std::map<int, double> g_uiScaleConf;   // SCREEN_UI_SCALE: a remote display's own UI scale ("104=2 103=1.5")
int64_t g_keyMs = INT64_MIN / 2;   // the last key typed (ft_vr_key: the front slot's screen has the typing)
constexpr double kFrontAway = 70;          // degrees off where you face: out of view
constexpr int64_t kFrontAwayMs = 600;      // ...for this long: it goes home
void ShowCarousel();

// Where a pose is from where the head faces (deg, right +).
double BearingFrom(const Mat &head, const Mat &p) {
    double fx = -head.m[0][2], fz = -head.m[2][2];
    const double n = std::hypot(fx, fz) + 1e-9;
    fx /= n, fz /= n;
    const double rx = -fz, rz = fx;
    const double dx = p.m[0][3] - head.m[0][3], dz = p.m[2][3] - head.m[2][3];
    return std::atan2(dx * rx + dz * rz, dx * fx + dz * fz) * 180 / M_PI;
}

// Where a screen lives: the one in the slot at home, one on its way somewhere where it's going.
bool HomeOf(int i, const Screen &s, Mat *p) {
    if (i == g_front.panel) return *p = g_front.home, true;
    const auto m = g_grabMoves.find(i);
    if (m != g_grabMoves.end()) return *p = m->second.to, true;
    return ScreenPose(s, p);
}

// The screens the lazy susan goes round, left to right as they live around you now.
std::vector<grabthat::Bearing> CarouselOrder(const Mat &head) {
    std::vector<grabthat::Bearing> out;
    for (auto &[i, s] : g_screens) {
        Mat p;
        if (i != g_front.panel && (!s.visible || !Spinnable(s) || g_handInput.heldByHand(i))) continue;
        if (HomeOf(i, s, &p)) out.push_back({i, BearingFrom(head, p)});
    }
    std::sort(out.begin(), out.end(), [](const grabthat::Bearing &a, const grabthat::Bearing &b) { return a.deg < b.deg; });
    return out;
}

// Its size in the slot at distance d: readable, or the view it was resized to there.
grabthat::Readable FrontSize(int i, const Screen &s, double d) {
    const auto ui = g_uiScale.find(i);
    const auto uc = g_uiScaleConf.find(i);
    const double scale = ui != g_uiScale.end() ? ui->second : uc != g_uiScaleConf.end() ? uc->second : 1;
    const double pxW = (s.floating ? s.cropW : s.width) / scale, pxH = (s.floating ? s.cropH : s.height) / scale;
    grabthat::ReadParams k = g_frontRead;
    const auto deg = g_frontDeg.find(i);
    if (deg != g_frontDeg.end() && deg->second > 1) k.ppd = pxW / deg->second, k.minW = 1, k.maxW = 170, k.maxH = 170;
    grabthat::Readable r = grabthat::ReadableSize(pxW, pxH, d, k);
    if (r.width <= 0 || s.floating) r.width = s.metres, r.curve = s.curve;   // (floating windows keep their size)
    return r;
}

void FrontHome(const char *why) {
    const auto it = g_screens.find(g_front.panel);
    if (it != g_screens.end()) {
        std::printf("front: screen %d goes home (%s)\n", g_front.panel + 1, why);
        it->second.curve = g_front.homeCurve;
        StartGrabMove(g_front.panel, it->second, g_front.home, g_front.homeWidth);
    }
    g_front = {};
}

// Screen i at `to` (facing you), as big as it reads there: to the slot (the one there goes home first),
// or (slot false: grab that) outside it, the slot's one going home first unless it's released.
void BringReadable(int i, const Mat &to, bool slot, const char *why) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end()) return;
    Screen &s = it->second;
    if (g_front.panel >= 0 && g_front.panel != i && (slot || !g_front.released)) FrontHome("another comes");
    Mat now;
    if (!ScreenPose(s, &now)) return;
    if (!slot) {
        if (g_front.panel == i) g_front.released = true;   // (grab that at the slot's one: a manual control)
        RememberPlace(i, s);
    } else if (g_front.panel != i) {
        const auto m = g_grabMoves.find(i);   // (on its way home: home is where it was going)
        g_front = {};
        g_front.panel = i;
        g_front.home = m != g_grabMoves.end() ? m->second.to : now;
        g_front.homeWidth = m != g_grabMoves.end() ? m->second.w1 : s.metres, g_front.homeCurve = s.curve;
        g_spin.front = i;   // typing and the active window go to it (ft_vr_poll: FT_FRONT)
        SendPointer("recenter");
    }
    double head[3] = {0, 0, 0};
    Mat hp;
    if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &hp)) head[0] = hp.m[0][3], head[1] = hp.m[1][3], head[2] = hp.m[2][3];
    const double d = std::max(0.2, std::hypot(to.m[0][3] - head[0], to.m[2][3] - head[2]));
    const grabthat::Readable r = FrontSize(i, s, d);
    if (!s.floating) s.curve = r.curve;
    if (slot) g_front.to = to, g_front.width = r.width, g_front.awaySinceMs = -1;
    StartGrabMove(i, s, to, r.width);
    std::printf("%s: screen %d %s (%s), %.2f m out, %.2f m wide%s (%.0fx%.0f deg, %.0f px/deg)\n", slot ? "front" : "grab that", i + 1,
                slot ? "to the front" : "to arm's reach", why, d, r.width, s.curve > 0 ? ", curved round you" : "", r.degW, r.degH, r.ppd);
    if (slot) ShowCarousel();
}

// A manual control took the slot's screen: it's yours until the next stroke puts it home.
void FrontRelease(int i, const char *why) {
    if (i != g_front.panel || g_front.released) return;
    g_front.released = true;
    std::printf("front: screen %d released (%s): it stays where you put it; the next stroke puts it home\n", i + 1, why);
}

// Sent away, a panel goes back out as far as it was before it was taken, at the size it was (user,
// 2026-10-13: "when you throw a monitor away, it should actually have the monitor return to where it
// was before you had grabbed it", then: "it only saves the size and distance it was at, but still
// allows you to move the screen around to other places ... If people want to have a saved layout,
// they can save and restore that layout"): along the throw's line, that far from the eyes. A hand's
// grab or "grab that" remembers it, unless it's remembered already (taken again before it was sent:
// still the first) or it's the front slot's (its home's). Put down out past kPlacedFar from the eyes,
// that's its distance now: forgotten, and a throw from there keeps it. A panel with nothing
// remembered flies out to throwDistance at its size, as before.
constexpr double kPlacedFar = 1.2;
void RememberPlace(int i, Screen &s) {
    Mat p, head;
    if (s.hasBefore || i == g_front.panel || !HomeOf(i, s, &p) || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;
    const auto m = g_grabMoves.find(i);
    s.beforeDist = Apart(head, p), s.beforeWidth = m != g_grabMoves.end() ? m->second.w1 : s.metres, s.beforeCurve = s.curve;
    s.hasBefore = true;
}

void ForgetIfPlacedFar(int i, Screen &s) {
    Mat head, p;
    if (!s.hasBefore || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) || !ScreenPose(s, &p)) return;
    if (Apart(head, p) > kPlacedFar) s.hasBefore = false;
    (void)i;
}

// Out along the line to `to` (a throw's place, facing the eyes), as far and as big as it was (the
// front slot's: as at home, and it leaves the slot), over ms, easing out. False: nothing remembered.
bool SendBack(int i, Screen &s, const Mat &to, double ms, const char *why) {
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return false;
    if (i == g_front.panel) {
        s.beforeDist = Apart(head, g_front.home), s.beforeWidth = g_front.homeWidth, s.beforeCurve = g_front.homeCurve;
        s.hasBefore = true;
        std::printf("front: screen %d leaves the slot (%s)\n", i + 1, why);
        g_front = {};
    }
    if (!s.hasBefore) return false;
    s.hasBefore = false;
    const double d[3] = {to.m[0][3] - head.m[0][3], to.m[1][3] - head.m[1][3], to.m[2][3] - head.m[2][3]};
    const double n = std::max(1e-6, std::hypot(d[0], d[1], d[2]));
    Mat at = to;
    for (int q = 0; q < 3; ++q) at.m[q][3] = head.m[q][3] + d[q] / n * s.beforeDist;
    std::printf("screen %d: %s, out as far as it was (%.1f m), %.2f m wide\n", i + 1, why, s.beforeDist, s.beforeWidth);
    if (!s.floating) s.curve = s.beforeCurve;
    StartGrabMove(i, s, at, s.beforeWidth, ms, true);
    return true;
}


// The gesture fired (user, 2026-10-10: consistent with a display already near you): at a panel that
// came by it and is still within arm's reach (where it came, or moved about there since), it goes
// back where it was; at another panel within arm's reach, it comes to the middle of arm's reach
// (and the gesture again sends it back there); at one farther away, it comes to arm's reach. One at
// a time: a panel that came before and is still where it came goes back as another comes, so they
// don't stack up in the same spot (three remote displays did, 2026-10-10).
// The panels "grab that" may take: shown, not pinned, not held (a laser or a hand carries it: s.drag) nor
// one a hand presses on (user, after the fourth live test: "only activate if the window is not already
// held and closed up"; near ones: grabthat's Params::nearest, in Pick).
std::vector<grabthat::Target> GrabThatTargets() {
    std::vector<grabthat::Target> targets;
    for (auto &[i, s] : g_screens) {
        Mat p;
        if (!s.visible || s.width <= 0 || s.height <= 0 || (s.floating && s.cropW <= 0) || s.pinned != kNone ||
            s.drag != Drag::None || g_handInput.heldByHand(i) || g_handInput.pressPanel(0) == i || g_handInput.pressPanel(1) == i ||
            !ScreenPose(s, &p))
            continue;
        targets.push_back({i, p, s.metres, s.heightMetres(), s.curve});
    }
    return targets;
}

// Per side: the panel "grab that" under way would take now (-1: none: it's idle, or aims at nothing it
// would take), and how far along it is (its glow; a fist or pinch at a panel waits on it only then).
int g_grabThatAim[2] = {-1, -1};
double g_grabThatProgress[2] = {0, 0};
void UpdateGrabThatAims() {
    const std::vector<grabthat::Target> targets = GrabThatTargets();
    for (int k = 0; k < 2; ++k) {
        grabthat::Fire f;
        double progress = 0;
        g_grabThatAim[k] = g_grabThat.params().on && g_grabThat.Aim(k, &f, &progress) ? grabthat::Pick(targets, f, g_grabThat.params()) : -1;
        g_grabThatProgress[k] = g_grabThatAim[k] >= 0 ? progress : 0;
    }
}

void GrabThat(const grabthat::Fire &f) {
    const std::vector<grabthat::Target> targets = GrabThatTargets();
    const grabthat::Params &k = g_grabThat.params();
    const char *hand = f.side ? "right" : "left";
    grabthat::Closest closest;
    const int i = grabthat::Pick(targets, f, k, &closest);
    if (i < 0) {
        // where the line went (seen from the head: right and up of straight ahead), and the panel it passed nearest
        double d[3], h[3];
        for (int q = 0; q < 3; ++q) d[q] = k.aimPalm ? f.normal[q] : f.palm[q] - f.eye[q];
        for (int q = 0; q < 3; ++q) h[q] = f.head.m[0][q] * d[0] + f.head.m[1][q] * d[1] + f.head.m[2][q] * d[2];
        const std::string near = closest.index >= 0 ? "; nearest screen " + std::to_string(closest.index + 1) + ", " +
                                                          std::to_string(int(std::lround(closest.deg))) + " deg off" : "; no panel it may take";
        return (void)std::printf("grab that (%s hand): no panel there (the line %+.0f deg right, %+.0f deg up of where the head points%s; "
                                 "%zu it may take; the palm %.0f deg off its line, %.2f m/s as it was held out)\n",
                                 hand, std::atan2(h[0], -h[2]) * 180 / M_PI, std::atan2(h[1], std::hypot(h[0], h[2])) * 180 / M_PI,
                                 near.c_str(), targets.size(), f.facing, f.speed);
    }
    // to the front slot at arm's reach (the one there goes home; the one there comes closer)
    BringReadable(i, grabthat::Arrival(f, k), false, i == g_front.panel ? "pulled from the front slot" : "grab that");
    (void)hand;
}

// Each tick: the panels on their way. One that something else moved meanwhile (a laser or a hand
// took it, the layout placed it, it was pinned) stays where that put it.
void AnimateGrabMoves() {
    for (auto it = g_grabMoves.begin(); it != g_grabMoves.end();) {
        const auto sit = g_screens.find(it->first);
        GrabMove &m = it->second;
        if (sit == g_screens.end() || sit->second.drag != Drag::None || sit->second.pinned != kNone ||
            Apart(sit->second.pose, m.lastSet) > 0.001) {
            it = g_grabMoves.erase(it);
            continue;
        }
        Screen &s = sit->second;
        const double ms = std::max(1.0, m.ms > 0 ? m.ms : g_grabThat.params().animMs);
        const double u = std::clamp((NowMs() - m.startMs) / ms, 0.0, 1.0);
        const double t = m.easeOut ? 1 - (1 - u) * (1 - u) * (1 - u) : grabthat::Ease(u);
        if (m.w1 != m.w0) {
            s.metres = std::clamp(m.w0 + (m.w1 - m.w0) * t, kMinWidth, 12.0);
            vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
            CapCurve(s);
            ApplyCurve(s);
        }
        m.lastSet = grabthat::Blend(m.from, m.to, t);
        SetAbsolute(s, m.lastSet);
        if (t < 1) {
            ++it;
            continue;
        }
        FitCurve(s);
        if (!s.floating && !s.remote) ArrangeDesktopSoon();
        it = g_grabMoves.erase(it);
    }
}

// The reset button: the layout puts the screens and remote displays back; the one in the front slot
// goes home first (a floating window stays where that puts it). Sizes set in the slot are forgotten.
void GrabThatReset() {
    for (auto it = g_grabMoves.begin(); it != g_grabMoves.end();) {
        const auto sit = g_screens.find(it->first);
        it = sit == g_screens.end() || !sit->second.floating ? g_grabMoves.erase(it) : std::next(it);
    }
    if (g_front.panel >= 0) FrontHome("the reset button");
    g_frontDeg.clear();
    for (auto &[i, s] : g_screens) s.hasBefore = false;
}

// The reset button may take a hold: pressed for g_resetHoldMs (HANDS_RESET_HOLD_MS) by the same laser
// or hand, the layout resets; let go sooner, nothing. (500 ms from 2026-10-11, after a stray poke reset
// the whole layout; a press again since 2026-10-13: "clicking the reset ... button that's underneath
// doesn't actually do anything", taps of 0.09-0.16 s, and the buttons sit farther apart: ControlUV.)
double g_resetHoldMs = 0;
struct ResetHold {
    int panel = -1;
    vr::TrackedDeviceIndex_t device = kNone;   // a laser's
    int side = -1;                             // ...or a hand's
    int64_t startMs = 0;
} g_resetHold;

void HideResetAsk();
void DoReset(int panel, const char *how) {
    std::printf("screen %d: reset the layout (%s)\n", panel + 1, how);
    HideResetAsk();
    GrabThatReset();
    RunLayout("reset");
}
void ResetHoldStart(int panel, vr::TrackedDeviceIndex_t device, int side) {
    if (g_resetHoldMs <= 0) return DoReset(panel, side >= 0 ? "poked" : "clicked");
    g_resetHold = {panel, device, side, NowMs()};
    std::printf("screen %d: the reset button held (%s): resets in %.1f s\n", panel + 1, side >= 0 ? "a hand" : "a laser", g_resetHoldMs / 1000);
}
void ResetHoldEnd(vr::TrackedDeviceIndex_t device) {
    if (g_resetHold.panel < 0 || g_resetHold.device != device) return;
    std::printf("screen %d: the reset button let go after %.2f s: no reset (hold it)\n", g_resetHold.panel + 1,
                (NowMs() - g_resetHold.startMs) / 1000.0);
    g_resetHold = {};
}
void UpdateResetHold() {
    if (g_resetHold.panel < 0) return;
    if (g_resetHold.side >= 0 && g_handInput.pressControl(g_resetHold.side) != handinput::Control::Reset) {
        std::printf("screen %d: the reset button let go after %.2f s: no reset (hold it)\n", g_resetHold.panel + 1,
                    (NowMs() - g_resetHold.startMs) / 1000.0);
        return void(g_resetHold = {});
    }
    if (NowMs() - g_resetHold.startMs >= g_resetHoldMs) {
        const ResetHold h = g_resetHold;
        g_resetHold = {};
        DoReset(h.panel, h.side >= 0 ? "held by a hand" : "held by a laser");
    }
}

// ---------------------------------------------------------------- the far stretch
// Both palms held out at the same far panel ("grab that" arming at it with both hands), then both closed
// into fists within kFarPairMs of each other: the panel scales round its centre as the fists move apart or
// together, until a hand opens or is lost (user, 2026-10-13: "when you have both palms facing a display
// even if it's far away, if you make two fists at the same time you should be able to resize the display
// even if you're not physically touching it"). "Grab that" doesn't fire meanwhile; a fire while both hands
// are at a panel waits kFarPairMs for the other fist.
//   Easier from the first live test ("functional but a little hard to trigger": fists 33-366 ms apart of
// 400): half an arming hold each (kFarArmed), the panel the line through the palms' midpoint meets when the
// two hands' lines part (hands apart, their lines from the eyes spread round a smaller panel), 600 ms, a
// looser fist, and one camera's view will do (its distance smoothed).
constexpr int64_t kFarPairMs = 600, kFarAimMs = 600, kFarLostMs = 300;
constexpr double kFarArmed = 0.5;   // grab that's progress (0..1 over its arming hold) that counts
constexpr double kFarFist = 1.15;   // grabthat::Features::open at most this: a fist (~0.8; grab that's 1.0)
constexpr double kFarOpen = 1.35;   // ...over this: the hand opened
struct FarStretch {
    int panel = -1;                     // under way (-1: not)
    double sep0 = 0, sep = 0, w0 = 0;   // the fists' distance at the start and now (smoothed), the width then (m)
    int px0 = 0, py0 = 0;               // a floating window's pixels at the start
    int64_t seenNs[2] = {0, 0};
    // before it: each hand's last arming (grab that's aim then, and the panel its line met), and its fist
    grabthat::Fire aim[2];
    int aimPanel[2] = {-1, -1};
    int64_t aimNs[2] = {INT64_MIN / 2, INT64_MIN / 2}, fistNs[2] = {INT64_MIN / 2, INT64_MIN / 2};
    bool fist[2] = {false, false};
    int64_t triedNs = INT64_MIN / 2;    // the fists last looked at (one log line each)
    std::vector<grabthat::Fire> held;   // fires waiting on the other fist
} g_far;

// That side's hand (its most confident), or none.
const handcut::Hand *SideHand(int k) {
    const handcut::Hand *h = nullptr;
    for (const handcut::Hand &c : g_hands.seen())
        if (c.right == (k == 1) && (!h || c.confidence > h->confidence)) h = &c;
    return h;
}

double FistsApart() {
    const handcut::Hand *a = SideHand(0), *b = SideHand(1);
    if (!a || !b) return -1;
    double p[3], q[3];
    a->Palm(p), b->Palm(q);
    return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
}

int64_t NsToMs(int64_t ns) { return ns / 1000000; }

// Both hands armed at a panel lately (grab that's aims, before this frame's).
bool FarPair(int64_t t) {
    return NsToMs(t - g_far.aimNs[0]) <= kFarAimMs && NsToMs(t - g_far.aimNs[1]) <= kFarAimMs;
}

// Both hands are ours from both palms held out (a fist passes through a pinch, and with a hand ray up that
// pinch pressed on the panel: user, 2026-10-13, "I was able to resize and also drag a window with a left
// click at the same time") until the far stretch ends.
bool FarClaims(int64_t t) { return g_far.panel >= 0 || FarPair(t); }

void EndFarStretch(const char *why) {
    const auto it = g_screens.find(g_far.panel);
    if (it != g_screens.end()) {
        Screen &s = it->second;
        std::printf("screen %d: far stretch ended (%s), %.2f m wide (was %.2f)\n", g_far.panel + 1, why, s.metres, g_far.w0);
        if (!s.floating) {
            FitCurve(s);
            if (!s.remote) ArrangeDesktopSoon();
        }
        ForgetIfPlacedFar(g_far.panel, s);   // (out there, that's its size now)
    }
    g_far.panel = -1;
    g_far.fist[0] = g_far.fist[1] = false;
    g_far.aimNs[0] = g_far.aimNs[1] = INT64_MIN / 2;
}

// The panel both fists are for, or -1 (why: for the log).
int FarPanel(std::string *why) {
    const int a = g_far.aimPanel[0], b = g_far.aimPanel[1];
    if (a >= 0 && a == b) return a;
    // the line from between the palms along their normals between them (or from between the eyes through it)
    grabthat::Fire mid = g_far.aim[1];
    for (int q = 0; q < 3; ++q)
        mid.eye[q] = (g_far.aim[0].eye[q] + g_far.aim[1].eye[q]) / 2, mid.palm[q] = (g_far.aim[0].palm[q] + g_far.aim[1].palm[q]) / 2,
        mid.normal[q] = (g_far.aim[0].normal[q] + g_far.aim[1].normal[q]) / 2;
    grabthat::Closest closest;
    const int m = grabthat::Pick(GrabThatTargets(), mid, g_grabThat.params(), &closest);
    if (m >= 0) return m;
    char b2[160];
    std::snprintf(b2, sizeof b2, "the hands at screens %d and %d, between them at none (nearest %d, %.0f deg off)", a + 1, b + 1,
                  closest.index + 1, closest.deg);
    *why = b2;
    return -1;
}

// Each hands frame, after "grab that" took it.
void UpdateFarStretch(int64_t t) {
    const Mat &head = g_hands.captureHead();
    for (int k = 0; k < 2; ++k) {
        const handcut::Hand *h = SideHand(k);
        if (!h) continue;
        g_far.seenNs[k] = t;
        const double open = grabthat::Measure(*h, head).open;
        if (!g_far.fist[k] && open <= kFarFist) g_far.fist[k] = true, g_far.fistNs[k] = t;
        else if (g_far.fist[k] && open > kFarOpen) g_far.fist[k] = false;
    }
    if (g_far.panel < 0) {
        if (!g_far.fist[0] || !g_far.fist[1]) return;
        const int64_t last = std::max(g_far.fistNs[0], g_far.fistNs[1]);
        if (last == g_far.triedNs || NsToMs(t - last) > 200) return;   // (a few frames for a hand's view)
        const int64_t apart = std::llabs(NsToMs(g_far.fistNs[0] - g_far.fistNs[1]));
        std::string why;
        int i = -1;
        // each hand armed at a panel within kFarAimMs before its fist
        bool armed[2];
        for (int k = 0; k < 2; ++k) armed[k] = NsToMs(g_far.fistNs[k] - g_far.aimNs[k]) <= kFarAimMs;
        if (apart > kFarPairMs) why = std::to_string(apart) + " ms apart";
        else if (!armed[0] && !armed[1]) return;   // (fists, not at a panel: not for us, no log)
        else if (!armed[0] || !armed[1]) why = std::string("only the ") + (armed[0] ? "left" : "right") + " hand was held out at a panel";
        else i = FarPanel(&why);
        const double sep = FistsApart();
        Screen *s = nullptr;
        if (i >= 0) {
            const auto it = g_screens.find(i);
            if (it == g_screens.end() || !it->second.visible) why = "the panel went";
            else if (it->second.drag != Drag::None || it->second.pinned != kNone) why = "the panel is held or pinned";
            else if (sep < 0) { if (NsToMs(t - last) < 200) return; why = "a hand not seen"; }
            else if (sep < 0.05) why = "the fists together";
            else s = &it->second;
        }
        g_far.triedNs = last;
        if (!s) return (void)std::printf("far stretch: both fists (%lld ms apart), not started: %s\n", (long long)apart, why.c_str());
        g_far.panel = i, g_far.sep0 = g_far.sep = sep, g_far.w0 = s->metres, g_far.px0 = s->cropW, g_far.py0 = s->cropH;
        g_far.held.clear();
        g_grabMoves.erase(i);
        std::printf("screen %d: far stretch (both fists, %lld ms apart%s), the fists %.2f m apart, %.2f m wide\n", i + 1,
                    (long long)apart, g_far.aimPanel[0] == g_far.aimPanel[1] ? "" : ", aimed between the hands", sep, s->metres);
    }
    g_grabThat.Quiet(t + int64_t(500 * 1e6));   // (not armed meanwhile, nor just after)
    const auto it = g_screens.find(g_far.panel);
    if (it == g_screens.end() || !it->second.visible) return EndFarStretch("the panel went");
    Screen &s = it->second;
    if (s.drag != Drag::None || s.pinned != kNone) return EndFarStretch("taken");
    for (int k = 0; k < 2; ++k) {
        if (NsToMs(t - g_far.seenNs[k]) > kFarLostMs) return EndFarStretch(k ? "the right hand lost" : "the left hand lost");
        if (!g_far.fist[k]) return EndFarStretch(k ? "the right hand opened" : "the left hand opened");
    }
    const double now = FistsApart();
    if (now < 0) return;
    g_far.sep += 0.5 * (now - g_far.sep);   // (one camera's view jumps a centimetre or two)
    const double scale = g_far.sep / g_far.sep0;
    if (s.floating) {
        if (g_far.px0 <= 0 || s.mpp <= 0 || g_tick - s.resizeSent < 4) return;   // about 20 a second
        const int w = std::max(320, int(std::lround(g_far.px0 * scale))), h = std::max(200, int(std::lround(g_far.py0 * scale)));
        if (w == s.resizeW && h == s.resizeH) return;
        s.resizeW = w, s.resizeH = h, s.resizeSent = g_tick;
        s.keepX = 0, s.keepY = 0, s.keepUntil = g_tick + 45;   // (round its centre)
        SendFloat("resize " + std::to_string(g_far.panel + 1) + " " + std::to_string(w) + " " + std::to_string(h));
        return;
    }
    const double w = std::clamp(g_far.w0 * scale, kMinWidth, 12.0);
    if (std::fabs(w - s.metres) < 0.002 * s.metres) return;
    SetWidth(s, w);
}

void UpdateGrabThat(const handinput::Input &in) {
    if (g_handInput.newFrame()) {
        bool allowed[2];
        // (not a hand typing holds back: the helper's "typing" word, as for pokes and near grabs)
        for (int k = 0; k < 2; ++k)
            allowed[k] = in.allowed[k] && !in.typing[k] && !g_handInput.pressing(k) && !g_handInput.grabbing(k);
        const int64_t t = g_hands.captureNs();
        // each hand's arming (before this frame: a fist ends it), for the far stretch
        for (int k = 0; k < 2; ++k) {
            grabthat::Fire f;
            double progress = 0;
            if (g_far.panel < 0 && g_grabThat.params().on && g_grabThat.Aim(k, &f, &progress) && progress >= kFarArmed)
                g_far.aim[k] = f, g_far.aimPanel[k] = g_grabThatAim[k], g_far.aimNs[k] = t;
        }
        g_grabThat.Frame(t, g_hands.seen(), g_hands.captureHead(), allowed);
        for (const grabthat::Fire &f : g_grabThat.fired()) g_far.held.push_back(f);
        UpdateFarStretch(t);
        // both hands at a panel: a fire waits on the other fist (a far stretch drops it)
        const bool pair = g_far.panel < 0 && FarPair(t);
        for (auto it = g_far.held.begin(); it != g_far.held.end();) {
            if (pair && NsToMs(t - it->t) < kFarPairMs) {
                ++it;
                continue;
            }
            const grabthat::Fire f = *it;
            it = g_far.held.erase(it);
            g_handInput.GrabThatFired(f.side);   // a fist waiting on it isn't a fist grab
            if (g_far.panel < 0) GrabThat(f);
        }
    }
    AnimateGrabMoves();
}

// The lazy susan (a stroke, Meta+Alt+Tab: "spin next|prev"): the next screen round, by where each one
// lives, to the slot at reading distance (see "the front slot" above).
void SusanCommand(bool next, char *reply, int size) {
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head))
        return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
    const std::vector<grabthat::Bearing> order = CarouselOrder(head);
    double from = 0;
    for (const grabthat::Bearing &b : order)
        if (b.index == g_front.panel) from = b.deg;
    const int i = grabthat::SusanPick(order, g_front.panel, from, next, kSpinAhead);
    if (i < 0) {
        ShowCarousel();
        return (void)std::snprintf(reply, size, "ok 0 (no other panel)");
    }
    if (g_front.released) FrontHome("restored by the next stroke");
    BringReadable(i, grabthat::InFront(head, g_frontReading, g_frontDrop), true, next ? "the lazy susan, next" : "the lazy susan, back");
    std::snprintf(reply, size, "ok screen %d", i + 1);
}
// "spin to N" (spotwatch) while the lazy susan brings: panel N to the front slot, the rest stay put.
void SusanBring(int i, const Mat &head, char *reply, int size) {
    if (g_front.panel != i) {
        if (g_front.released) FrontHome("restored by spin to");
        BringReadable(i, grabthat::InFront(head, g_frontReading, g_frontDrop), true, "spin to");
    }
    std::snprintf(reply, size, "ok screen %d", i + 1);
}

// ---------------------------------------------------------------- the carousel strip
// For a moment after each stroke (user, 2026-10-11, the proposals: "it wasn't clear which ones it would
// switch to"): every screen the lazy susan goes round, by name, left to right as they live around you,
// under the one in the slot: it lit, the next each way marked with an arrow.
struct Carousel {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int64_t shownMs = -1;
} g_carousel;
constexpr int64_t kCarouselMs = 2500, kCarouselFadeMs = 500;

void FillRound(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, int r, const uint8_t c[4]) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            const int dx = x < x0 + r ? x0 + r - x : x >= x1 - r ? x - (x1 - r - 1) : 0;
            const int dy = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r - 1) : 0;
            const double out = std::sqrt(double(dx * dx + dy * dy)) - r;
            const double a = std::clamp(0.5 - out, 0.0, 1.0) * c[3] / 255.0;
            if (a <= 0) continue;
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (c[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], 255 * a));
        }
}

std::string ShortLabel(const Screen &s, int i) {
    std::string l = s.label.empty() ? "Screen " + std::to_string(i + 1) : s.label;
    for (const char *pre : {"frametop-", "Frametop "})
        if (l.rfind(pre, 0) == 0) l = l.substr(std::strlen(pre));
    if (l.size() > 26) l = l.substr(0, 25) + "\xe2\x80\xa6";   // (an ellipsis)
    return l;
}

void ShowCarousel() {
    Mat head;
    if (g_front.panel < 0 || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;
    const std::vector<grabthat::Bearing> order = CarouselOrder(head);
    if (order.size() < 2) return;
    int at = 0;
    for (size_t k = 0; k < order.size(); ++k)
        if (order[k].index == g_front.panel) at = int(k);
    const int n = int(order.size()), size = 30, padX = 26, gap = 14, H = 64;
    std::vector<std::string> text(static_cast<size_t>(n));
    std::vector<int> widths(static_cast<size_t>(n));
    int W = gap;
    for (int k = 0; k < n; ++k) {
        const Screen &s = g_screens[order[size_t(k)].index];
        std::string t = ShortLabel(s, order[size_t(k)].index);
        if (k == (at + 1) % n) t += "  \xe2\x80\xba";                 // the next (a stroke to the left)
        if (k == (at + n - 1) % n && n > 2) t = "\xe2\x80\xb9  " + t;  // the one before
        text[size_t(k)] = t;
        widths[size_t(k)] = std::max(120, keyboard::LabelWidth(t.c_str(), size) + 2 * padX);
        W += widths[size_t(k)] + gap;
    }
    W = std::min(W, 2048);
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    int x = gap;
    for (int k = 0; k < n && x < W; ++k) {
        const bool lit = k == at;
        const uint8_t bg[4] = {uint8_t(lit ? 240 : 24), uint8_t(lit ? 240 : 24), uint8_t(lit ? 244 : 30), uint8_t(lit ? 235 : 190)};
        const uint8_t fg[3] = {uint8_t(lit ? 20 : 235), uint8_t(lit ? 20 : 235), uint8_t(lit ? 24 : 240)};
        FillRound(px, W, H, x, 6, x + widths[size_t(k)], H - 6, (H - 12) / 2, bg);
        keyboard::DrawLabel(px.data(), W, H, text[size_t(k)].c_str(), size, x + widths[size_t(k)] / 2, H / 2, lit ? 255 : 210, fg);
        x += widths[size_t(k)] + gap;
    }
    if (g_carousel.overlay == vr::k_ulOverlayHandleInvalid &&
        vr::VROverlay()->CreateOverlay("frametop.carousel", "Frametop: screens", &g_carousel.overlay) != vr::VROverlayError_None)
        return;
    const vr::VROverlayHandle_t o = g_carousel.overlay;
    vr::VROverlay()->SetOverlayRaw(o, px.data(), uint32_t(W), uint32_t(H), 4);
    // under the screen in the slot, past its bar: 0.45 mm a pixel at 1 m (30 px text: about 0.8 deg)
    const Screen &s = g_screens[g_front.panel];
    const double d = std::max(0.3, Apart(head, g_front.to));
    const double mpp = 0.00045 * d, pxW = s.floating ? s.cropW : s.width, pxH = s.floating ? s.cropH : s.height;
    const double h = pxW > 0 ? g_front.width * pxH / pxW : 0.3;
    vr::VROverlay()->SetOverlayWidthInMeters(o, float(W * mpp));
    vr::VROverlay()->SetOverlaySortOrder(o, 10);
    const Mat at3 = Mul(g_front.to, Translation(0, -(h / 2 + 0.07 * d + H * mpp / 2), 0.02));
    vr::HmdMatrix34_t m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = at3.m[r][c];
    vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &m);
    vr::VROverlay()->SetOverlayAlpha(o, 1.f);
    vr::VROverlay()->ShowOverlay(o);
    g_carousel.shownMs = NowMs();
}

void UpdateCarousel() {
    if (g_carousel.shownMs < 0 || g_carousel.overlay == vr::k_ulOverlayHandleInvalid) return;
    const int64_t age = NowMs() - g_carousel.shownMs;
    if (age >= kCarouselMs || g_front.panel < 0) {
        vr::VROverlay()->HideOverlay(g_carousel.overlay);
        g_carousel.shownMs = -1;
    } else if (age > kCarouselMs - kCarouselFadeMs) {
        vr::VROverlay()->SetOverlayAlpha(g_carousel.overlay, float(double(kCarouselMs - age) / kCarouselFadeMs));
    }
}

// ---------------------------------------------------------------- the reset confirmation
// A hand's tap on the reset button asks first (user, 2026-10-13: "add a confirmation dialogue you have to
// tap to reset the displays as well ONLY for while you're doing hand tracking"): "Reset the layout?" shows
// under the button for kResetAskMs, and a tap on it (or on the button again) resets. A laser's click
// resets straight away.
struct ResetAsk {
    int panel = -1;
    int64_t shownMs = -1;
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    double w = 0, h = 0;                  // its size now (m)
    Mat pose{};                           // ...and where it is
    double tipZ[2] = {1, 1};              // each index tip's way in front of it last frame (m; 1: away)
    int px = 0;                           // its width in pixels
} g_resetAsk;
constexpr int64_t kResetAskMs = 5000, kResetAskLeastMs = 300;
constexpr int kResetAskH = 84;
constexpr const char *kResetAskText = "Reset the layout? Tap";

void HideResetAsk() {
    if (g_resetAsk.overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(g_resetAsk.overlay);
    g_resetAsk.panel = -1, g_resetAsk.shownMs = -1;
}

// Under the reset button, a button's height lower, and at least a finger's size.
bool PlaceResetAsk(Screen &s) {
    Mat p, head;
    if (!ScreenPose(s, &p)) return false;
    const double d = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ? std::max(0.3, Apart(head, p)) : 1.0;
    g_resetAsk.h = std::max(0.00045 * d * kResetAskH, 1.4 * s.grip);
    g_resetAsk.w = g_resetAsk.h * g_resetAsk.px / kResetAskH;
    double u[7], v[7];
    ControlUV(s, u, v);
    const double at = v[6] - s.grip / 2 - std::max(s.grip, s.chrome * 0.06) - g_resetAsk.h / 2;
    g_resetAsk.pose = Mul(p, OnSurface(s, u[6], at, 0.004));
    const vr::VROverlayHandle_t o = g_resetAsk.overlay;
    vr::VROverlay()->SetOverlayWidthInMeters(o, float(g_resetAsk.w));
    vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &g_resetAsk.pose);
    return true;
}

void ShowResetAsk(int panel) {
    const auto it = g_screens.find(panel);
    if (it == g_screens.end()) return;
    const int H = kResetAskH, W = keyboard::LabelWidth(kResetAskText, 32) + 2 * 40;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    const uint8_t bg[4] = {196, 52, 48, 240}, fg[3] = {255, 255, 255};
    FillRound(px, W, H, 2, 2, W - 2, H - 2, (H - 4) / 2, bg);
    keyboard::DrawLabel(px.data(), W, H, kResetAskText, 32, W / 2, H / 2, 255, fg);
    if (g_resetAsk.overlay == vr::k_ulOverlayHandleInvalid &&
        vr::VROverlay()->CreateOverlay("frametop.resetask", "Frametop: reset the layout?", &g_resetAsk.overlay) != vr::VROverlayError_None)
        return;
    vr::VROverlay()->SetOverlayRaw(g_resetAsk.overlay, px.data(), uint32_t(W), uint32_t(H), 4);
    vr::VROverlay()->SetOverlaySortOrder(g_resetAsk.overlay, 10);
    g_resetAsk.panel = panel, g_resetAsk.shownMs = NowMs(), g_resetAsk.px = W;
    g_resetAsk.tipZ[0] = g_resetAsk.tipZ[1] = 1;
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    vr::VROverlay()->ShowOverlay(g_resetAsk.overlay);
}

void ResetTapped(int panel, int side) {
    const int64_t age = NowMs() - g_resetAsk.shownMs;
    if (g_resetAsk.panel == panel && age < kResetAskLeastMs) return;   // (the same tap)
    if (g_resetAsk.panel == panel) {
        HideResetAsk();
        return DoReset(panel, side ? "the right hand, the button twice" : "the left hand, the button twice");
    }
    ShowResetAsk(panel);
    std::printf("screen %d: reset the layout? (a hand's tap: asks, %.0f s)\n", panel + 1, kResetAskMs / 1000.0);
}

// Each tick: it follows its panel, and goes after kResetAskMs; each hands frame, an index tip coming through
// it from the front is the tap.
void UpdateResetAsk() {
    if (g_resetAsk.panel < 0) return;
    const auto it = g_screens.find(g_resetAsk.panel);
    if (it == g_screens.end() || !it->second.visible || NowMs() - g_resetAsk.shownMs >= kResetAskMs) {
        if (it != g_screens.end()) std::printf("screen %d: reset the layout? not tapped: no reset\n", g_resetAsk.panel + 1);
        return HideResetAsk();
    }
    if (!PlaceResetAsk(it->second)) return HideResetAsk();
    if (!g_handInput.newFrame()) return;
    const Mat in = Inverse(g_resetAsk.pose);
    for (int k = 0; k < 2; ++k) {
        const handcut::Hand *h = SideHand(k);
        double z = 1;
        if (h) {
            const float *q = h->p[8];
            double l[3];
            for (int r = 0; r < 3; ++r) l[r] = in.m[r][0] * q[0] + in.m[r][1] * q[1] + in.m[r][2] * q[2] + in.m[r][3];
            const double margin = 0.01;
            if (std::fabs(l[0]) <= g_resetAsk.w / 2 + margin && std::fabs(l[1]) <= g_resetAsk.h / 2 + margin && l[2] > -0.03) z = l[2];
        }
        const bool through = g_resetAsk.tipZ[k] > 0.008 && g_resetAsk.tipZ[k] < 0.08 && z <= 0.003;
        g_resetAsk.tipZ[k] = z;
        if (through && NowMs() - g_resetAsk.shownMs >= kResetAskLeastMs) {
            const int panel = g_resetAsk.panel;
            HideResetAsk();
            return DoReset(panel, k ? "the right hand, confirmed" : "the left hand, confirmed");
        }
    }
}

// Each tick: the one in the slot goes home when you turn away from it (unless kept: clicks, scrolls, drags or
// keys on it add up to g_frontKeepMs; a pointer passing over it doesn't, user 2026-10-11: gaze kept every one); moved elsewhere,
// it leaves the slot and lives there; resized there, that's its size in the slot from now on.
void UpdateFront() {
    UpdateCarousel();
    UpdateResetAsk();
    if (g_front.panel < 0) return;
    const auto it = g_screens.find(g_front.panel);
    if (it == g_screens.end()) return void(g_front = {});
    const int64_t now = NowMs();
    const double dt = g_front.lastTickMs ? double(now - g_front.lastTickMs) : 0;
    g_front.lastTickMs = now;
    if (g_front.released || g_grabMoves.count(g_front.panel)) return;   // (yours; or still on its way)
    Screen &s = it->second;
    Mat p, head;
    if (!ScreenPose(s, &p)) return;
    const int i = g_front.panel;
    if (s.pinned != kNone) return FrontRelease(i, "pinned");
    const bool held = s.drag != Drag::None || g_handInput.heldByHand(i);
    if (held && Apart(p, g_front.to) > 0.03) return FrontRelease(i, "carried by hand or laser");
    if (!held && std::fabs(s.metres - g_front.width) > 0.01 * g_front.width && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        // resized there (two fists, a corner): that's its size in the slot, here and next time
        const double d = std::max(0.2, Apart(head, p));
        g_frontDeg[i] = (s.curve > 0 ? s.metres / d : 2 * std::atan(s.metres / (2 * d))) * 180 / M_PI;
        std::printf("front: screen %d resized in the slot: %.2f m, %.0f deg wide from now on\n", i + 1, s.metres, g_frontDeg[i]);
        g_front.width = s.metres, g_front.to = p;
    } else if (!held && Apart(p, g_front.to) > 0.05) {
        return FrontRelease(i, "moved");
    }
    // used a while: kept (input on it within the last moment counts as use)
    if (!g_front.kept && g_frontKeepMs > 0 && (held || now - s.useMs < 250 || now - g_keyMs < 250)) {
        g_front.usedMs += dt;
        if (g_front.usedMs >= g_frontKeepMs) {
            g_front.kept = true;
            std::printf("front: screen %d kept (used %.1f s): it stays when you look away\n", i + 1, g_front.usedMs / 1000);
        }
    }
    if (!g_frontAway || held || g_front.kept || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        g_front.awaySinceMs = -1;
        return;
    }
    if (std::fabs(BearingFrom(head, p)) <= kFrontAway) {
        g_front.awaySinceMs = -1;
    } else if (g_front.awaySinceMs < 0) {
        g_front.awaySinceMs = now;
    } else if (now - g_front.awaySinceMs >= kFrontAwayMs) {
        FrontHome("out of view");
    }
}

// The glow on the panel a gesture under way would take (user, 2026-10-10: "a glow over the window
// that grab that is targeting, so the user knows the motion will trigger on it"), and after the fourth
// live test ("The glow is too powerful. It should be a white glow and far more subtle. It should only be
// around the outside of the window."): a soft white halo outside the panel's edges, about 1.3 degrees
// wide, nothing over its content; one overlay ("frametop.guide.target": the pointer helper's cursor and
// rays pass through guides), sized and curved to the panel, in its plane, drawn after it. "Grab that" (an
// open hand held out to it): it fades in as the gesture arms, over armMs, and goes as it's let go or
// fires; a push or a flick would throw it (an open palm close in front of it, toward it): dimmer.
// HANDS_TARGET_GLOW.
bool g_targetGlowOn = true;
bool g_pushGlowOn = false;

// An overlay event from a laser: a controller's, or our own (ft_pointer's) while a hand ray is up
// (the pointer helper's "ray"; else it's the mouse's or the gaze's, which sits on a panel all the
// time). SteamVR also sends panels events from the headset and from no device at all. (2026-10-13:
// those, then the mouse's, kept the grab-that glow off the panels.)
// ft_pointer's (our 3D mouse: the mouse's, the gaze's, or a hand ray's): 1; another controller's: -1;
// not a controller: 0. Looked up every 10 s.
int OurPointer(uint32_t d) {
    if (d >= vr::k_unMaxTrackedDeviceCount || vr::VRSystem()->GetTrackedDeviceClass(d) != vr::TrackedDeviceClass_Controller) return 0;
    static int64_t ours[vr::k_unMaxTrackedDeviceCount] = {};
    static int64_t lookedMs[vr::k_unMaxTrackedDeviceCount] = {};
    if (!ours[d] || NowMs() - lookedMs[d] > 10000) {
        char type[64] = "";
        vr::VRSystem()->GetStringTrackedDeviceProperty(d, vr::Prop_ControllerType_String, type, sizeof type);
        ours[d] = std::strcmp(type, "ft_pointer") ? -1 : 1, lookedMs[d] = NowMs();
    }
    return int(ours[d]);
}
// ft_pointer while a hand ray is up (the pointer helper's "ray", every second).
bool HandRay(uint32_t d) {
    return OurPointer(d) > 0 && NowMs() - g_rayWord.atMs < 3000 && (g_rayWord.side[0] || g_rayWord.side[1]);
}
bool FromLaser(const vr::VREvent_t &ev) {
    const int ours = OurPointer(ev.trackedDeviceIndex);
    return ours < 0 || (ours > 0 && HandRay(ev.trackedDeviceIndex));
}
struct TargetGlow {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int panel = -1, texW = 0, texH = 0, texM = 0;
    bool shown = false;
    double alpha = 0, shownAlpha = -1;
    int64_t lastMs = 0;
} g_target;

double HeadDistance(const Mat &p) {
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return 1;
    return Apart(head, p);
}

void UpdateTargetGlow() {
    int best = -1;
    double want = 0;
    if (g_targetGlowOn && g_vr) {
        // "grab that": the panel it would take now, as far along as the gesture is
        for (int k = 0; k < 2; ++k)
            if (g_grabThatAim[k] >= 0 && g_grabThatProgress[k] > want) best = g_grabThatAim[k], want = g_grabThatProgress[k];
        if (g_far.panel >= 0) best = g_far.panel, want = 1;   // (a far stretch: the one it sizes)
        // a push or a flick (dimmer; HANDS_PUSH_GLOW, off since 2026-10-11: "a near display that can't be
        // pulled shouldn't have the white overlay when your palm faces it: it makes clicking annoying")
        if (best < 0 && g_pushGlowOn)
            for (const auto &[i, a] : g_handInput.throwAims())
                if (0.6 * a > want) best = i, want = 0.6 * a;
    }
    const auto it = best >= 0 ? g_screens.find(best) : g_screens.end();
    if (it == g_screens.end() || !it->second.visible) best = -1, want = 0;
    // not over a panel a laser is on (user, 2026-10-11: no pull highlight while the laser points at it; the
    // gesture still works there)
    else if (NowMs() - it->second.laserMs < 400) {
        static int heldBack = -1;
        if (heldBack != best) {
            char name[64] = "?";
            if (it->second.laserDevice < vr::k_unMaxTrackedDeviceCount && g_vr)
                vr::VRSystem()->GetStringTrackedDeviceProperty(it->second.laserDevice, vr::Prop_ControllerType_String, name, sizeof name);
            std::printf("grab that: no glow on screen %d, a laser is on it (device %u, %s)\n", best + 1, it->second.laserDevice, name);
            heldBack = best;
        }
        best = -1, want = 0;
    }
    // fade: in over about 120 ms (on top of grab that's own arming), out over 150
    const int64_t now = NowMs();
    const double dt = g_target.lastMs ? std::clamp((now - g_target.lastMs) / 1000.0, 0.0, 0.1) : 0;
    g_target.lastMs = now;
    if (best >= 0) g_target.panel = best;
    g_target.alpha = want > g_target.alpha ? std::min(want, g_target.alpha + dt / 0.12) : std::max(want, g_target.alpha - dt / 0.15);
    auto hide = [&] {
        if (g_target.shown) vr::VROverlay()->HideOverlay(g_target.overlay), g_target.shown = false;
    };
    const auto pit = g_screens.find(g_target.panel);
    if (g_target.alpha < 0.01 || pit == g_screens.end() || !pit->second.visible) return hide();
    if (g_target.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay("frametop.guide.target", "Frametop: gesture target", &g_target.overlay) != vr::VROverlayError_None)
            return;
        vr::VROverlay()->SetOverlaySortOrder(g_target.overlay, 1);   // (after the panels, under their controls)
    }
    Screen &s = pit->second;
    Mat p;
    if (!ScreenPose(s, &p)) return hide();
    // the panel kTargetW - 2m pixels wide in it, m the halo's width
    constexpr int kTargetW = 512;
    const double W = s.metres, H = s.heightMetres(), halo = softglow::Width(HeadDistance(p));
    const int m = std::clamp(int(std::lround(kTargetW * halo / (W + 2 * halo))), 3, 48);
    const int h = std::clamp(int(std::lround((kTargetW - 2 * m) * H / std::max(1e-6, W))) + 2 * m, 2 * m + 4, 2048);
    if (kTargetW != g_target.texW || h != g_target.texH || m != g_target.texM) {
        const std::vector<uint8_t> px = softglow::TargetTexture(kTargetW, h, m);
        vr::VROverlay()->SetOverlayRaw(g_target.overlay, const_cast<uint8_t *>(px.data()), kTargetW, uint32_t(h), 4);
        g_target.texW = kTargetW, g_target.texH = h, g_target.texM = m;
        static int logged = -1;
        if (logged != g_target.panel)
            std::printf("grab that: glow on screen %d, %dx%d (halo %d px), %.2f x %.2f m\n", g_target.panel + 1, kTargetW, h, m, W, H);
        logged = g_target.panel;
    }
    const double width = W * kTargetW / double(kTargetW - 2 * m);
    vr::VROverlay()->SetOverlayWidthInMeters(g_target.overlay, float(width));
    vr::VROverlay()->SetOverlayCurvature(g_target.overlay, s.curve > 0 ? float(std::clamp(width / (2 * M_PI * s.curve), 0.0, 1.0)) : 0.f);
    vr::VROverlay()->SetOverlayTransformAbsolute(g_target.overlay, vr::TrackingUniverseStanding, &p);
    if (std::fabs(g_target.alpha - g_target.shownAlpha) > 0.01) {
        vr::VROverlay()->SetOverlayAlpha(g_target.overlay, float(g_target.alpha));
        g_target.shownAlpha = g_target.alpha;
    }
    if (!g_target.shown) vr::VROverlay()->ShowOverlay(g_target.overlay), g_target.shown = true;
}

// Each hand's soft glow at a panel (handinput's guide(), Params::edgeGlow; user, after the fourth live
// test: "for resizing, I'd like the indicators to be almost like a cutout of the soft glow we had for
// highlighting, just around the edges and corners when you're within the interaction zone"): the same
// white halo, only round the corner a pinch there would resize from (an L along its two edges), along
// the edge it would move the panel by (a short segment), or round a button (a ring); brighter as the
// fingers come closer, brightest while resizing from that corner or pressing that button. One overlay
// per hand ("frametop.guide.hand.left/right"), in the panel's plane, clear over the panel, so it shows
// with the cutouts off. HANDS_EDGE_GLOW (off: the resize handle follows the hand, as before).
struct HandGuide {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    std::string key;           // the texture on it
    bool shown = false;
    double alpha = 0, shownAlpha = -1;
    int64_t lastMs = 0;
    handinput::Guide g;        // what it shows (kept while it fades out)
} g_handGuides[3];   // the left hand's, the right's, and the lasers' (a corner: g_laserCorner)

void UpdateHandGuides() {
    static std::map<std::string, std::vector<uint8_t>> textures;
    const bool on = g_vr && g_handInput.params().edgeGlow && HandInputOn();
    const int64_t now = NowMs();
    for (int k = 0; k < 3; ++k) {
        HandGuide &hg = g_handGuides[k];
        const handinput::Guide g = k == 2 ? (g_vr ? g_laserCorner : handinput::Guide{}) : on ? g_handInput.guide(k) : handinput::Guide{};
        const double dt = hg.lastMs ? std::clamp((now - hg.lastMs) / 1000.0, 0.0, 0.1) : 0;
        hg.lastMs = now;
        const bool same = g.kind == hg.g.kind && g.panel == hg.g.panel && g.sx == hg.g.sx && g.sy == hg.g.sy && g.control == hg.g.control;
        double want = g.kind != handinput::Guide::None ? std::clamp(g.strength, 0.0, 1.0) : 0;
        if (g.kind != handinput::Guide::None && (same || hg.alpha < 0.02)) hg.g = g;   // (a new one when the old has faded)
        else if (g.kind != handinput::Guide::None) want = 0;                         // ...first fade the old out
        hg.alpha = want > hg.alpha ? std::min(want, hg.alpha + dt / 0.08) : std::max(want, hg.alpha - dt / 0.15);
        auto hide = [&] {
            if (hg.shown) vr::VROverlay()->HideOverlay(hg.overlay), hg.shown = false;
        };
        // on one of our screens, or on one of SteamVR's panels (see "SteamVR's panels by hand")
        const auto it = g_screens.find(hg.g.panel);
        const SteamPanel *sp = it == g_screens.end() ? FindSteam(hg.g.panel) : nullptr;
        const bool there = it != g_screens.end() ? it->second.visible : sp && sp->visible && sp->fit.ok;
        if (hg.alpha < 0.01 || hg.g.kind == handinput::Guide::None || !there) {
            hide();
            continue;
        }
        if (hg.overlay == vr::k_ulOverlayHandleInvalid) {
            const char *key = k == 2 ? "frametop.guide.laser" : k ? "frametop.guide.hand.right" : "frametop.guide.hand.left";
            if (vr::VROverlay()->CreateOverlay(key, "Frametop: hand guide", &hg.overlay) != vr::VROverlayError_None) continue;
            vr::VROverlay()->SetOverlaySortOrder(hg.overlay, 1);
        }
        Screen *s = sp ? nullptr : &it->second;
        Mat p = sp ? sp->fit.pose : Identity();
        if (s && !ScreenPose(*s, &p)) {
            hide();
            continue;
        }
        const double W = s ? s->metres : sp->fit.width, H = s ? s->heightMetres() : sp->fit.height, curve = s ? s->curve : 0;
        double halo = std::round(softglow::Width(HeadDistance(p)) * 1000) / 1000;   // (by the mm: fewer textures)
        // its size and place on the panel
        double u = hg.g.u, v = hg.g.v, wm = 0, hm = 0, arm = 0, r = 0, edge = 1e9;
        int tw = 128, th = 128;
        bool curved = false;
        switch (hg.g.kind) {
            case handinput::Guide::Corner:
                if (k == 2 && s) halo = std::max(halo, HandleSize(*s));   // (the laser's: as wide as the handle it presses)
                arm = std::clamp(0.25 * std::min(W, H), 0.03, 0.10);
                u = hg.g.sx * W / 2, v = hg.g.sy * H / 2, wm = hm = 2 * std::max(arm, halo) * 1.05, curved = true;
                break;
            case handinput::Guide::Edge: {
                const double along = hg.g.sy ? W : H;
                arm = std::clamp(0.3 * along, 0.05, 0.16);
                if (hg.g.sy) {
                    u = along > arm ? std::clamp(u, -W / 2 + arm / 2, W / 2 - arm / 2) : 0, v = hg.g.sy * H / 2;
                    wm = arm, hm = 2 * halo, curved = true, th = std::max(8, int(std::lround(tw * hm / wm)));
                } else {
                    v = along > arm ? std::clamp(v, -H / 2 + arm / 2, H / 2 - arm / 2) : 0, u = hg.g.sx * W / 2;
                    wm = 2 * halo, hm = arm, tw = std::max(8, int(std::lround(th * wm / hm)));
                }
                break;
            }
            case handinput::Guide::Button: {
                r = std::max(1e-3, hg.g.size);
                const double hb = std::clamp(r, 0.005, halo);
                wm = hm = 2 * (r + hb), tw = th = 64;
                edge = -H / 2 - v;   // (the panel's bottom edge, over a button under it)
                halo = hb;
                break;
            }
            case handinput::Guide::Touch:   // the dot: its halo in proportion, so one texture each way
                r = std::max(1e-3, hg.g.size), halo = 0.7 * r, wm = hm = 2 * (2.2 * r), tw = th = 64;
                break;
            default: break;
        }
        char key[160];
        std::snprintf(key, sizeof key, "%d %d %d %d %d %d %.0f %.0f %.0f %.0f", int(hg.g.kind), hg.g.sx, hg.g.sy, hg.g.selected, tw, th,
                      halo / wm * 200, arm / wm * 200, r / wm * 200, std::min(edge, wm) / wm * 200);
        if (hg.key != key) {
            auto t = textures.find(key);
            if (t == textures.end()) {
                if (textures.size() > 48) textures.clear();
                t = textures.emplace(key, softglow::GuideTexture(hg.g, tw, th, wm, halo, arm, r, edge)).first;
            }
            vr::VROverlay()->SetOverlayRaw(hg.overlay, const_cast<uint8_t *>(t->second.data()), uint32_t(tw), uint32_t(th), 4);
            hg.key = key;
        }
        vr::VROverlay()->SetOverlayWidthInMeters(hg.overlay, float(wm));
        vr::VROverlay()->SetOverlayCurvature(hg.overlay, curved && curve > 0 ? float(std::clamp(wm / (2 * M_PI * curve), 0.0, 1.0)) : 0.f);
        // (the dot just over the content; on SteamVR's panels all of it a little out, over theirs)
        const double out = hg.g.kind == handinput::Guide::Touch ? 0.003 : 0;
        const Mat m = Mul(p, s ? OnSurface(*s, u, v, out) : Translation(u, v, out + 0.003));
        vr::VROverlay()->SetOverlayTransformAbsolute(hg.overlay, vr::TrackingUniverseStanding, &m);
        if (std::fabs(hg.alpha - hg.shownAlpha) > 0.01) {
            vr::VROverlay()->SetOverlayAlpha(hg.overlay, float(hg.alpha));
            hg.shownAlpha = hg.alpha;
        }
        if (!hg.shown) vr::VROverlay()->ShowOverlay(hg.overlay), hg.shown = true;
    }
}

// The hand interactions Input Settings switches on and off live ("handsetting KEY 0|1"); each also read
// from frametop.conf at start (ft_vr_init). A number-valued one goes back to its default when switched on.
bool SetHandSetting(const char *key, bool on) {
    handinput::Params &k = g_handInput.params();
    const handinput::Params d;
    const std::string n = key;
    if (n == "HANDS_PINCH_CLICK") k.pinchClick = on;
    else if (n == "HANDS_CARD_PINCH") k.cardPinch = on;
    else if (n == "HANDS_TWO_FINGER_SCROLL") k.twoFingerScroll = on;
    else if (n == "HANDS_TOUCH_DOT") k.touchDot = on;
    else if (n == "HANDS_TAP_LOCK") k.lockOut = on ? d.lockOut : 0;
    else if (n == "HANDS_MOTION_PRESS") k.motionPress = on;
    else if (n == "HANDS_LOCK_PUSH") k.lockPushDepth = on ? d.lockPushDepth : 0;
    else if (n == "HANDS_GRAB_ROTATE") k.grabRotate = on;
    else if (n == "HANDS_STRETCH_TURN") k.stretchTurn = on;
    else if (n == "HANDS_FIST_GRAB") k.fistGrab = on;
    else if (n == "HANDS_FIST_PAIR") k.fistPair = on;
    else if (n == "HANDS_FIST_RESIZE_ONLY") k.fistResizeOnly = on;
    else if (n == "HANDS_PINCH_BUTTONS") k.pinchButtons = on;
    else if (n == "HANDS_EDGE_GLOW") k.edgeGlow = on;
    else if (n == "HANDS_RELEASE_FREEZE") k.openFreeze = on ? d.openFreeze : 0;
    else if (n == "HANDS_FLICK") k.flick = on;
    else if (n == "HANDS_PUSH") k.push = on;
    else if (n == "HANDS_THROW") k.cardThrow = on;
    else if (n == "HANDS_SWEEP") k.sweep = on;
    else if (n == "HANDS_SWEEP_BACK") k.sweepBack = on;
    else if (n == "HANDS_TRACK_GATE") k.gate = on;
    else if (n == "HANDS_GRAB_THAT") g_grabThat.params().on = on;
    else if (n == "HANDS_TARGET_GLOW") g_targetGlowOn = on;
    else if (n == "HANDS_PUSH_GLOW") g_pushGlowOn = on;
    else if (n == "HANDS_PALM_MENU") g_palm.params().on = on;
    else if (n == "SCREEN_SPIN_BRING") g_susanBring = on;
    else if (n == "SCREEN_FRONT_AWAY") g_frontAway = on;
    else return false;
    return true;
}

void UpdateHandInput(void (*handle)(const struct ft_event *, void *), void *data) {
    std::vector<handinput::Panel> panels;
    for (auto &[i, s] : g_screens) {
        Mat p;
        if (s.visible && s.width > 0 && s.height > 0 && (!s.floating || s.cropW > 0) && ScreenPose(s, &p))
            panels.push_back(InputPanel(s, i, p));
    }
    handinput::Input in;
    in.nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    in.hands = &g_hands;
    in.haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &in.head);
    for (int k = 0; k < 2; ++k) {
        in.allowed[k] = in.haveHead && SideAllowed(k);
        static bool heldOn[2] = {false, false};
        const bool holding = g_handInput.grabbing(k) || g_handInput.pressing(k);
        if (!in.allowed[k] && in.haveHead && holding && HoldsThroughWord(k)) {
            in.allowed[k] = true;
            if (!heldOn[k])
                std::printf("hands: %s hand \"off\" in the pointer helper's word while it %s: held on\n", k ? "right" : "left",
                            g_handInput.grabbing(k) ? ("holds screen " + std::to_string(g_handInput.grabPanel(k) + 1)).c_str() : "presses");
            heldOn[k] = true;
        } else if (in.allowed[k] || !holding) {
            heldOn[k] = false;
        }
    }
    for (int k = 0; k < 2; ++k) in.typing[k] = NowMs() - g_typingWord.atMs < 3000 && g_typingWord.side[k];
    in.pointerBusy = g_press.buttons && !IsHandHolder(g_press.device);
    // SteamVR's own panels near the hands (see "SteamVR's panels by hand")
    if (HandInputOn()) {
        ScanSteamPanels(in);
        SteamInputPanels(panels);
    }
    // KWin's button, let go of by someone else (ReleaseStuck: its screen hid, or paused)
    for (int k = 0; k < 2; ++k)
        if (g_handInput.pressingKwin(k) && !(g_press.buttons && g_press.device == kHandHolder[k])) g_handInput.ForgetPress(k);
    UpdatePalmMenu(in, panels);
    for (int k = 0; k < 2; ++k) in.allowed[k] = in.allowed[k] && !g_palm.shown(k);   // the palm menu has that hand
    // a fist or pinch waits on "grab that" only while it aims at a panel it would take (one at arm's
    // reach it doesn't: the fourth live test's fist at the near panel waited, then clicked)
    UpdateGrabThatAims();
    for (int k = 0; k < 2; ++k) in.grabThatArmed[k] = g_grabThat.params().on && g_grabThat.armed(k) && g_grabThatAim[k] >= 0;
    g_handInput.Update(in, panels);
    for (const std::string &l : g_handInput.logs()) std::printf("hands: %s\n", l.c_str());
    UpdateGrabThat(in);
    UpdateGrabThatAims();
    UpdateTargetGlow();
    UpdateHandGuides();

    std::set<int> letGo, thrown;   // panels a hand let go of, and ones thrown, this tick
    for (const handinput::Event &e : g_handInput.events()) {
        if (e.type == handinput::Event::Spin) {   // the lazy susan by hand: swept left, the panel on the right comes (next)
            char reply[96];
            SpinCommand(e.x < 0 ? "next" : "prev", reply, sizeof reply);
            std::printf("lazy susan by hand (%s, swept %s): %s\n", e.side ? "right" : "left", e.x < 0 ? "left" : "right", reply);
            continue;
        }
        if (SteamEvent(e)) continue;   // one of SteamVR's panels: the pointer helper's laser does it
        const auto it = g_screens.find(e.panel);
        if (it == g_screens.end()) continue;
        Screen &s = it->second;
        const vr::TrackedDeviceIndex_t holder = kHandHolder[e.side];
        ft_event f{};
        f.screen = e.panel;
        f.x = e.x, f.y = e.y;
        switch (e.type) {
            case handinput::Event::Motion:
                s.inputMs = s.handPointerMs = NowMs();
                if (g_press.buttons && g_press.device == holder) g_press.screen = e.panel, g_press.x = e.x, g_press.y = e.y;
                f.type = FT_MOTION;
                handle(&f, data);
                break;
            case handinput::Event::Press:
                s.inputMs = s.handPointerMs = s.useMs = NowMs();
                PressDown(holder, BTN_LEFT, e.panel, e.x, e.y);
                f.type = FT_BUTTON, f.button = BTN_LEFT, f.pressed = true;
                f.controller = true;   // the click filter holds a poke's wobble still (controller-click.h)
                handle(&f, data);
                break;
            case handinput::Event::Spin: break;   // (handled above)
            case handinput::Event::Settle:   // a let-go's rewind (handinput Params::rewindMs): just after its GrabEnd
                if (s.drag == Drag::None && s.pinned == kNone) SetAbsolute(s, e.to);
                break;
            case handinput::Event::Scroll:
                s.inputMs = s.useMs = NowMs();
                f.type = FT_SCROLL, f.dx = e.x, f.dy = e.y;
                handle(&f, data);
                break;
            case handinput::Event::Release:
                if (g_press.device == holder && (g_press.buttons & ButtonBit(BTN_LEFT))) {
                    g_press.buttons &= ~ButtonBit(BTN_LEFT);
                    if (!g_press.buttons) g_press.upAt = -1;
                }
                f.type = FT_BUTTON, f.button = BTN_LEFT, f.pressed = false, f.controller = true;
                s.handPointerMs = NowMs();
                handle(&f, data);
                break;
            case handinput::Event::Control:
                s.inputMs = NowMs();
                std::printf("screen %d: %s (%s hand)\n", e.panel + 1, handinput::ControlName(e.control), e.side ? "right" : "left");
                if (e.control == handinput::Control::Curve) {
                    ToggleCurve(s);
                } else if (e.control == handinput::Control::Reset) {
                    ResetTapped(e.panel, e.side);   // (a hand: it asks first)
                } else if (e.control == handinput::Control::Dock || e.control == handinput::Control::Close) {
                    SendFloat((e.control == handinput::Control::Dock ? "dock " : "close ") + std::to_string(e.panel + 1));
                }
                break;
            case handinput::Event::GrabStart:
                // the handle goes to that corner (not while the other hand holds it: a stretch)
                if ((e.zone == handinput::Zone::Tab || e.zone == handinput::Zone::Corner) && s.drag == Drag::None) {
                    s.keepX = -e.sx, s.keepY = -e.sy;   // (a floating window's opposite corner stays put)
                    if (!g_handInput.params().edgeGlow) s.cornerX = e.sx, s.cornerY = e.sy, PlaceChrome(s);   // (else its glow shows it)
                }
                RememberPlace(e.panel, s);
                StartHandDrag(s, e.side, e.zone);
                break;
            case handinput::Event::Throw: {
                // a flick at it, or a carry let go fast (after its GrabEnd: let go, unless it went back
                // on a wrist): out to throwDistance, easing out, at its width: that's its place now (the
                // front slot's one too: released, until the next stroke puts it home)
                if (s.drag != Drag::None || s.pinned != kNone) break;
                thrown.insert(e.panel);
                if (SendBack(e.panel, s, e.to, e.ms, e.push ? "pushed away" : e.flick ? "flicked away" : "thrown")) {
                    g_grabThat.Quiet(g_handInput.frameNs() + int64_t(g_grabThat.params().quietAfterSendMs * 1e6));
                    break;
                }
                FrontRelease(e.panel, e.push ? "pushed away" : e.flick ? "flicked away" : "thrown");   // (yours: it flies)
                Mat head;
                double far = 0;
                if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) far = Apart(head, e.to);
                std::printf("screen %d: %s, %.1f m out\n", e.panel + 1, e.push ? "pushed away" : e.flick ? "flicked away" : "thrown", far);
                StartGrabMove(e.panel, s, e.to, s.metres, e.ms, true);
                // (the hand that sent it held out open and pulled back closing isn't "grab that")
                g_grabThat.Quiet(g_handInput.frameNs() + int64_t(g_grabThat.params().quietAfterSendMs * 1e6));
                break;
            }
            case handinput::Event::GrabEnd:
                if (s.drag == Drag::None || s.dragDevice != holder) break;
                if (g_handInput.grabPanel(1 - e.side) == e.panel) {   // two hands to one
                    s.dragDevice = kHandHolder[1 - e.side];
                    if (s.drag == Drag::Resize) s.drag = Drag::Move;
                    ApplyAlpha(s);
                } else {
                    FinishDrag(s, e.panel);
                    letGo.insert(e.panel);
                }
                break;
        }
    }
    // put down (not thrown) out past kPlacedFar: it lives there now
    for (int i : letGo) {
        const auto it = g_screens.find(i);
        if (it != g_screens.end() && !thrown.count(i)) ForgetIfPlacedFar(i, it->second);
    }
    for (const handinput::Carry &c : g_handInput.carries()) {
        const auto it = g_screens.find(c.panel);
        if (it == g_screens.end()) continue;
        Screen &s = it->second;
        if (s.drag == Drag::None || !IsHandHolder(s.dragDevice)) continue;   // a laser took it, or it was let go
        if (c.kind == handinput::Carry::Resize) {
            if (s.floating) {
                ResizeFloat(s, c.panel, c.hx, c.hy, c.grabX, c.grabY, c.sx, c.sy);
            } else if (c.width > 0) {   // the width, and the centre so the opposite corner stays put
                s.metres = std::clamp(c.width, kMinWidth, 12.0);
                vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
                ApplyCurve(s);
                Mat d;
                if (s.pinned != kNone && DevicePose(s.pinned, &d)) Pin(s, s.pinned, Mul(Inverse(d), c.pose));
                else if (s.pinned == kNone) SetAbsolute(s, c.pose);
            }
            continue;
        }
        if (c.width > 0 && !s.floating) {
            s.metres = std::clamp(c.width, kMinWidth, 12.0);
            vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
            ApplyCurve(s);
        }
        SetAbsolute(s, c.pose);
    }
    UpdateSteamTouches(g_handInput.carries());
    const auto &hovered = g_handInput.hovered();
    for (auto &[i, s] : g_screens) {
        const auto h = hovered.find(i);
        const unsigned bits = h == hovered.end() ? 0 : h->second;
        if (bits != s.handHover) s.handHover = bits, ApplyAlpha(s);
        const bool near = g_handInput.nearPanels().count(i) > 0;
        if (near) s.nearUntil = g_tick + kControlsLinger, s.handNearUntil = g_tick + kControlsLinger;   // the controls show
        const bool grown = g_tick < s.handNearUntil;
        if (grown != s.handNear) s.handNear = grown, PlaceChrome(s);   // (and grow for a finger)
    }
    // the near claim, for each hands frame (the helper waits for it at a pinch's begin): 1, the hand's
    // pinches and grips are ours (within the touch zone; a hand the palm menu has, so its pinch
    // doesn't click for the helper; one closing into "grab that"'s fist, so its grip isn't a gaze
    // grip); 2, only its ray stays hidden (the zone's margin and linger: its pinches are the
    // helper's); 0, neither. An older helper takes 2 as 1.
    auto near = [](int k) {
        if (g_handInput.claims(k) || g_palm.shown(k) || FarClaims(g_handInput.frameNs()) ||
            (g_grabThat.claims(k, g_handInput.frameNs()) && (!g_grabThat.armed(k) || g_grabThatAim[k] >= 0)))
            return "1";
        return g_handInput.rayHidden(k) ? "2" : "0";
    };
    if (g_handInput.newFrame() && (HandInputOn() || g_palm.params().on))
        SendPointer("near " + std::to_string(g_handInput.frameNs()) + " " + near(0) + " " + near(1));
    WantHz(g_handInput.anyNear() || g_handInput.approaching() || g_palm.anyShown() || g_grabThat.armed(0) || g_grabThat.armed(1) || g_far.panel >= 0);
}

// Steam in front: the dashboard (the Steam menu) is open, or Steam's own keyboard is up
// (valve.steam.gamepadui.keyboard, for text fields in Steam and the dashboard). Our
// keyboard steps aside then, and comes back where it was when Steam is out of the way; one
// asked for meanwhile appears then. Checked every 9 ticks; Steam makes its keyboard's
// overlay again now and then, so it's looked up each time.
bool g_steamInFront = false;
bool g_keyboardAside = false;   // ours is waiting for Steam to get out of the way
Mat g_asidePose = Identity();   // ...and goes here then

bool SteamInFront() {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    // In the dashboard mode the screens only show with the dashboard, so it doesn't count.
    return (g_mode != Mode::Dashboard && vr::VROverlay()->IsDashboardVisible()) ||
           (vr::VROverlay()->FindOverlay("valve.steam.gamepadui.keyboard", &h) == vr::VROverlayError_None &&
            vr::VROverlay()->IsOverlayVisible(h));
}

void UpdateSteamInFront() {
    const bool front = SteamInFront();
    if (front == g_steamInFront) return;
    g_steamInFront = front;
    std::printf("Steam %s%s\n", front ? "in front (the Steam menu or Steam's keyboard)" : "out of the way",
                front && keyboard::Shown() ? ": our keyboard steps aside" : !front && g_keyboardAside ? ": our keyboard comes back" : "");
    if (front && keyboard::Shown()) {
        g_asidePose = keyboard::Pose();
        keyboard::Hide();
        g_keyboardAside = true;
    } else if (!front && g_keyboardAside) {
        g_keyboardAside = false;
        if (keyboard::Show(g_asidePose)) AnnounceOverlay("frametop.keyboard");
    }
}

}  // namespace

extern "C" {

bool ft_vr_init(void) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_None) {
        vr::VR_Shutdown();
        vr::VR_Init(&err, vr::VRApplication_Overlay);
    }
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return false;
    }
    if (!vr::VRIPCResourceManager()) {
        std::fprintf(stderr, "openvr: no IVRIPCResourceManagerClient (SteamVR too old?)\n");
        return false;
    }
    g_vr = true;
    RefreshPoses();
    // hand cutouts and near hand input settings (~/.config/frametop.conf)
    const std::string matte = handcut::ReadMatteSettings(g_matte);
    g_hands.SetShift(g_matte.shift);
    const std::string lead = handcut::Setting("HANDS_MATTE_LEAD", "");
    if (!lead.empty()) g_hands.SetPrediction(true, std::atof(lead.c_str()));
    roomview::Force rv;
    if (roomview::ParseForce(handcut::Setting("HANDS_ROOMVIEW", "auto"), &rv)) g_roomView.SetForce(rv);
    g_roomView.SetLog(true);
    // pokes come at a panel's face (handinput.h): the finger's angle to it, and a hand resting low
    // by the body (typing) doesn't poke
    auto num = [](const char *name, double &v, double lo, double hi) {
        const std::string t = handcut::Setting(name, "");
        char *end;
        const double x = t.empty() ? 0 : std::strtod(t.c_str(), &end);
        if (!t.empty() && end != t.c_str() && !*end && x >= lo && x <= hi) v = x;
    };
    handinput::Params &k = g_handInput.params();
    num("HANDS_NEAR_REACH", k.nearFront, 0.03, 0.6);   // within it a hand touches: no ray, pokes, its pinches are ours
    // the leave margin and a resting hand's reach keep their proportion to the reach (0.025 and
    // 0.04 at the default 0.075)
    k.nearLeave = std::clamp(k.nearFront / 3, 0.015, 0.05), k.restFront = std::min(k.restFront, k.nearFront);
    k.bandDepth = std::max(0.04, k.nearFront);   // a pinch in the zone by the border takes the panel
    num("HANDS_POKE_ANGLE", k.fingerAngle, 10, 90);
    num("HANDS_POKE_TRAVEL", k.travel, 0, 0.2);   // a one-frame press needs this much travel in; 0: never one frame
    if (k.travel <= 0) k.travel = 1e9;
    num("HANDS_GRAB_BAND", k.bandOut, 0.01, 0.2);   // a pinch this close outside a panel's edge takes it
    k.cornerReach = std::max(k.cornerReach, k.bandOut), k.barReach = std::max(k.barReach, k.bandOut);
    k.pinchClick = handcut::Setting("HANDS_PINCH_CLICK", "1") != "0";
    k.cardPinch = handcut::Setting("HANDS_CARD_PINCH", "1") != "0";
    k.fistGrab = handcut::Setting("HANDS_FIST_GRAB", "1") != "0";
    k.fistResizeOnly = handcut::Setting("HANDS_FIST_RESIZE_ONLY", "1") != "0";
    k.fistPair = handcut::Setting("HANDS_FIST_PAIR", "1") != "0";
    k.twoFingerScroll = handcut::Setting("HANDS_TWO_FINGER_SCROLL", "1") != "0";
    k.motionPress = handcut::Setting("HANDS_MOTION_PRESS", "1") != "0";
    k.sweep = handcut::Setting("HANDS_SWEEP", "1") != "0";
    k.sweepBack = handcut::Setting("HANDS_SWEEP_BACK", "0") != "0";
    {
        const std::string aim = handcut::Setting("HANDS_TAP_AIM", "path");
        k.aimMode = aim == "finger" ? 1 : aim == "straight" ? 0 : 2;
    }
    if (handcut::Setting("HANDS_RELEASE_FREEZE", "1") == "0") k.openFreeze = 0;
    k.touchDot = handcut::Setting("HANDS_TOUCH_DOT", "1") != "0";
    if (handcut::Setting("HANDS_TAP_LOCK", "1") == "0") k.lockOut = 0;
    num("HANDS_POKE_REST_BELOW", k.restBelow, 0, 2);
    num("HANDS_POKE_REST_NEAR", k.restNear, 0, 2);
    // what a near grab carries, smoothed (handinput.h, Params::grab*)
    num("HANDS_GRAB_SMOOTH_MINCUTOFF", k.grabMinCutoff, 0.05, 30);
    num("HANDS_GRAB_SMOOTH_BETA", k.grabBeta, 0, 200);
    num("HANDS_GRAB_DEADZONE", k.grabDeadzone, 0, 0.1);
    // a panel one hand carries turns with the wrist (handinput.h, Params::grabRotate)
    k.grabRotate = handcut::Setting("HANDS_GRAB_ROTATE", "1") != "0";
    num("HANDS_GRAB_ROTATE_DEADZONE", k.rotDeadzone, 0, 45);
    // what a hand holds stays held through a tracking loss this long (handinput.h, Params::holdLostMs)
    num("HANDS_HOLD_LOST_MS", k.holdLostMs, 250, 10000);
    // throws (handinput.h, Params::flick, Params::cardThrow): a flick at a panel, a carry let go fast
    k.flick = handcut::Setting("HANDS_FLICK", "1") != "0";
    num("HANDS_FLICK_SPEED", k.flickSpeed, 0.3, 10);
    num("HANDS_FLICK_OPENING", k.flickOpening, 0, 1);
    k.cardThrow = handcut::Setting("HANDS_THROW", "1") != "0";
    num("HANDS_THROW_SPEED", k.throwSpeed, 0.3, 10);
    num("HANDS_THROW_DISTANCE", k.throwDistance, 0.5, 10);
    // the third live test (2026-10-10): pushes, two hands turning, tracking gates, the target's glow,
    // the width a panel goes away at (handinput.h Params::push, stretchTurn, gate; vr.cpp)
    k.push = handcut::Setting("HANDS_PUSH", "1") != "0";
    num("HANDS_PUSH_SPEED", k.pushSpeed, 0.2, 5);
    k.stretchTurn = handcut::Setting("HANDS_STRETCH_TURN", "1") != "0";
    k.gate = handcut::Setting("HANDS_TRACK_GATE", "1") != "0";
    g_targetGlowOn = handcut::Setting("HANDS_TARGET_GLOW", "1") != "0";
    g_pushGlowOn = handcut::Setting("HANDS_PUSH_GLOW", "0") != "0";
    // the fourth live test (2026-10-10): two anchors, fists, buttons for hands, the soft glow at corners,
    // edges and buttons (handinput.h Params::joinFront, fistFront, buttonReach, pinchButtons, edgeGlow)
    num("HANDS_JOIN_REACH", k.joinFront, 0, 0.5);   // 0: off
    num("HANDS_FIST_REACH", k.fistFront, 0, 0.3);   // 0: the touch zone's (as before)
    if (k.fistFront <= 0) k.fistBack = 0, k.fistClaimCurl = 0;
    num("HANDS_BUTTON_REACH", k.buttonReach, 0, 0.05);
    num("HANDS_BUTTON_SIZE", g_handButton, 0, 0.08);
    k.pinchButtons = handcut::Setting("HANDS_PINCH_BUTTONS", "1") != "0";
    k.edgeGlow = handcut::Setting("HANDS_EDGE_GLOW", "1") != "0";
    const std::string near = handcut::Setting("HANDS_NEAR", "1");
    g_nearMode = near == "0" ? NearMode::Off : near == "force" ? NearMode::Force : NearMode::Helper;
    // The passthrough holes in the panels: off by default since ft-handview draws the hands
    // (user, 2026-10-09: the drawn hands instead of the bubble); `cutouts on` brings them back.
    g_cutouts = handcut::Setting("HANDS_CUTOUTS", "0") == "1";
    std::printf("hands: cutouts %s\n", g_cutouts ? "on (HANDS_CUTOUTS=1)" : "off (the drawn hands show them; HANDS_CUTOUTS=1 cuts holes too)");
    g_palm.params().on = handcut::Setting("HANDS_PALM_MENU", "1") != "0";
    std::printf("hands: palm menu %s\n", g_palm.params().on ? "on (palm up, pinch: the Steam menu)" : "off");
    grabthat::Params &gt = g_grabThat.params();
    gt.on = handcut::Setting("HANDS_GRAB_THAT", "1") != "0";
    num("HANDS_GRAB_THAT_DISTANCE", gt.distance, 0.2, 1.5);
    num("HANDS_GRAB_THAT_WIDTH", gt.maxWidth, 0, 12);
    num("HANDS_RELEASE_REWIND_MS", g_handInput.params().rewindMs, 0, 400);   // a slow let-go goes back this far (0: off)
    num("HANDS_LOCK_PUSH", g_handInput.params().lockPushDepth, 0, 0.1);   // pushed this far through at a frozen aim: a tap
    num("HANDS_TAP_MS", g_handInput.params().tapMs, 0, 2000);   // a press doesn't drag this soon: a tap clicks where it went down
    num("HANDS_PRESS_STILL_MS", g_handInput.params().pressStillMs, 0, 60000);   // a still press lets go after this
    num("HANDS_RAY_HIDE_REACH", g_handInput.params().rayReach, 0, 1);   // a hand this near a panel hides its ray
    gt.aimPalm = handcut::Setting("HANDS_GRAB_THAT_AIM", "palm") != "eyes";   // the line: the palm's normal, or eyes through palm
    num("HANDS_GRAB_THAT_FROM", gt.nearest, 0, 5);      // panels nearer than this aren't taken (0: any, near ones go back)
    num("HANDS_GRAB_THAT_MARGIN", gt.aimMargin, 0, 0.5);  // the line through the palm may pass this far off a panel
    num("HANDS_GRAB_THAT_SMALL", gt.smallDeg, 0, 30);   // small-looking panels: more margin
    num("HANDS_GRAB_THAT_MARGIN_DEG", gt.aimMarginDeg, 0, 20);   // ...or this far (deg seen from the eyes) when it meets none
    num("HANDS_GRAB_THAT_FACING", gt.aimFacing, 0, 90);   // the palm along the aim within this (deg; 0: any)
    {
        double deg = g_curveMaxArc * 180 / M_PI;
        num("SCREEN_CURVE_MAX_DEG", deg, 20, 300);   // a curved screen bends at most this far round (CapCurve)
        g_curveMaxArc = deg * M_PI / 180;
    }
    g_susanBring = handcut::Setting("SCREEN_SPIN_BRING", "1") != "0";   // the lazy susan brings panels (0: the room turns)
    num("SCREEN_SPIN_DISTANCE", g_frontReading, 0.4, 3);   // the front slot's reading distance (m)
    num("SCREEN_SPIN_PPD", g_frontRead.ppd, 8, 80);          // ...as big as its pixels at this many per degree need
    num("SCREEN_SPIN_MAX_DEG", g_frontRead.maxW, 30, 100);   // ...at most this wide (deg; wider ones curve round you)
    num("SCREEN_SPIN_MAX_TALL_DEG", g_frontRead.maxH, 20, 90);
    num("SCREEN_SPIN_MAX_WIDE_DEG", g_frontRead.maxWideW, 30, 170);   // ...an ultrawide (3:1 or wider) at most this wide
    g_wideCurveMaxArc = g_frontRead.maxWideW * M_PI / 180;
    num("SCREEN_FRONT_KEEP_MS", g_frontKeepMs, 0, 600000);   // this much use keeps it in the slot when you look away
    g_frontAway = handcut::Setting("SCREEN_FRONT_AWAY", "0") != "0";   // turned away from, it goes home
    num("HANDS_RESET_HOLD_MS", g_resetHoldMs, 0, 5000);       // the reset button needs this long a hold (0: a press)
    {   // SCREEN_UI_SCALE="104=2 103=1.5": a remote display's own UI scale (its OS's), for its readable size
        std::istringstream in(handcut::Setting("SCREEN_UI_SCALE", ""));
        std::string kv;
        while (in >> kv) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const int n = std::atoi(kv.substr(0, eq).c_str());
            const double sc = std::atof(kv.substr(eq + 1).c_str());
            if (n >= 1 && sc >= 0.25 && sc <= 8) g_uiScaleConf[n - 1] = sc;
        }
    }
    std::printf("front slot: %s; reading %.2f m, readable at %.0f px/deg (at most %.0fx%.0f deg, ultrawides %.0f wide), arm's reach by grab that; "
                "home on the next stroke%s\n",
                g_susanBring ? "the lazy susan brings the next screen to it" : "the lazy susan turns the room (SCREEN_SPIN_BRING=0)",
                g_frontReading, g_frontRead.ppd, g_frontRead.maxW, g_frontRead.maxH, g_frontRead.maxWideW,
                g_frontAway ? (", or turned from (kept after " + std::to_string(g_frontKeepMs / 1000).substr(0, 3) + " s of use)").c_str()
                            : "; it stays until you move it (SCREEN_FRONT_AWAY=0)");
    if (gt.on)
        std::printf("hands: grab that on (an open hand held out, then a fist pulled back: the panel comes %.2f m ahead%s); "
                    "it takes a panel %s meets%s, %s, not one held, the palm within %.0f deg of the line from the eyes\n",
                    gt.distance, gt.maxWidth > 0 ? (", screens at most " + std::to_string(gt.maxWidth).substr(0, 4) + " m wide").c_str() : "",
                    gt.aimPalm ? "the palm faces: the line out of it along its normal" : "the line from the eyes through the palm",
                    ((gt.aimMargin > 0 ? " (or passes " + std::to_string(int(std::lround(gt.aimMargin * 100))) + " cm off)" : std::string()) +
                     (gt.aimMarginDeg > 0 ? " (else the one it passes nearest, within " + std::to_string(int(std::lround(gt.aimMarginDeg))) + " deg)"
                                          : std::string()))
                        .c_str(),
                    gt.nearest > 0 ? ("farther than " + std::to_string(gt.nearest).substr(0, 4) + " m (push, flick or throw near ones away)").c_str()
                                   : "any distance (at a near one: back where it came from)",
                    gt.aimFacing > 0 ? gt.aimFacing : 180.0);
    else
        std::printf("hands: grab that off\n");
    std::printf("hands: near (touch, no ray) within %.3f m of a panel (%.3f resting low), pokes within %.0f deg of its "
                "normal (%s), not from a hand %.2f m under the eyes and within %.2f m\n",
                k.nearFront, std::min(k.nearFront, k.restFront), k.fingerAngle,
                k.travel < 1 ? ("one frame past the surface after " + std::to_string(int(std::lround(k.travel * 1000))) + " mm in").c_str()
                             : "two frames past the surface",
                k.restBelow, k.restNear);
    std::printf("hands: near pinches take a panel within %.0f cm of its border (corners resize)%s; on its content they %s; "
                "a fist at a panel %s\n",
                k.bandOut * 100, k.cardPinch ? ", either side of its edge like a card" : "", k.pinchClick ? "click" : "do nothing",
                !k.fistGrab ? "does nothing (HANDS_FIST_GRAB=0)" : k.fistResizeOnly ? "holds it still, two hands with a fist resize it (HANDS_FIST_RESIZE_ONLY)" : "carries and turns it (two stretch it)");
    std::printf("hands: throws %.1f m out: a flick %s, a carry let go %s, a push %s; sent away, a panel %s\n", k.throwDistance,
                k.flick ? ("at " + std::to_string(k.flickSpeed).substr(0, 4) + " m/s, the fingers opening by " +
                           std::to_string(k.flickOpening).substr(0, 4)).c_str() : "off (HANDS_FLICK=0)",
                k.cardThrow ? ("at " + std::to_string(k.throwSpeed).substr(0, 4) + " m/s").c_str() : "off (HANDS_THROW=0)",
                k.push ? ("at " + std::to_string(k.pushSpeed).substr(0, 4) + " m/s (or 15 cm in)").c_str() : "off (HANDS_PUSH=0)",
                "keeps its width (the one in the front slot steps back instead)");
    std::printf("hands: two hands turn a panel %s; held hands' impossible frames %s; the gesture target glows %s\n",
                k.stretchTurn ? "on every axis" : "about the vertical (HANDS_STRETCH_TURN=0)",
                k.gate ? "dropped (HANDS_TRACK_GATE)" : "taken (HANDS_TRACK_GATE=0)",
                g_targetGlowOn ? "softly white round its edges (HANDS_TARGET_GLOW)" : "not (HANDS_TARGET_GLOW=0)");
    std::printf("hands: a second hand %s; fists reach %.0f cm in front and %.0f behind; buttons %.1f cm each way for a fingertip%s; "
                "corners, edges and buttons glow %s\n",
                k.joinFront > 0 ? ("within " + std::to_string(int(std::lround(k.joinFront * 100))) +
                                   " cm of a panel the other holds joins it (two anchors)").c_str() : "has to find a grab zone (HANDS_JOIN_REACH=0)",
                std::max(k.nearFront, k.fistFront) * 100, -std::min(k.nearBack, k.fistBack) * 100, k.buttonReach * 100,
                k.pinchButtons ? " or a pinch" : " (a pinch takes the bar: HANDS_PINCH_BUTTONS=0)",
                k.edgeGlow ? "softly as a hand comes to them (HANDS_EDGE_GLOW)" : "not; the resize handle follows the hand (HANDS_EDGE_GLOW=0)");
    std::printf("hands: grabs smoothed from %.2f Hz (+%.1f Hz per m/s), a %.1f cm dead zone, held through %.2f s of "
                "tracking loss; the wrist turns them %s\n",
                k.grabMinCutoff, k.grabBeta, k.grabDeadzone * 100, k.holdLostMs / 1000,
                k.grabRotate ? ("past " + std::to_string(int(std::lround(k.rotDeadzone))) + " deg").c_str() : "not (HANDS_GRAB_ROTATE=0)");
    std::printf("hands: matte %s, margin %.2f deg + %.0f ms of motion (at most %.2f more), lead %.0f ms, shift %s; near input %s%s%s\n",
                handcut::ShapeName(g_matte.shape), g_matte.marginDeg, g_matte.motionMs, g_matte.motionMaxDeg,
                g_hands.leadMs(), g_matte.shift.empty() ? "off" : "on",
                g_nearMode == NearMode::Off ? "off" : g_nearMode == NearMode::Force ? "forced on" : "on (the pointer helper's word)",
                matte.empty() ? "" : "; settings: ", matte.c_str());
    // The catcher (see UpdateCatcher): clear and invisible, but the laser lands on it, and
    // it keeps SteamVR's laser mouse on while it's up.
    if (vr::VROverlay()->CreateOverlay("frametop.catcher", "Frametop: release catcher", &g_catcher) ==
        vr::VROverlayError_None) {
        static std::vector<uint8_t> clear(4 * 4 * 4, 0);
        vr::VROverlay()->SetOverlayRaw(g_catcher, clear.data(), 4, 4, 4);
        vr::VROverlay()->SetOverlayInputMethod(g_catcher, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayAlpha(g_catcher, 0);
        vr::VROverlay()->SetOverlayFlag(g_catcher, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    }
    return true;
}

void ft_vr_shutdown(void) {
    if (!g_vr) return;
    // The panels go first, their textures cleared, and the imports only after SteamVR has had
    // a moment to take them down (see the shutdown in compositor.c).
    for (auto &[i, s] : g_screens) {
        vr::VROverlay()->ClearOverlayTexture(s.overlay);
        for (const auto &[k, sub] : s.subs) vr::VROverlay()->ClearOverlayTexture(sub.overlay);
    }
    if (g_cutterState == 1)
        for (auto &[i, s] : g_screens) g_cutter.DropPanel(i);  // drops their imports while SteamVR is up
    if (g_catcher != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(g_catcher);
    g_catcher = vr::k_ulOverlayHandleInvalid;
    for (auto &[i, s] : g_screens) {
        for (auto o : s.All()) vr::VROverlay()->DestroyOverlay(o);
        for (auto &[k, sub] : s.subs) vr::VROverlay()->DestroyOverlay(sub.overlay);
    }
    for (auto &[dev, g] : g_guides)
        for (auto o : {g.ring.overlay, g.dot.overlay}) vr::VROverlay()->DestroyOverlay(o);
    for (GuidePart &r : g_palmRing)
        if (r.overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(r.overlay), r = GuidePart{};
    if (g_target.overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(g_target.overlay), g_target = TargetGlow{};
    for (HandGuide &hg : g_handGuides)
        if (hg.overlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(hg.overlay), hg = HandGuide{};
    keyboard::Destroy();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (auto &[k, h] : g_imports) vr::VRIPCResourceManager()->UnrefResource(h);
    g_guides.clear();
    g_screens.clear();
    g_imports.clear();
    vr::VR_Shutdown();
    g_vr = false;  // the buffers that go after this (KWin's, the streams') have nothing to tell SteamVR
}

int ft_vr_modifiers(uint32_t format, uint64_t *out, int max) {
    if (!g_vr) {  // --no-vr: nothing imports the buffers, so any layout KWin can draw
        if (max < 1) return 0;
        out[0] = 0;  // DRM_FORMAT_MOD_LINEAR
        return 1;
    }
    uint32_t n = uint32_t(max);
    if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, format, &n, out)) return 0;
    return int(n < uint32_t(max) ? n : uint32_t(max));
}

bool ft_vr_screens_shown(void) { return g_vr && ModeVisible(); }
bool ft_vr_paused(void) { return g_paused; }

bool ft_vr_screen_visible(int index) {
    const auto it = g_screens.find(index);
    return !g_vr || (it != g_screens.end() && it->second.visible);
}

enum ft_attention ft_vr_screen_attention(int index) {
    const auto it = g_screens.find(index);
    return g_vr && it != g_screens.end() ? it->second.attention : FT_FOCUSED;
}

bool ft_vr_vsync(double *since, double *hz) {
    if (!g_vr) return false;
    float s = 0;
    uint64_t frame = 0;
    if (!vr::VRSystem()->GetTimeSinceLastVsync(&s, &frame)) return false;
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    const float f = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                  vr::Prop_DisplayFrequency_Float, &err);
    if (err != vr::TrackedProp_Success || !(f >= 30 && f <= 240) || !(s >= 0 && s < 1)) return false;
    *since = s, *hz = f;
    return true;
}

}  // extern "C"

namespace {

// A panel and its controls. `prefix` names the overlays (frametop.screen.N,
// frametop.float.N), `label` is what SteamVR shows ("Screen 2", "Floating window 1").
bool MakePanel(Screen &s, const char *prefix, const char *label) {
    char key[64], name[64];
    std::snprintf(key, sizeof key, "%s", prefix);
    std::snprintf(name, sizeof name, "%s", label);
    s.label = label;
    if (const auto err = vr::VROverlay()->CreateOverlay(key, name, &s.overlay); err != vr::VROverlayError_None) {
        std::fprintf(stderr, "openvr: can't create overlay %s: %s\n", key, vr::VROverlay()->GetOverlayErrorNameFromEnum(err));
        return false;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    vr::VROverlay()->SetOverlayInputMethod(s.overlay, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    const auto &corner = HandleTexture(false);
    static const auto curve = CurveTexture(64);
    static const auto roll = RollTexture(64);
    // (each told to the pointer helper, as the panel is: its ray and the gaze find them at once)
    auto chrome = [&](const char *part, const char *what, const std::vector<uint8_t> &px, int w, int h) {
        std::snprintf(key, sizeof key, "%s.%s", prefix, part);
        std::snprintf(name, sizeof name, "%s: %s", label, what);
        const vr::VROverlayHandle_t o = MakeChrome(key, name, px, w, h);
        if (o != vr::k_ulOverlayHandleInvalid) AnnounceOverlay(key);
        return o;
    };
    s.bar = chrome("bar", "move", BarTexture(false), 256, 24);
    vr::VROverlay()->SetOverlayFlag(s.bar, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    s.curveButton = chrome("curve", "curve", curve, 64, 64);
    s.rollButton = chrome("roll", "roll", roll, 64, 64);
    vr::VROverlay()->SetOverlayFlag(s.rollButton, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    s.handle = chrome("resize", "resize", corner, 64, 64);
    if (s.floating) {
        static const auto dock = DockTexture(64);
        static const auto close = CloseTexture(64);
        s.dockButton = chrome("dock", "back to the desktop", dock, 64, 64);
        s.closeButton = chrome("close", "close", close, 64, 64);
    } else {
        static const auto reset = ResetTexture(64);
        s.resetButton = chrome("reset", "reset the layout", reset, 64, 64);
    }
    ApplyAlpha(s);
    return true;
}

}  // namespace

extern "C" {

void ft_vr_screen_create(int index, double metres, int count) {
    if (!g_vr) return;
    Screen &s = g_screens[index];
    s.metres = metres;
    char prefix[64], label[64];
    std::snprintf(prefix, sizeof prefix, "frametop.screen.%d", index + 1);
    std::snprintf(label, sizeof label, "Screen %d", index + 1);
    if (!MakePanel(s, prefix, label)) return;
    // Until the layout places it: 2 m ahead of the head, in a row, screen 1 on the left.
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double heading = std::atan2(head.m[0][2], head.m[2][2]) * 180 / M_PI;
    const double yaw = heading + (double(count - 1) / 2 - index) * 35;
    const double dx = -std::sin(yaw * M_PI / 180), dz = -std::cos(yaw * M_PI / 180);
    SetAbsolute(s, PanelPose(head.m[0][3] + dx * 2, head.m[1][3], head.m[2][3] + dz * 2, yaw, 0, 0));
}

// A remote screen's panel (remote.c): like a screen's, with its own overlay keys. False if
// SteamVR made no panel (it allows k_unMaxOverlayCount overlays in all, and each panel with
// its controls takes six).
// A remote screen's place while its stream is stopped (Remote Displays' Disconnect): its next
// start puts it back there. Only for this run: the room's coordinates may not be the same in
// the next one, and ft-layout places it then.
struct Parked {
    Mat pose;  // where it was (pinned: where the pin had it then)
    double metres, curve;
    vr::TrackedDeviceIndex_t pinned;
    Mat pinRel;
    bool alone;
};
std::map<int, Parked> g_parked;

bool ft_vr_remote_create(int index, const char *label, double metres, bool *restored) {
    *restored = false;
    if (!g_vr) return false;
    Screen &s = g_screens[index];
    s.remote = true;
    s.metres = metres;
    char prefix[64];
    std::snprintf(prefix, sizeof prefix, "frametop.remote.%d", index + 1);
    if (!MakePanel(s, prefix, label)) {
        g_screens.erase(index);
        return false;
    }
    AnnounceOverlay(prefix);
    // Until the layout places it: 2 m ahead of the head.
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]) * 180 / M_PI;
    const double dx = -std::sin(yaw * M_PI / 180), dz = -std::cos(yaw * M_PI / 180);
    SetAbsolute(s, PanelPose(head.m[0][3] + dx * 2, head.m[1][3], head.m[2][3] + dz * 2, yaw, 0, 0));
    const auto pk = g_parked.find(index);
    if (pk != g_parked.end()) {
        const Parked &k = pk->second;
        SetWidth(s, k.metres);
        s.curve = k.curve;
        CapCurve(s);
        ApplyCurve(s);
        Mat d;
        if (k.pinned != kNone && DevicePose(k.pinned, &d)) Pin(s, k.pinned, k.pinRel);
        else SetAbsolute(s, k.pose);
        s.alone = k.alone;
        g_parked.erase(pk);
        UpdateVisibility();
        *restored = true;
    }
    return true;
}

void ft_vr_beside(void) { g_beside = true; }

// A spare output's panel (number `slot` from 1): made when a window floats on it (SetFloat).
void ft_vr_float_create(int index, int slot) {
    if (!g_vr) return;
    Screen &s = g_screens[index];
    s.floating = true;
    s.slot = slot;
}

void ft_vr_float_output(int index, bool on) {
    auto it = g_screens.find(index);
    if (it != g_screens.end()) it->second.outputOn = on;
}

void ft_vr_screen_scale(int index, double scale) { g_uiScale[index] = scale; }
void ft_vr_key(void) { g_keyMs = NowMs(); }

void ft_vr_screen_destroy(int index) {
    auto it = g_screens.find(index);
    if (it == g_screens.end()) return;
    if (g_cutterState == 1) g_cutter.DropPanel(index);
    Mat p;
    const Screen &s = it->second;
    if (s.remote && ScreenPose(s, &p)) g_parked[index] = {p, s.metres, s.curve, s.pinned, s.pinRel, s.alone};
    for (auto o : it->second.All())
        if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[k, sub] : it->second.subs) vr::VROverlay()->DestroyOverlay(sub.overlay);
    g_screens.erase(it);
}

bool ft_vr_screen_present(int index, const void *key, const struct ft_dmabuf *b) {
    if (!g_vr) return false;
    auto sit = g_screens.find(index);
    if (sit == g_screens.end()) return false;
    Screen &s = sit->second;
    auto it = g_imports.find(key);
    if (it == g_imports.end()) {
        vr::DmabufAttributes_t a{};
        a.unWidth = uint32_t(b->width);
        a.unHeight = uint32_t(b->height);
        a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
        a.unFormat = b->format;
        a.ulModifier = b->modifier;
        a.unPlaneCount = uint32_t(b->n_planes);
        for (int i = 0; i < b->n_planes && i < int(vr::MaxDmabufPlaneCount); ++i) {
            a.plane[i].unOffset = b->offset[i];
            a.plane[i].unStride = b->stride[i];
            a.plane[i].nFd = b->fd[i];
        }
        vr::SharedTextureHandle_t h = 0;
        if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &h)) {
            std::fprintf(stderr, "openvr: ImportDmabuf failed: %dx%d format 0x%x modifier 0x%llx\n", b->width,
                         b->height, b->format, (unsigned long long)b->modifier);
            return false;
        }
        it = g_imports.emplace(key, h).first;
    }
    if (b->width != s.width || b->height != s.height) {
        s.width = b->width, s.height = b->height;
        if (s.floating) {
            ApplyCrop(s);
        } else {
            vr::HmdVector2_t scale = {float(s.width), float(s.height)};
            vr::VROverlay()->SetOverlayMouseScale(s.overlay, &scale);
        }
        PlaceChrome(s);  // the height changed
        std::printf("screen %d: %dx%d\n", index + 1, s.width, s.height);
    }
    s.key = key, s.buf = *b, s.plain = it->second, ++s.frames;
    // While cutting, the next tick draws the new buffer with the cutouts (never floating).
    if (!s.cutting) SetScreenTexture(s, it->second);
    vr::SharedTextureHandle_t handle = it->second;
    vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->SetOverlayTexture(sub.overlay, &tex);
    s.shown = key;  // UpdateVisibility shows it on the next tick
    return true;
}

void ft_vr_forget(const void *key) {
    if (!g_vr) return;
    if (g_cutterState == 1) g_cutter.Forget(key);
    auto it = g_imports.find(key);
    for (auto &[i, s] : g_screens) {
        if (s.key == key) s.key = nullptr;
        // Still on the panel (a remote screen's buffers go while it shows one: its stream
        // stopped or started over): the panel lets go of it first. vrcompositor crashed
        // drawing a texture whose import was gone (2026-10-07, ft-screens quitting with
        // remote screens up, as the headset left standby).
        if (it != g_imports.end() && s.plain == it->second) {
            if (!s.cutting) vr::VROverlay()->ClearOverlayTexture(s.overlay);
            for (const auto &[k, sub] : s.subs) vr::VROverlay()->ClearOverlayTexture(sub.overlay);
            s.plain = 0;
            s.shown = nullptr;  // hidden until a new picture comes
        }
    }
    if (it == g_imports.end()) return;
    vr::VRIPCResourceManager()->UnrefResource(it->second);
    g_imports.erase(it);
}

void ft_vr_poll(void (*handle)(const struct ft_event *, void *), void *data) {
    if (!g_vr) return;
    RefreshPoses();
    // the hands, once per tick, for the cutouts and hand input
    {
        Mat head;
        g_handsNow = (g_cutouts || HandInputOn() || g_palm.params().on) && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) &&
                     g_hands.Update(head, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    }
    for (auto &[index, s] : g_screens) {
        vr::VREvent_t ev;
        // The screen itself (and a floating window's popups): input for KWin.
        auto panelEvent = [&](const vr::VREvent_t &ev, bool sub) {
            ft_event e{};
            e.screen = index;
            if (ev.eventType != vr::VREvent_FocusLeave) {
                s.inputMs = NowMs();
                if (FromLaser(ev)) s.laserMs = NowMs(), s.laserDevice = ev.trackedDeviceIndex;
            }
            if (ev.eventType == vr::VREvent_MouseButtonDown || ev.eventType == vr::VREvent_ScrollDiscrete ||
                ev.eventType == vr::VREvent_ScrollSmooth || g_press.buttons)
                s.useMs = NowMs();
            auto at = [&] { s.ToBuffer(ev.data.mouse.x, ev.data.mouse.y, &e.x, &e.y); };
            switch (ev.eventType) {
                case vr::VREvent_MouseMove:
                    if (s.titleCarry) return;  // KWin's pointer stays where the title bar was pressed
                    // a hand holds the button, or moved this panel's pointer just now: it's the hand's
                    if ((g_press.buttons && IsHandHolder(g_press.device)) || NowMs() - s.handPointerMs < kHandPointerMs) return;
                    e.type = FT_MOTION;
                    at();
                    // A press on a remote screen with the laser on another one, or off its
                    // edge: the moves belong to the remote screen under it (UpdateRemoteDrag).
                    if (s.remote && g_press.buttons && (g_press.remoteOn != index || !OnBuffer(s, e.x, e.y))) return;
                    if (g_press.buttons) g_press.screen = index, g_press.x = e.x, g_press.y = e.y;
                    break;
                case vr::VREvent_MouseButtonDown:
                case vr::VREvent_MouseButtonUp:
                    if (ev.eventType == vr::VREvent_MouseButtonUp) EndDragsBy(ev.trackedDeviceIndex);
                    e.type = FT_BUTTON;
                    e.button = LinuxButton(ev.data.mouse.button);
                    e.pressed = ev.eventType == vr::VREvent_MouseButtonDown;
                    e.controller = IsHandController(ev.trackedDeviceIndex);
                    at();
                    if (!e.pressed && s.titleCarry) e.x = s.carryX, e.y = s.carryY, s.titleCarry = false;
                    // The release of a drag that went onto another remote screen, or off this
                    // one: it comes up where the host's pointer is.
                    if (!e.pressed && s.remote && (g_press.remoteOn != index || !OnBuffer(s, e.x, e.y)) &&
                        g_press.screen >= 0)
                        e.screen = g_press.screen, e.x = g_press.x, e.y = g_press.y;
                    if (e.pressed) {
                        PressDown(ev.trackedDeviceIndex, e.button, index, e.x, e.y);
                        // A floating window's title bar: carry the panel, and KWin (which starts
                        // moving the window on the press) sees no motion until the release.
                        if (!sub && s.floating && e.button == BTN_LEFT && s.titleH > 0 && e.y >= s.cropY &&
                            e.y < s.cropY + s.titleH && s.drag == Drag::None) {
                            s.titleCarry = true, s.carryX = e.x, s.carryY = e.y;
                            StartDrag(s, Drag::Move, ev.trackedDeviceIndex);
                        }
                    } else {
                        g_press.buttons &= ~ButtonBit(e.button);
                        if (!g_press.buttons) g_press.upAt = -1;
                    }
                    break;
                case vr::VREvent_ScrollDiscrete:
                    e.type = FT_SCROLL;
                    e.dx = -ev.data.scroll.xdelta;
                    e.dy = -ev.data.scroll.ydelta;
                    break;
                case vr::VREvent_FocusLeave:
                    if (g_press.buttons) return;  // KWin keeps the pointer while a button is held
                    if (sub) return;              // off a popup is usually onto its window
                    if (NowMs() - s.handPointerMs < kHandPointerMs) return;  // a hand has it
                    e.type = FT_LEAVE;
                    break;
                default:
                    return;
            }
            handle(&e, data);
        };
        while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) panelEvent(ev, false);
        for (const auto &[k, sub] : s.subs)
            while (vr::VROverlay()->PollNextOverlayEvent(sub.overlay, &ev, sizeof ev)) panelEvent(ev, true);
        // The controls light up under a laser.
        auto hover = [&](int k) {
            const bool on = ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_FocusEnter;
            if (on) {
                s.inputMs = NowMs();
                if (FromLaser(ev)) s.laserMs = NowMs(), s.laserDevice = ev.trackedDeviceIndex;
            }
            if (!on && ev.eventType != vr::VREvent_FocusLeave) return;
            if (s.hover[k] != on) s.hover[k] = on, ApplyAlpha(s);
        };
        // The bar: move (and push/pull with the wheel while moving).
        while (vr::VROverlay()->PollNextOverlayEvent(s.bar, &ev, sizeof ev)) {
            hover(0);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Move, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::Move)
                Push(s, ev.data.scroll.ydelta);
        }
        // The corner: resize.
        while (vr::VROverlay()->PollNextOverlayEvent(s.handle, &ev, sizeof ev)) {
            hover(3);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Resize, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // The curve button.
        while (vr::VROverlay()->PollNextOverlayEvent(s.curveButton, &ev, sizeof ev)) {
            hover(1);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                ToggleCurve(s);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // A floating window's buttons: back to the desktop, and close (ft-floatd does both).
        for (int k : {4, 5}) {
            const vr::VROverlayHandle_t o = s.Controls()[k];
            if (o == vr::k_ulOverlayHandleInvalid) continue;
            while (vr::VROverlay()->PollNextOverlayEvent(o, &ev, sizeof ev)) {
                hover(k);
                if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left) {
                    SendFloat((k == 4 ? "dock " : "close ") + std::to_string(index + 1));
                } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                    EndDragsBy(ev.trackedDeviceIndex);
                    ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
                }
            }
        }
        // The reset button: the profile in use opened again, or every screen back in the layout,
        // around where you are now (`ft-layout reset`, like Meta+Shift+R; it refuses a second copy).
        while (s.resetButton != vr::k_ulOverlayHandleInvalid &&
               vr::VROverlay()->PollNextOverlayEvent(s.resetButton, &ev, sizeof ev)) {
            hover(6);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left) {
                ResetHoldStart(index, ev.trackedDeviceIndex, -1);   // (held g_resetHoldMs: the layout resets)
            } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                ResetHoldEnd(ev.trackedDeviceIndex);
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // The roll button: drag around like a knob, or scroll.
        while (vr::VROverlay()->PollNextOverlayEvent(s.rollButton, &ev, sizeof ev)) {
            hover(2);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Roll, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::None) {
                Mat p;
                if (!ScreenPose(s, &p)) continue;
                s.rollFrom = s.pinned != kNone ? s.pinRel : p;
                ApplyRoll(s, ev.data.scroll.ydelta * kRollStep * M_PI / 180);
            }
        }
        if (s.drag != Drag::None) UpdateDrag(s, index);
    }
    // A release on the catcher, or the pointer helper's word that left came up (see "up").
    vr::VREvent_t ev;
    while (g_catcher != vr::k_ulOverlayHandleInvalid &&
           vr::VROverlay()->PollNextOverlayEvent(g_catcher, &ev, sizeof ev))
        if (ev.eventType == vr::VREvent_MouseButtonUp)
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
    if (g_press.upAt >= 0 && g_tick >= g_press.upAt) ReleaseAway(BTN_LEFT, handle, data);
    UpdateHandInput(handle, data);
    ReleaseStuck(handle, data);
    UpdateRemoteDrag(handle, data);
    RefreshChrome();
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
            ft_event e{};
            e.type = FT_QUIT;
            handle(&e, data);
        }
        // A carrying controller that goes away drops its screen.
        if (ev.eventType == vr::VREvent_TrackedDeviceDeactivated) EndDragsBy(ev.trackedDeviceIndex);
        g_roomView.Event(ev.eventType);
    }
    // Our keyboard: its keys, and its Close key. It goes when the screens do.
    struct Forward {
        void (*handle)(const struct ft_event *, void *);
        void *data;
    } forward{handle, data};
    keyboard::Poll(
        [](const keyboard::Event &k, void *f) {
            ft_event e{};
            e.screen = -1;
            e.type = k.type == keyboard::Event::Key ? FT_KEY : FT_KEYBOARD_CLOSED;
            e.key = k.code;
            e.pressed = k.pressed;
            static_cast<Forward *>(f)->handle(&e, static_cast<Forward *>(f)->data);
        },
        &forward);
    if (g_tick % 9 == 0) UpdateSteamInFront();
    if ((keyboard::Shown() || g_keyboardAside) && !ModeVisible()) {
        g_keyboardAside = false;
        keyboard::Hide();
        ft_event e{};
        e.type = FT_KEYBOARD_CLOSED;
        e.screen = -1;
        handle(&e, data);
    }
    ++g_tick;
    UpdateSpin();
    UpdateFront();
    UpdateResetHold();
    if (g_spin.front >= 0) {
        ft_event e{};
        e.type = FT_FRONT;
        e.screen = g_spin.front;
        g_spin.front = -1;
        handle(&e, data);
    }
    UpdateGame();
    UpdateArrange();
    UpdateVisibility();
    UpdateAttention();
    UpdateAim();
    UpdateLasers();
    UpdateControls();
    UpdateGuides();
    UpdateCutouts();
    UpdateCatcher();
}

// Our keyboard (keyboard.cpp) for a screen. It's placed where you'll reach it, not on the
// screen: kKeyboardAhead in front of you (the way your head faces, level) and
// kKeyboardBelow under your eyes, turned to face your eyes, and it stays where it opened
// (or where its grab bar carries it). With Steam in front (UpdateSteamInFront), it waits.
// Without a head pose (the headset is in standby, say) it doesn't open: anywhere else could
// be out of sight or reach. The next text field opens it.
constexpr double kKeyboardAhead = 0.7, kKeyboardBelow = 0.35;

bool ft_vr_keyboard_show(int index) {
    if (g_screens.find(index) == g_screens.end()) return false;
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        std::printf("keyboard: no head pose, not opened\n");
        return false;
    }
    const double fx = -head.m[0][2], fz = -head.m[2][2], n = std::sqrt(fx * fx + fz * fz) + 1e-9;
    const double at[3] = {head.m[0][3] + fx / n * kKeyboardAhead, head.m[1][3] - kKeyboardBelow,
                          head.m[2][3] + fz / n * kKeyboardAhead};
    keyboard::SetLasers(LasersByMode());
    g_steamInFront = SteamInFront();
    if (g_steamInFront) {
        g_asidePose = FacingPose(at, head);
        g_keyboardAside = true;
        std::printf("keyboard: waiting for Steam to close\n");
        return true;
    }
    if (!keyboard::Show(FacingPose(at, head))) return false;
    AnnounceOverlay("frametop.keyboard");
    return true;
}

void ft_vr_keyboard_hide(void) {
    g_keyboardAside = false;
    keyboard::Hide();
}

// Control commands (datagrams on @ft_screens, replies to the sender):
//   place <screen> <x> <y> <z> <yaw> <pitch> <roll>   centre (standing universe) and facing
//   width <screen> <metres>
//   curve <screen> <radius>   cylinder radius in metres; 0 = flat
//   curve <screen> on|off     on: the radius is the head's distance to it now -> "ok <radius>"
//   pin <screen|all> <left|right|head> [12 numbers]   pin to that hand's controller or the
//                             headset: as it is now, or at the given device->screen transform
//                             (rows of a 3x4)
//   unpin <screen|all>
//   get <screen>  -> "ok x y z  xx xy xz  yx yy yz  zx zy zz  width height curve pin
//                     [12 numbers: device->screen, when pinned]"   (pin: none|left|right|head)
//   screens       -> "ok <count> <index>:<pixels w>x<h>:<metres> ..."
//   head          -> "ok x y z yaw"
//   visibility always|dashboard|gesture|toggle
//   wrist <degrees>           a pinned screen shows while you see its front within this
//   gesture <left|right> <degrees>   the gesture mode: look within this of that controller
//   hide | show | toggle      the manual switch (see g_manual)
//   conceal <screen|all> | reveal <screen|all>   a screen hidden on its own, whatever the mode
//   concealed     -> "ok [<screen> ...]"   the screens hidden on their own
//   controllers always|outside_games|dashboard   when controllers' lasers work the screens
//   ingames hide|visible      during a VR game, "always" acts like "only with the dashboard"
//                             (hide), or stays as it is (visible)
//   up                        the pointer helper: the mouse's left button came up. If SteamVR
//                             hasn't delivered that release to one of our overlays within
//                             ~100 ms (it landed on something else), KWin gets it anyway
//   state         -> "ok <mode> <manual 0|1> <wrist deg> <gesture hand> <gesture deg>
//                     <controllers> <game running 0|1> <ingames>"
//   spin next|prev|<degrees>  turn every panel in the room about your head (see the lazy
//                             susan) -> "ok <degrees turned>"
//   spin to <N>               ...the short way, so panel N comes to straight ahead and gets
//                             typing -> "ok <degrees>", "error pinned", "error not in the room"
//   front <N>                 panel N gets typing and its window becomes KWin's active one,
//                             where it is (as when a spin brings it to the front)
//   cutouts on|off|state      hand cutouts (see handcut.h) -> "ok <on|off> <ready|idle|unavailable>
//                             <last composite ms> ms, predict <on|off> lead <ms> ms; ...; room view <on|off> (<why>)"
//   cutouts roomview auto|on|off   cut only with Room View on: detected (roomview.h), or forced
//   cutouts predict on|off    move the hands ahead along their velocity (on by default)
//   cutouts lead <ms>         ...to this long after now: about when the frame is on the displays
//   cutouts matte circle|mesh|capsules   a round hole per hand, the hand-shaped matte, or ft-hands' capsules
//   cutouts margin <deg> [<motion ms> [<motion max deg>]]   the matte's margin around the hand
//   cutouts shift off|"<dist>:<yaw>,<pitch> ..."   the per-distance correction (degrees; off by default)
//   hands <L> <R> <allowed>   the pointer helper: each side hand|controller|off, hand input allowed 0|1
//                             (on change and every second; older than 3 s it lapses)
//   ray <L> <R>               the pointer helper: that side's hand ray is up 0|1 (on change and every
//                             second, older than 3 s it lapses)
//   typing <L> <R>            the pointer helper: typing holds that side's hand back 0|1 (no pokes,
//                             near grabs or "grab that"); on change and every second, older than 3 s it lapses
//   handinput on|off|force|state   near hand input: on follows the helper's word, force ignores it
//                             -> "ok <mode> <left state> <right state>; <counts>"
//   palmmenu on|off|state      the palm menu (HANDS_PALM_MENU) -> "ok <on|off>; <left> <right>; <counts>"
//   grabthat on|off|state      "grab that" (HANDS_GRAB_THAT) -> "ok <on|off>; <left> <right>; <counts>; <panels that came>"
// Floating windows (from ft-floatd; <screen> is the spare output's number, after the screens):
//   float <screen> <metres per pixel> <x> <y> <w> <h> <title>   the window's rectangle in the
//                             buffer and its title bar's height (pixels); shows the panel
//   unfloat <screen>          hides it
//   pose <screen> <12 numbers>  its place in the room (rows of a 3x4, standing universe)
//   sub <screen> <k> <x> <y> <w> <h> | sub <screen> <k> off   popup or dialog k over it
//   minimized <screen> 0|1
//   carry <screen>            the window's own title bar was pressed (an app that draws its
//                             own): carry the panel with the pressing laser until the release
// (size <screen> <w> <h> and key <code> <value> are handled in compositor.c.) Screens are
// numbered from 1 here, like everywhere the user sees them. "screens" and "all" leave out
// floating windows.
void ft_vr_command(const char *cmd, char *reply, int size) {
    if (!g_vr) return (void)std::snprintf(reply, size, "error no SteamVR (--no-vr)");
    RefreshPoses();
    int n;
    double x, y, z, yaw, pitch, roll, w;
    char word[16], hand[16];
    float r[12];
    auto each = [&](const char *which, auto fn) -> bool {  // "all" or a screen number
        if (std::strcmp(which, "all") == 0) {
            for (auto &[i, s] : g_screens)
                if (!s.floating) fn(s);
            return true;
        }
        Screen *s = Find(std::atoi(which));
        if (s) fn(*s);
        return s != nullptr;
    };
    if (std::sscanf(cmd, "place %d %lf %lf %lf %lf %lf %lf", &n, &x, &y, &z, &yaw, &pitch, &roll) == 7) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        EndDrag(*s);
        g_spin.base.erase(n - 1);
        SetAbsolute(*s, PanelPose(x, y, z, yaw, pitch, roll));
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "width %d %lf", &n, &w) == 2) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        SetWidth(*s, w);
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "curve %d %7s", &n, word) == 2 && (!std::strcmp(word, "on") || !std::strcmp(word, "off"))) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        if ((s->curve > 0) != (word[1] == 'n')) ToggleCurve(*s);
        std::snprintf(reply, size, "ok %.3f", s->curve);
    } else if (std::sscanf(cmd, "curve %d %lf", &n, &w) == 2) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        s->curve = w > 0 ? std::max(0.5, w) : 0;
        ApplyCurve(*s);
        PlaceChrome(*s);
        std::snprintf(reply, size, "ok");
    } else if (const int got = std::sscanf(cmd, "pin %15s %15s %f %f %f %f %f %f %f %f %f %f %f %f", word, hand,
                                           &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7], &r[8], &r[9],
                                           &r[10], &r[11]);
               got >= 2) {
        if (std::strcmp(hand, "left") && std::strcmp(hand, "right") && std::strcmp(hand, "head"))
            return (void)std::snprintf(reply, size, "error pin to left, right, or head");
        const vr::TrackedDeviceIndex_t dev = HandDevice(hand);
        Mat c;
        if (dev == vr::k_unTrackedDeviceIndex_Hmd && !DevicePose(dev, &c))
            return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
        if (dev == kNone || !DevicePose(dev, &c))
            return (void)std::snprintf(reply, size, "error no %s controller tracked", hand);
        Mat rel = Identity();
        for (int k = 0; k < 12; ++k) rel.m[k / 4][k % 4] = r[k];
        const bool found = each(word, [&](Screen &s) {
            Mat p;
            EndDrag(s);
            if (got == 14) Pin(s, dev, rel);
            else if (ScreenPose(s, &p)) Pin(s, dev, Mul(Inverse(c), p));
        });
        std::snprintf(reply, size, found ? "ok" : "error no such screen");
    } else if (std::sscanf(cmd, "unpin %15s", word) == 1) {
        const bool found = each(word, [&](Screen &s) {
            Mat p;
            if (s.pinned != kNone && ScreenPose(s, &p)) SetAbsolute(s, p);
        });
        std::snprintf(reply, size, found ? "ok" : "error no such screen");
    } else if (std::sscanf(cmd, "get %d", &n) == 1) {
        Screen *s = Find(n);
        Mat m;
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        if (!ScreenPose(*s, &m)) return (void)std::snprintf(reply, size, "error screen %d has no pose", n);
        int len = std::snprintf(reply, size,
                                "ok %.4f %.4f %.4f  %.5f %.5f %.5f  %.5f %.5f %.5f  %.5f %.5f %.5f  %.4f %.4f %.3f %s",
                                m.m[0][3], m.m[1][3], m.m[2][3], m.m[0][0], m.m[1][0], m.m[2][0], m.m[0][1], m.m[1][1],
                                m.m[2][1], m.m[0][2], m.m[1][2], m.m[2][2], s->metres, s->heightMetres(), s->curve,
                                s->pinned == kNone ? "none" : HandName(s->pinned));
        if (s->pinned != kNone)
            for (int k = 0; k < 12 && len < size; ++k)
                len += std::snprintf(reply + len, size - len, " %.5f", s->pinRel.m[k / 4][k % 4]);
    } else if (std::strncmp(cmd, "screens", 7) == 0) {
        // KWin's screens only: ft-layout, Display Settings and ft-floatd count KWin's outputs
        // by it. Remote screens are listed by "remotes" (remote.c).
        const auto kwin = [](const Screen &s) { return !s.floating && !s.remote; };
        const size_t count = std::count_if(g_screens.begin(), g_screens.end(), [&](auto &e) { return kwin(e.second); });
        int len = std::snprintf(reply, size, "ok %zu", count);
        for (auto &[i, s] : g_screens)
            if (len < size && kwin(s))
                len += std::snprintf(reply + len, size - len, " %d:%dx%d:%.3f", i + 1, s.width, s.height, s.metres);
    } else if (std::strncmp(cmd, "head", 4) == 0) {
        Mat m;
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &m))
            return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
        std::snprintf(reply, size, "ok %.4f %.4f %.4f %.2f", m.m[0][3], m.m[1][3], m.m[2][3],
                      std::atan2(m.m[0][2], m.m[2][2]) * 180 / M_PI);
    } else if (std::sscanf(cmd, "visibility %15s", word) == 1) {
        const std::string m = word;
        if (m == "always") g_mode = Mode::Always;
        else if (m == "dashboard") g_mode = Mode::Dashboard;
        else if (m == "gesture") g_mode = Mode::Gesture;
        else if (m == "toggle") g_mode = Mode::Toggle;
        else return (void)std::snprintf(reply, size, "error modes: always dashboard gesture toggle");
        g_manual = false;
        std::snprintf(reply, size, "ok %s", ModeName());
    } else if (std::sscanf(cmd, "wrist %lf", &w) == 1) {
        g_wristAngle = std::clamp(w, 10.0, 180.0);
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "gesture %15s %lf", hand, &w) == 2) {
        g_gestureHand = std::strcmp(hand, "right") == 0 ? "right" : "left";
        g_gestureAngle = std::clamp(w, 5.0, 90.0);
        std::snprintf(reply, size, "ok");
    } else if (std::strncmp(cmd, "concealed", 9) == 0) {
        int len = std::snprintf(reply, size, "ok");
        for (auto &[i, s] : g_screens)
            if (len < size && !s.floating && s.alone) len += std::snprintf(reply + len, size - len, " %d", i + 1);
    } else if (std::sscanf(cmd, "conceal %15s", word) == 1 || std::sscanf(cmd, "reveal %15s", word) == 1) {
        const bool hide = cmd[0] == 'c';
        const Screen *one = std::strcmp(word, "all") ? Find(std::atoi(word)) : nullptr;
        if (std::strcmp(word, "all") && (!one || one->floating))
            return (void)std::snprintf(reply, size, "error no screen %s", word);
        each(word, [&](Screen &s) { s.alone = hide; });
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (!std::strncmp(cmd, "hide", 4) || !std::strncmp(cmd, "show", 4) || !std::strncmp(cmd, "toggle", 6)) {
        const bool always = EffectiveMode() == Mode::Always;
        const bool shownNow = always ? !g_manual : g_manual;
        const bool want = cmd[0] == 's' ? true : cmd[0] == 'h' ? false : !shownNow;
        g_manual = always ? !want : want;
        UpdateVisibility();
        std::snprintf(reply, size, "ok %s", want ? "shown" : "hidden");
    } else if (std::sscanf(cmd, "pause %15s", word) == 1) {
        if (!std::strcmp(word, "on") || !std::strcmp(word, "off")) {
            const bool on = !std::strcmp(word, "on");
            if (on != g_paused)
                std::printf("%s\n", on ? "paused for a VR game: everything hidden, KWin slowed down" : "resumed");
            g_paused = on;
            UpdateVisibility();
        } else if (std::strcmp(word, "state") != 0) {
            return (void)std::snprintf(reply, size, "error pause on|off|state");
        }
        std::snprintf(reply, size, "ok %s", g_paused ? "paused" : "running");
    } else if (std::sscanf(cmd, "ingames %15s", word) == 1) {
        if (!std::strcmp(word, "hide")) g_inGames = InGames::Hide;
        else if (!std::strcmp(word, "visible")) g_inGames = InGames::Visible;
        else return (void)std::snprintf(reply, size, "error modes: hide visible");
        g_manual = false;
        UpdateVisibility();
        std::snprintf(reply, size, "ok %s", word);
    } else if (std::sscanf(cmd, "controllers %15s", word) == 1) {
        const std::string m = word;
        if (m == "always") g_lasers = Lasers::Always;
        else if (m == "outside_games") g_lasers = Lasers::OutsideGames;
        else if (m == "dashboard") g_lasers = Lasers::Dashboard;
        else return (void)std::snprintf(reply, size, "error modes: always outside_games dashboard");
        UpdateLasers();
        std::snprintf(reply, size, "ok %s", LasersName());
    } else if (char key[48]; !std::strncmp(cmd, "handsetting ", 12) && std::sscanf(cmd, "handsetting %47s %15s", key, word) == 2) {
        // one hand interaction on or off now (Frametop Input Settings' Hands page writes frametop.conf too)
        const int on = !std::strcmp(word, "1") || !std::strcmp(word, "on") ? 1 : !std::strcmp(word, "0") || !std::strcmp(word, "off") ? 0 : -1;
        if (on < 0 || !SetHandSetting(key, on)) return (void)std::snprintf(reply, size, "error handsetting <HANDS_...> 0|1");
        std::printf("hands: %s %s (Input Settings)\n", key, on ? "on" : "off");
        std::snprintf(reply, size, "ok %s %d", key, on);
    } else if (char sl[16], sr[16]; std::sscanf(cmd, "hands %15s %15s %d", sl, sr, &n) == 3) {
        const bool l = !std::strcmp(sl, "hand"), r = !std::strcmp(sr, "hand"), allowed = n != 0;
        if (l != g_handsWord.hand[0] || r != g_handsWord.hand[1] || allowed != g_handsWord.allowed)
            std::printf("hands from the pointer helper: left %s, right %s, %s\n", sl, sr, allowed ? "allowed" : "not allowed");
        g_handsWord.hand[0] = l, g_handsWord.hand[1] = r, g_handsWord.allowed = allowed, g_handsWord.atMs = NowMs();
        g_handsWord.controller[0] = !std::strcmp(sl, "controller"), g_handsWord.controller[1] = !std::strcmp(sr, "controller");
        for (int k = 0; k < 2; ++k)
            if (g_handsWord.hand[k]) g_handsWord.handMs[k] = NowMs();
        std::snprintf(reply, size, "ok");
    } else if (!std::strncmp(cmd, "steamvr ", 8)) {   // the helper's overlay list (see "SteamVR's panels by hand")
        SteamListPart(cmd);
        std::snprintf(reply, size, "ok");
    } else if (int tl = 0, tr = 0; std::sscanf(cmd, "typing %d %d", &tl, &tr) == 2) {
        g_typingWord.side[0] = tl != 0, g_typingWord.side[1] = tr != 0, g_typingWord.atMs = NowMs();
        std::snprintf(reply, size, "ok");
    } else if (int rl = 0, rr = 0; std::sscanf(cmd, "ray %d %d", &rl, &rr) == 2) {
        g_rayWord.side[0] = rl != 0, g_rayWord.side[1] = rr != 0, g_rayWord.atMs = NowMs();
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "handinput %15s", word) == 1) {
        if (!std::strcmp(word, "on")) g_nearMode = NearMode::Helper;
        else if (!std::strcmp(word, "off")) g_nearMode = NearMode::Off;
        else if (!std::strcmp(word, "force")) g_nearMode = NearMode::Force;
        else if (std::strcmp(word, "state") != 0) return (void)std::snprintf(reply, size, "error handinput on|off|force|state");
        const auto &st = g_handInput.stats;
        std::snprintf(reply, size,
                      "ok %s, helper %s (left %s, right %s, %s); %s; %d frames, %d near, %d presses (%d on one frame, %d by "
                      "pinch), %d controls, %d grabs (%d card, %d fist; %d fists waited, %d were grab that), %d stretches; "
                      "%d poke attempts, %d refused, %d off the content, %d at the edge; held back: %d one view, %d bent, "
                      "%d not nearest, %d pinching, %d busy, %d resting, %d typing, %d askew, %d slow, %d flicks, %d open; %d flicks, "
                      "%d throws, %d pushes; holds: %d coasts, %d back, %d glitches, %d held through, %d ended unseen; gates: %d frames "
                      "and %d turns dropped, %d moved, %d near the edge, %d one camera; %d fists ft-hands called pinches; %d second "
                      "hands joined, %d pinches waited to be fists (%d were), %d buttons pinched, %d pokes on a panel the other hand held",
                      g_nearMode == NearMode::Off ? "off" : g_nearMode == NearMode::Force ? "force" : "on",
                      NowMs() - g_handsWord.atMs < 3000 ? "heard" : "silent", g_handsWord.hand[0] ? "hand" : "-",
                      g_handsWord.hand[1] ? "hand" : "-", g_handsWord.allowed ? "allowed" : "not allowed",
                      g_handInput.Describe().c_str(), st.frames, st.nearFrames, st.presses, st.quickPresses, st.pinchClicks,
                      st.controls, st.grabs, st.cards, st.fists, st.fistsWaited, st.fistsCancelled, st.stretches, st.attempts,
                      st.refused, st.offContent, st.edgePresses, st.heldNotStereo, st.heldNotExtended, st.heldNotNearest,
                      st.heldPinching, st.heldBusy, st.heldResting, st.heldTyping, st.heldAskew, st.heldSlow, st.heldFlick, st.heldOpen,
                      st.flicks, st.throws, st.pushes, st.coasts, st.reacquired, st.glitches, st.heldThrough, st.lostEnds, st.gated,
                      st.turnsGated, st.moved, st.edgeFrames, st.monoFrames, st.fistPinches, st.joins, st.settles, st.settledFists,
                      st.buttonPinches, st.heldByOther);
    } else if (std::sscanf(cmd, "palmmenu %15s", word) == 1) {
        if (!std::strcmp(word, "on") || !std::strcmp(word, "off")) g_palm.params().on = !std::strcmp(word, "on");
        else if (std::strcmp(word, "state") != 0) return (void)std::snprintf(reply, size, "error palmmenu on|off|state");
        const auto &st = g_palm.stats;
        std::snprintf(reply, size, "ok %s; %s; %d shown, %d pinches, %d toggles, %d cancelled, %d not a plain pinch",
                      g_palm.params().on ? "on" : "off", g_palm.Describe().c_str(), st.shows, st.pinches, st.toggles, st.cancels,
                      st.notPlain);
    } else if (std::sscanf(cmd, "grabthat %15s", word) == 1) {
        if (!std::strcmp(word, "on") || !std::strcmp(word, "off")) g_grabThat.params().on = !std::strcmp(word, "on");
        else if (std::strcmp(word, "state") != 0) return (void)std::snprintf(reply, size, "error grabthat on|off|state");
        const auto &st = g_grabThat.stats;
        std::string came;
        if (g_front.panel >= 0)
            came = std::to_string(g_front.panel + 1) + " (front slot" + (g_front.released ? ", released" : g_front.kept ? ", kept" : "") + ")";
        std::snprintf(reply, size, "ok %s; %s; %d armed, %d closed, %d fired, %d timed out; came: %s",
                      g_grabThat.params().on ? "on" : "off", g_grabThat.Describe().c_str(), st.arms, st.closings, st.fires,
                      st.timeouts, came.empty() ? "none" : came.c_str());
    } else if (std::sscanf(cmd, "cutouts %15s", word) == 1) {
        char arg[16] = "";
        double ms = 0, a = 0, b = 0;
        if (!std::strcmp(word, "on")) g_cutouts = true;
        else if (!std::strcmp(word, "off")) g_cutouts = false;
        else if (!std::strcmp(word, "predict") && std::sscanf(cmd, "cutouts predict %15s", arg) == 1 &&
                 (!std::strcmp(arg, "on") || !std::strcmp(arg, "off")))
            g_hands.SetPrediction(!std::strcmp(arg, "on"), g_hands.leadMs());
        else if (!std::strcmp(word, "lead") && std::sscanf(cmd, "cutouts lead %lf", &ms) == 1)
            g_hands.SetPrediction(g_hands.predicting(), ms);
        else if (handcut::MatteShape shape; !std::strcmp(word, "matte") && std::sscanf(cmd, "cutouts matte %15s", arg) == 1 &&
                 handcut::ParseShape(arg, &shape))
            g_matte.shape = shape;
        else if (const int got = std::sscanf(cmd, "cutouts margin %lf %lf %lf", &ms, &a, &b);
                 !std::strcmp(word, "margin") && got >= 1 && ms >= 0 && ms <= 10) {
            g_matte.marginDeg = ms;
            if (got >= 2) g_matte.motionMs = std::clamp(a, 0.0, 200.0);
            if (got >= 3) g_matte.motionMaxDeg = std::clamp(b, 0.0, 10.0);
        } else if (!std::strcmp(word, "shift")) {
            const char *spec = cmd + std::strlen("cutouts shift");
            while (*spec == ' ') ++spec;
            if (!std::strcmp(spec, "off")) g_matte.shift.clear();
            else if (!handcut::ParseShift(spec, g_matte.shift)) return (void)std::snprintf(reply, size, "error shift <dist>:<yaw>,<pitch> ...");
            g_hands.SetShift(g_matte.shift);
        } else if (roomview::Force rv; !std::strcmp(word, "roomview") && std::sscanf(cmd, "cutouts roomview %15s", arg) == 1 &&
                   roomview::ParseForce(arg, &rv)) {
            g_roomView.SetForce(rv);
            g_roomView.Update(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(),
                              g_gameRunning, g_constructRoomView);
        } else if (std::strcmp(word, "state") != 0)
            return (void)std::snprintf(reply, size, "error cutouts on|off|state|predict on|off|lead <ms>|matte circle|mesh|capsules|"
                                                    "margin <deg> [ms [deg]]|shift off|<spec>|roomview auto|on|off");
        // Composite's counts since the last state, per second.
        static auto since = Clock::now();
        const double dt = std::max(1e-3, std::chrono::duration<double>(Clock::now() - since).count());
        since = Clock::now();
        const handcut::CutStats c = g_cutter.TakeStats();
        const int calls = c.draws + c.same + c.busy;
        std::snprintf(reply, size,
                      "ok %s %s %.2f ms, predict %s lead %.0f ms; per s: %.1f draws (%.1f partial), %.1f same, %.1f busy, "
                      "%.1f waits; CPU %.2f ms avg %.2f worst; matte %s margin %.2f deg + %.0f ms (max %.2f), shift %s; "
                      "room view %s (%s%s)",
                      g_cutouts ? "on" : "off", g_cutterState > 0 ? "ready" : g_cutterState < 0 ? "unavailable" : "idle",
                      g_cutter.lastMs(), g_hands.predicting() ? "on" : "off", g_hands.leadMs(), c.draws / dt,
                      c.partial / dt, c.same / dt, c.busy / dt, c.waits / dt, calls ? c.cpuMs / calls : 0.0, c.worstMs,
                      handcut::ShapeName(g_matte.shape), g_matte.marginDeg, g_matte.motionMs, g_matte.motionMaxDeg,
                      g_matte.shift.empty() ? "off" : "on", g_roomView.on() ? "on" : "off", g_roomView.why(),
                      g_roomView.on() ? "" : ": no cutouts");
    } else if (int x0, y0, w0, h0, t0; std::sscanf(cmd, "float %d %lf %d %d %d %d %d", &n, &w, &x0, &y0, &w0, &h0, &t0) == 7) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (!(w > 1e-5 && w < 0.01) || w0 < 1 || h0 < 1) return (void)std::snprintf(reply, size, "error bad float");
        SetFloat(*s, w, x0, y0, w0, h0, std::max(0, t0));
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "unfloat %d", &n) == 1) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        Unfloat(*s);
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "pose %d %f %f %f %f %f %f %f %f %f %f %f %f", &n, &r[0], &r[1], &r[2], &r[3], &r[4],
                           &r[5], &r[6], &r[7], &r[8], &r[9], &r[10], &r[11]) == 13) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        Mat m{};
        for (int k = 0; k < 12; ++k) m.m[k / 4][k % 4] = r[k];
        EndDrag(*s);
        g_spin.base.erase(n - 1);
        SetAbsolute(*s, m);
        std::snprintf(reply, size, "ok");
    } else if (int k0; std::sscanf(cmd, "sub %d %d %d %d %d %d", &n, &k0, &x0, &y0, &w0, &h0) == 6 ||
                       (std::sscanf(cmd, "sub %d %d %15s", &n, &k0, word) == 3 && !std::strcmp(word, "off"))) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (std::strstr(cmd, " off")) w0 = h0 = 0;
        SetSub(*s, n - 1, k0, x0, y0, w0, h0);
        std::snprintf(reply, size, "ok");
    } else if (int on; std::sscanf(cmd, "minimized %d %d", &n, &on) == 2) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        s->minimized = on != 0;
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "carry %d", &n) == 1) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (!(g_press.buttons & ButtonBit(BTN_LEFT)) || g_press.screen != n - 1 || s->drag != Drag::None)
            return (void)std::snprintf(reply, size, "error not pressed there");
        s->titleCarry = true, s->carryX = g_press.x, s->carryY = g_press.y;
        StartDrag(*s, Drag::Move, g_press.device);
        std::snprintf(reply, size, "ok");
    } else if (std::strcmp(cmd, "up") == 0) {
        if ((g_press.buttons & ButtonBit(BTN_LEFT)) && g_press.device != kNone && !IsHandController(g_press.device) &&
            !IsHandHolder(g_press.device))
            g_press.upAt = g_tick + 9;
        std::snprintf(reply, size, "ok");
    } else if (!std::strncmp(cmd, "spin ", 5)) {
        SpinCommand(cmd + 5, reply, size);
    } else if (std::sscanf(cmd, "front %d", &n) == 1) {
        // Typing and the active window go to panel N where it is, as after a spin to it (spotwatch,
        // for a pinned panel, which doesn't spin).
        if (!Find(n)) return (void)std::snprintf(reply, size, "error no panel %d", n);
        g_spin.front = n - 1;
        std::snprintf(reply, size, "ok");
    } else if (std::strcmp(cmd, "debug") == 0) {
        // One line for scripts/report.sh: what decides whether a laser drags and the keyboard shows.
        std::string drags;
        for (const auto &[index, s] : g_screens)
            if (s.drag != Drag::None)
                drags += (drags.empty() ? "" : ",") + std::to_string(index + 1) + ":" + DragName(s.drag) + ":" +
                         DeviceLabel(s.dragDevice);
        std::snprintf(reply, size,
                      "ok dashboard=%d steam_front=%d keyboard=%s mode=%s manual=%d lasers=%s game=%d paused=%d "
                      "press=%#x by=%s on=%d catcher=%d drags=%s",
                      vr::VROverlay()->IsDashboardVisible() ? 1 : 0, SteamInFront() ? 1 : 0,
                      keyboard::Shown() ? "shown" : g_keyboardAside ? "aside" : "hidden", ModeName(), g_manual ? 1 : 0,
                      LasersName(), g_gameRunning ? 1 : 0, g_paused ? 1 : 0, g_press.buttons,
                      DeviceLabel(g_press.device).c_str(), g_press.screen + 1, g_catcherShown ? 1 : 0,
                      drags.empty() ? "-" : drags.c_str());
    } else if (std::strncmp(cmd, "state", 5) == 0) {
        std::snprintf(reply, size, "ok %s %d %.0f %s %.0f %s %d %s", ModeName(), g_manual ? 1 : 0, g_wristAngle,
                      g_gestureHand.c_str(), g_gestureAngle, LasersName(), g_gameRunning ? 1 : 0,
                      g_inGames == InGames::Hide ? "hide" : "visible");
    } else {
        std::snprintf(reply, size, "error unknown command");
    }
}

}  // extern "C"
