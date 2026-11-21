// Where the dashboard tiles' group frame goes in the room (dashtiles.h's Board lays the tiles out
// in it; screens/vrtiles.cpp's Regroup places it): at your head when the dashboard opened, level,
// -Z toward Steam's bar (AtBar) or the way you faced (GroupFrame), and Steam's menu as a slot in
// the strip (SteamSlot).
//
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "tiling.h"

#include <algorithm>
#include <cmath>

namespace dashtiles {

// The group frame in the room, from the head's pose: at the head, level, its -Z the way the head
// faces, so the strip is in front of you wherever Steam put its bar (the palm menu's Steam menu
// opens it low and to the side). The way is the forward's, level (right whatever the roll). Near
// straight up or down, where the forward's level part goes to nothing, the up's level part joins
// it, eased in from kUpFromDeg to kUpFullDeg of pitch (fwd - up * fwd.y: the forward with the pitch
// taken out, right without roll; eased, the way turns smoothly with the pitch, no jump), so looking
// straight down at a palm or straight up still faces the same way. `headFwd`, `headUp`: the head's
// -Z and +Y. A pose that isn't finite, or has no way at all, faces -Z.
constexpr double kUpFromDeg = 75, kUpFullDeg = 88;
inline void GroupFrame(const double head[3], const double headFwd[3], const double headUp[3], double out[3][4]) {
    const double s0 = std::sin(tiling::Rad(kUpFromDeg)), s1 = std::sin(tiling::Rad(kUpFullDeg));
    const double t = std::clamp((std::fabs(headFwd[1]) - s0) / (s1 - s0), 0.0, 1.0), w = t * t * (3 - 2 * t);
    double fx = headFwd[0] - w * headUp[0] * headFwd[1], fz = headFwd[2] - w * headUp[2] * headFwd[1];
    const double n = std::hypot(fx, fz);
    if (!(n >= 1e-9) || !std::isfinite(n)) fx = 0, fz = -1;
    else fx /= n, fz /= n;
    const double x[3] = {-fz, 0, fx}, y[3] = {0, 1, 0}, z[3] = {-fx, 0, -fz};
    for (int r = 0; r < 3; ++r) out[r][0] = x[r], out[r][1] = y[r], out[r][2] = z[r], out[r][3] = head[r];
}

// Steam's menu as a slot in the strip (tiling.h's reserve), from the group frame `g`, the bar's
// position and its width in metres (the menu over it is about as wide): the slot's middle round
// you (`azDeg`, in the group frame: also set when there's no slot) and its width (`spanDeg`, at
// most kMaxSlotDeg). Wherever it is round you (the Steam button opens it in front, the palm menu
// off to the side): the strip keeps the slot only when it would reach it (tiling.h). False, no
// slot, when its width isn't known, it's over your head, or anything isn't finite.
constexpr double kMaxSlotDeg = 90;
inline bool SteamSlot(const double g[3][4], const double bar[3], double barWidthM, double *azDeg, double *spanDeg) {
    const double d[3] = {bar[0] - g[0][3], bar[1] - g[1][3], bar[2] - g[2][3]};
    const double x = d[0] * g[0][0] + d[1] * g[1][0] + d[2] * g[2][0], z = d[0] * g[0][2] + d[1] * g[1][2] + d[2] * g[2][2];
    const double flat = std::hypot(x, z);
    const double az = tiling::Deg(std::atan2(x, -z));
    *azDeg = std::isfinite(az) ? az : 0;
    if (!(barWidthM > 0) || !std::isfinite(barWidthM) || !(flat >= 0.2) || !std::isfinite(az)) return false;
    *spanDeg = std::min(kMaxSlotDeg, 2 * tiling::Deg(std::atan2(barWidthM / 2, flat)));
    return true;
}

// The wall at Steam's bar (user, 2026-11-18: "the same distance that the Steam menu is ... the
// middle display should be positioned right above where the Steam menu bar is"; DASH_TILE_AT_BAR,
// on unless "0"): the group frame at the head, level, facing the bar, so the strip is centred over
// it; the wall (the tiles' corners) kBehindBarM further from the head than the bar is (user,
// 2026-11-18: "move the display's distance a little bit back ... I can't interact with the Steam
// menu because it's covered by one of the displays": a flat tile's middle comes nearer than its
// corners, and the laser starts at the hand, below the head, so a tile at the bar's distance stood
// between the hand and the bar); and the desk line `gapDeg` above the bar's top edge, an angle, so
// it stays clear of the bar however far back the wall is. `bar`: its middle, `barUp`: its +Y (a unit vector: Steam tilts it back,
// so the top edge is a little further away too), `barHeightM` (0: not known, its middle counts).
// Into the frame g, *radius (kWallMinM..kWallMaxM) and *deskDeg (kDeskMinDeg..kDeskMaxDeg), the
// ranges DASH_TILE_RADIUS and DASH_TILE_DESK_DEG have. False, nothing set, with the bar under
// kMinFlatM round from straight under or over the head, or anything not finite.
constexpr double kWallMinM = 0.8, kWallMaxM = 6, kDeskMinDeg = -40, kDeskMaxDeg = 20, kMinFlatM = 0.2, kBehindBarM = 0.3;
// The desk line's lift over the bar's top edge (the `gapDeg` callers pass): the bar draws over the
// tiles whatever their depth, and at the tiles' gap (0.8 degrees) the front tile's bottom edge stood
// under it (user, 2026-11-18: "it is just slightly above the dashboard bar and somewhat gets covered.
// So we just want to rise that up just a little bit"); 4 degrees still did, 6 now (user, 2026-11-21: "the
// dashboard bar still occludes").
constexpr double kOverBarDeg = 6;
inline bool AtBar(const double head[3], const double bar[3], const double barUp[3], double barHeightM, double gapDeg,
                  double g[3][4], double *radius, double *deskDeg) {
    const double d[3] = {bar[0] - head[0], bar[1] - head[1], bar[2] - head[2]};
    const double flat = std::hypot(d[0], d[2]), dist = std::sqrt(flat * flat + d[1] * d[1]);
    const double half = std::isfinite(barHeightM) && barHeightM > 0 ? barHeightM / 2 : 0;
    const double top[3] = {d[0] + barUp[0] * half, d[1] + barUp[1] * half, d[2] + barUp[2] * half};
    const double el = tiling::Deg(std::atan2(top[1], std::hypot(top[0], top[2]))) + gapDeg;
    if (!(flat >= kMinFlatM) || !std::isfinite(dist + el)) return false;
    const double fwd[3] = {d[0] / flat, 0, d[2] / flat}, up[3] = {0, 1, 0};
    GroupFrame(head, fwd, up, g);
    *radius = std::clamp(dist + kBehindBarM, kWallMinM, kWallMaxM);
    *deskDeg = std::clamp(el, kDeskMinDeg, kDeskMaxDeg);
    return true;
}

}  // namespace dashtiles
