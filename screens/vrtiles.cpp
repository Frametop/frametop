// The dashboard tiles' glue on OpenVR (user, 2026-11-16; docs/design.md, Dashboard tiles): panels
// anchored to the SteamVR dashboard. The policy is dashtiles.h's (which are anchored, tiled or
// satellites, the keys, the memory), the places tiling.h's, the panel vrpanel.h's. Here: the group
// frame, over Steam's bar (or in front of you) when the dashboard opens (Regroup), held to the bar after that
// (a pin on kDashboard rides on its frame: vr.cpp's MoveFramePins), so a dashboard dragged carries its
// tiles; the panels pinned to their places in it (RetileDash); which panel the tiling keys act on and
// its border (UpdateTileFocus); each app's or display's last state in ~/.config/frametop-tiles.txt;
// and the tiles' commands (TileCommand).

#include "vrpanel.h"
#include "centreglow.h"
#include "overlaytex.h"
#include "tilecurve.h"
#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ftvr {

dashtiles::Memory g_tileMemory;

namespace {
dashtiles::Board g_tiles;
std::map<int, std::string> g_tileKey;     // panel -> its memory key ("screen:2", "app:ID", "remote:HOST/APP")
std::set<int> g_tileRoom;                 // panels put in the room on purpose (TileInRoom): remembered as "room"
Mat g_tileGroupRel = Identity();          // the dashboard's frame (DashFrame) -> the group frame
bool g_tileGroupSet = false;              // ...since the dashboard was first seen with a panel anchored
bool g_tileRegroup = true;                // the dashboard opened: the group goes to where it is now
bool g_tileReset = false;                 // "tiles recentre" (ft-layout's reset): the group in front of your head now
bool g_dashWasUp = false;
bool g_tileStayTheater = false;           // DASH_TILE_STAY_THEATER: staying ones stay over a game in SteamVR's theater too
// The wall: at Steam's bar (DASH_TILE_AT_BAR, tileframe.h's AtBar), or where DASH_TILE_RADIUS and
// DASH_TILE_DESK_DEG put it; which one the group has now (the last Regroup's: the bar may not be placed).
struct Wall {
    double radius = tiling::Params().radius, deskDeg = tiling::Params().deskDeg;
};
Wall g_tileWallSet, g_tileWallBar;        // the settings', and the bar's at the last Regroup
bool g_tileAtBar = true;
bool g_tileWallAtBar = false;
bool g_tileWallBarKnown = false;          // the bar's wall measured once (a Regroup at the bar)
bool g_tileWallKeepBar = false;           // a reset: round where you face, the wall as far as at the bar (or it
                                          // jumped nearer on the reset and back on the next opening)
bool g_tileSteamOverBar = false;          // ...and Steam's tile turned over it (Regroup again once it's tiled)
// The command-center arc (tiling::FromEyes): its centre this far behind your eyes. The wall's numbers
// (Wall, DASH_TILE_GAP_DEG, DASH_TILE_WALL_DEG) are as your eyes see them, a panel's size too (TileInfo).
constexpr double kFocusBackM = 2.0;
// DASH_TILE_CURVE_M (user, 2026-11-22: "they might want to have the tiling curve at a sharper angle
// around them"): the arc's radius, its centre to the wall, metres; at the wall's distance or less the
// arc is round your head. 0, not set: kFocusBackM behind your eyes, whatever the wall's distance.
double g_tileCurveM = 0;
// DASH_TILE_CENTRE_CURVE_M: the centre display's column on an arc of its own radius, the arc above
// its sides' then (tilecurve.h). 0: one arc all round.
double g_tileCentreCurveM = 0;
double BackFor(double wallM) {
    const double curve = g_tileCentreCurveM > 0 ? g_tileCentreCurveM : g_tileCurveM;
    return curve > 0 ? std::max(0.0, curve - wallM) : kFocusBackM;
}
double g_tileHeightCurveM = 0;            // DASH_TILE_HEIGHT_CURVE_M: the rows' over and under the centre display (0: the arc's)
double g_tileSideM = 0;                   // the sides' radius now (ApplyWall; 0: one arc)
tiling::Eyes g_tileEyes;                  // the gaps' and the desk's width (the wall and the desk line: Wall's)
double g_tileFit = 0;                     // DASH_TILE_FIT: 1, the tiles fitted by size with no columns (tilefit.h)
void ApplyWall() {
    const Wall &w = g_tileWallAtBar || g_tileWallKeepBar ? g_tileWallBar : g_tileWallSet;
    g_tileEyes.radius = w.radius, g_tileEyes.deskDeg = w.deskDeg;
    const double back = BackFor(w.radius), moved = back - g_tiles.params.backM;
    // (the arc's centre moved along the group's +Z: your eyes stay where they were; Regroup sets the frame anew)
    if (g_tileGroupSet && moved != 0)
        for (int r = 0; r < 3; ++r) g_tileGroupRel.m[r][3] += float(g_tileGroupRel.m[r][2] * moved);
    tiling::FromEyes(g_tileEyes, back, &g_tiles.params);
    g_tiles.params.fit = g_tileFit >= 0.5;
    g_tileSideM = !(g_tileCentreCurveM > 0) ? 0 : g_tileCurveM > 0 ? g_tileCurveM : w.radius + kFocusBackM;
}
// The wall the layout now is on (tilecurve.h): where each tile's azimuth and elevation are round you.
tilecurve::Wall TileWall() {
    return tilecurve::For(g_tiles.Last(), g_tiles.WallCentre(), g_tiles.params, g_tileSideM, g_tileHeightCurveM);
}
int g_tileFocus = -1;                     // the panel the tiling keys act on (UpdateTileFocus)
int g_tileHold = -1;                      // ...the one a tiling key moved, until the 3D mouse is back on a panel or you look away
int64_t g_tileHoldMs = 0;
constexpr int64_t kTileHoldSettleMs = 500;   // the cursor's panel changing this soon after a key: the tiles moved under it
double g_tileHoldFwd[3] = {0, 0, 0};      // where you faced at that key (all 0: not known)
constexpr double kTileHoldTurnDeg = 20;   // turned this far from it: you look at another panel now
bool g_tileLook = true;                   // DASH_TILE_LOOK: with no pointer on a panel, the one you look at has the focus
Clock::time_point g_tilesSetAt;           // the last "tiles set" or "tiles add": an add long after it is one on its own
constexpr std::chrono::seconds kTilesAddWindow{5};
int g_tileFocusSaid = -1;                 // what the relay was told last: 1 an anchored app's tile has the focus (KeysTile), 2 a display's, 0 none
int64_t g_tileFocusSaidMs = 0;
constexpr int64_t kTileFocusRenewMs = 2000;   // said again this often (the relay lapses it after 5 s)
constexpr double kTileBorder = 0.003;         // the focused tile's border, metres each side (user, 2026-11-19: "far, far, far less pronounced")
struct TileBorder {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int on = -1;                  // the panel it's round, or -1 (hidden)
    int texW = 0, texH = 0;       // its texture now (made again when the panel's shape changes)
    double width = 0;             // its width and place as last set (set again only when they change)
    Mat at = {};
} g_tileBorder;

std::string TilesPath() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/frametop-tiles.txt";
}
void WriteTileMemory() {
    const std::string path = TilesPath(), tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return (void)std::fprintf(stderr, "dashboard tiles: can't write %s\n", tmp.c_str());
    const std::string text = g_tileMemory.Text();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0)
        std::fprintf(stderr, "dashboard tiles: can't write %s\n", path.c_str());
}
dashtiles::SaveTimer g_tileSave;
// The memory changed: written a moment later (SaveTileMemoryDue, on the tick), a burst at once.
void SaveTileMemory() { g_tileSave.Changed(NowMs()); }
}  // namespace
void LoadTileMemory() {
    std::FILE *f = std::fopen(TilesPath().c_str(), "r");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1 << 20)) text.append(buf, n);
    std::fclose(f);
    g_tileMemory.Load(text);
    std::printf("dashboard tiles: %zu apps and displays remembered (%s)\n", g_tileMemory.Size(), TilesPath().c_str());
}
void SaveTileMemoryDue() {
    if (g_tileSave.Due(NowMs())) WriteTileMemory();
}
void FlushTileMemory() {
    if (g_tileSave.Flush()) WriteTileMemory();
}
namespace {
// A panel's state to remember: the grid's, or "room" when it was put there on purpose.
std::string TileLine(int i) {
    return !g_tiles.Anchored(i) && g_tileRoom.count(i) ? "room" : g_tiles.Remember(i);
}
// Every panel with a key remembered as it is now (a change to one moves the others on the grid too).
void RememberTiles() {
    bool changed = false;
    for (const auto &[i, key] : g_tileKey) changed |= g_tileMemory.Set(key, TileLine(i));
    if (changed) SaveTileMemory();
}
}  // namespace

namespace {
// Its natural size on the wall: its pixels at the wall's density (a floating window's, an app's: as
// many as it has now; the wall doesn't resize them), with what's glued left of it (GluedPx).
// A panel's pixels as a tile: those a resize by its corner stretched it to while the window's new
// ones are on their way (SendStretch), so the strip makes room for the size let go at, not the old one.
int TilePxW(const Screen &s) { return s.stretchW > 0 ? s.stretchW : PictureW(s); }
int TilePxH(const Screen &s) { return s.stretchW > 0 ? s.stretchH : PictureH(s); }
dashtiles::Info TileInfo(int id) {
    dashtiles::Info info;
    const auto it = g_screens.find(id);
    if (it == g_screens.end()) return info;
    const int pw = TilePxW(it->second) + GluedPx(id), ph = TilePxH(it->second);
    tiling::Params at = g_tiles.params;
    at.radius = g_tileEyes.radius;   // (as big as it was with the wall round your eyes: the arc's centre is further)
    const double mpp = tiling::MetresPerPx(at);
    if (pw > 0 && ph > 0) info.widthM = pw * mpp, info.heightM = ph * mpp;
    return info;
}
// A tile being dragged: the others stay as they are under it (no gap filled, nothing settled) till
// it's let go (a drop there: the gap waiting is yours).
bool TileDragging() {
    for (const int id : g_tiles.Ids())
        if (const auto it = g_screens.find(id); it != g_screens.end() && it->second.drag != Drag::None) return true;
    return false;
}
// TileInfo is its own size, not Info's guess (it has no picture size yet).
bool TileSized(int id) {
    const auto it = g_screens.find(id);
    return it != g_screens.end() && PictureW(it->second) > 0 && PictureH(it->second) > 0;
}
// The part of a tile's width that's the panel's own (the rest is glued left of it), 0..1.
double OwnShare(int id, const Screen &s) {
    const int own = TilePxW(s), glued = GluedPx(id);
    return own > 0 && glued > 0 ? double(own) / (own + glued) : 1;
}
}  // namespace

namespace {
// The centre display curved round the arc now (Board::ArcNow; not with something glued to it: its
// glue is a flat panel's): its curve the arc's radius, its own kept aside as a flat tile's is.
std::set<int> g_tileArcOn;
bool ArcTile(int i) { return g_tiles.ArcNow(i) && GluedPx(i) == 0; }
// Tiled panels flat but the centre curved round the arc, the others with their own curves back
// (tiling.h's FlatWhileTiled).
void FlattenTiles() {
    for (auto &[i, s] : g_screens) {
        if (s.overlay == vr::k_ulOverlayHandleInvalid) continue;
        bool changed = false;
        if (ArcTile(i)) {
            if (g_tileArcOn.insert(i).second) {   // (its own curve aside, as FlatWhileTiled keeps it)
                if (s.curve > 0) s.tileCurve = s.curve;
                s.curve = 0;
            }
            const double arc = g_tiles.params.radius;
            if (std::fabs(s.curve - arc) < 1e-6) continue;
            s.curve = arc;
            ApplyCurve(s);   // (not CapCurve'd: the arc's, however far round it reaches)
            PlaceChrome(s);
            continue;
        }
        if (g_tileArcOn.erase(i)) s.curve = 0, changed = true;
        changed = tiling::FlatWhileTiled(g_tiles.Tiled(i), &s.curve, &s.tileCurve) || changed;
        if (!changed) continue;
        CapCurve(s);
        ApplyCurve(s);
        PlaceChrome(s);
    }
}

Mat ToMat(const double m[3][4]) {
    Mat o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] = float(m[r][c]);
    return o;
}
}  // namespace

// Each tiled panel pinned to its place at its size there, and each satellite at its own. Nothing
// until the group has a frame (the dashboard seen).
void RetileDash() {
    ApplyWall();
    FlattenTiles();
    if (!g_tileGroupSet || !Dash().Framed()) return;
    const auto &l = g_tiles.Arrange(TileInfo);
    const tilecurve::Wall wall = TileWall();
    for (const tiling::Placed &p : l.tiles) {
        auto it = g_screens.find(p.id);
        if (g_tiles.absent.count(p.id) || it == g_screens.end() || it->second.overlay == vr::k_ulOverlayHandleInvalid) continue;
        Screen &s = it->second;
        if (s.drag != Drag::None) continue;
        // (a floating window or an app with a stretch on its way: its width once its pixels are there)
        const bool waits = (s.floating || s.gsapp || IsWindowPanel(s)) && !s.steamDash && s.stretchW > 0;
        // (the centre curved round the arc: its middle on it, as wide along it as it spans flat)
        const bool arc = ArcTile(p.id);
        const double dist = arc ? g_tiles.params.radius : p.distM;
        const double share = OwnShare(p.id, s), flat = p.widthM * tilecurve::Fit(wall, p);
        const double width = arc ? centreglow::ArcWidth(p.widthM, p.distM, dist) : flat * share;
        if (!waits && std::fabs(s.metres - width) > 0.002) {
            if (s.floating && PictureW(s) > 0) s.mpp = width / PictureW(s);
            SetWidth(s, width);
        }
        double m[3][4];
        tilecurve::Place(wall, p, dist, m);
        // (the right of the tile: what's glued to it goes left of it, steamglue.h; the tile is flat)
        for (int r = 0; r < 3; ++r) m[r][3] += m[r][0] * flat * (1 - share) / 2;
        PinIn(s, kDashboard, Mul(g_tileGroupRel, ToMat(m)), levelpin::Rigid);
    }
    for (const int id : g_tiles.Ids()) {
        const dashtiles::Spot *spot = g_tiles.SpotOf(id);
        auto it = g_screens.find(id);
        if (!spot || !spot->satellite || it == g_screens.end() || it->second.drag != Drag::None) continue;
        PinIn(it->second, kDashboard, Mul(g_tileGroupRel, ToMat(spot->rel)), levelpin::Rigid);
    }
    PlaceTileBorder();   // (with its tile, not a tick after it)
    PlaceCentreGlow();
}

namespace {
// The group to where the dashboard is now, in front of your head, with Steam's menu a slot in it
// when the strip reaches it (the split kept for where it opened: Board::SlotMoved): false without a head pose.
// atHead: round where you face, not at Steam's bar (a reset: the bar is where the dashboard last opened).
bool Regroup(bool atHead = false) {
    Mat head;
    if (!Dash().Framed() || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return false;
    atHead = atHead || Dash().guessed;   // (a guessed bar: round you, the wall as set, no slot for its menu)
    const Mat bar = DashFrame();
    const double h[3] = {head.m[0][3], head.m[1][3], head.m[2][3]}, b[3] = {bar.m[0][3], bar.m[1][3], bar.m[2][3]},
                 fwd[3] = {-head.m[0][2], -head.m[1][2], -head.m[2][2]}, up[3] = {head.m[0][1], head.m[1][1], head.m[2][1]};
    const double barUp[3] = {bar.m[0][1], bar.m[1][1], bar.m[2][1]};
    double g[3][4];
    g_tileWallAtBar = g_tileAtBar && !atHead && dashtiles::AtBar(h, b, barUp, DashHeight(), dashtiles::kOverBarDeg, g,
                                                      &g_tileWallBar.radius, &g_tileWallBar.deskDeg);
    if (!g_tileWallAtBar) dashtiles::GroupFrame(h, fwd, up, g);
    g_tileWallBarKnown = g_tileWallBarKnown || g_tileWallAtBar;
    g_tileWallKeepBar = atHead && g_tileAtBar && g_tileWallBarKnown;
    ApplyWall();
    for (int r = 0; r < 3; ++r) g[r][3] += g[r][2] * g_tiles.params.backM;   // (the arc's centre behind your eyes: its +Z)
    const double barW = DashWidth();
    // (at the bar, Steam's page over it, just in front of the tiles: the slot as wide as the page, or they'd show behind it)
    const double slotW = g_tileWallAtBar ? std::max(barW, SteamPageWidth()) : barW;
    double barAz = 0, slot = 0;
    if (Dash().guessed || !dashtiles::SteamSlot(g, b, slotW, &barAz, &slot)) slot = 0;
    const int steam = SteamDashIndex();
    // Steam's window a tile itself (STEAM_DASH_PANEL), its menu glued to it: no slot kept (it isn't
    // tiled yet when the dashboard opens, so not Tiled(steam): that kept a slot's gap in the strip)
    if (steam >= 0) slot = 0;
    g_tiles.params.reserveAzDeg = barAz, g_tiles.params.reserveDeg = slot;
    g_tiles.SlotMoved();
    // the strip centred again (moves since kept it where it was), its middle straight ahead, Steam's
    // page too (user, 2026-11-19: "the tiling doesn't keep itself centered"; Steam's page over the bar
    // put the strip all off to one side)
    g_tileSteamOverBar = g_tileWallAtBar && steam >= 0 && g_tiles.Tiled(steam);
    g_tiles.Recentre(-1);
    g_tileGroupRel = Mul(Inverse(bar), ToMat(g));
    g_tileGroupSet = true;
    RetileDash();
    const tiling::Layout &l = g_tiles.Last();
    // (the facing in the room, 0 its -Z, right positive; the bar from there)
    std::printf("dashboard tiles: round the head at %.2f %.2f %.2f, facing %.0f degrees; Steam's bar %.0f degrees "
                "round, %.2f m wide, its slot %.0f degrees (0: none), %d columns left of it (-1: the strip doesn't reach it); "
                "the wall %.2f m away %s%s, the desk %.0f degrees up, the tallest column to %.0f, the strip %.0f..%.0f\n",
                h[0], h[1], h[2], tiling::Deg(std::atan2(-g[0][2], g[2][2])), barAz, barW, slot, l.split,
                g_tiles.params.radius, g_tileWallAtBar ? "(at Steam's bar)" : g_tileWallKeepBar ? "(as at Steam's bar)" : "(as set)",
                g_tileSteamOverBar ? ", Steam's page over the bar" : "", l.el0, l.el1, l.az0, l.az1);
    return true;
}
}  // namespace

// The DASH_TILE_* numbers in frametop.conf (vrpanel.h's TileSetting).
std::array<TileSetting, 9> TileSettings() {
    // (the desk and the wall while they aren't at Steam's bar, DASH_TILE_AT_BAR, which keeps to their ranges)
    static_assert(dashtiles::kDeskMinDeg == -40 && dashtiles::kDeskMaxDeg == 20 && dashtiles::kWallMinM == 0.8 &&
                  dashtiles::kWallMaxM == 6, "AtBar's ranges are DASH_TILE_DESK_DEG's and DASH_TILE_RADIUS's");
    return {{{"DASH_TILE_DESK_DEG", &g_tileWallSet.deskDeg, -40, 20},   // the desk line, up from your eyes
             {"DASH_TILE_RADIUS", &g_tileWallSet.radius, 0.8, 6},   // the wall: the tiles' corners from you, metres
             {"DASH_TILE_PX_PER_DEG", &g_tiles.params.pxPerDeg, 15, 120},   // a panel's natural size
             {"DASH_TILE_GAP_DEG", &g_tileEyes.gapDeg, 0, 10},   // between tiles, round you and up
             {"DASH_TILE_WALL_DEG", &g_tileEyes.wallDeg, 30, 300},   // the desk's width before panels stack
             {"DASH_TILE_FIT", &g_tileFit, 0, 1},   // 1: fitted by size, no columns (tilefit.h)
             {"DASH_TILE_CURVE_M", &g_tileCurveM, 0, 12},   // the arc's radius (0: kFocusBackM behind your eyes)
             {"DASH_TILE_CENTRE_CURVE_M", &g_tileCentreCurveM, 0, 12},   // the centre display's own (0: the arc's)
             {"DASH_TILE_HEIGHT_CURVE_M", &g_tileHeightCurveM, 0, 12}}};   // the rows' over and under it (0: the arc's)
}
// One of them live: false if it isn't one, or out of its range. The tiles go round again.
bool SetTileSetting(const char *name, double value) {
    for (const TileSetting &t : TileSettings()) {
        if (std::strcmp(t.name, name) != 0) continue;
        if (!(value >= t.lo && value <= t.hi)) return false;
        *t.value = value;
        ApplyWall();
        RetileDash();
        return true;
    }
    return false;
}

bool TileAnchored(int i) { return g_tiles.Anchored(i); }
bool TileTiled(int i) { return g_tiles.Tiled(i); }
double TileGapM() { return tiling::Rad(g_tiles.params.gapDeg) * g_tiles.params.radius; }
void SetTileStayDefault(bool on) {
    if (on == g_tiles.stayDefault) return;
    g_tiles.stayDefault = on;   // (each one that follows it: shown or hidden next tick)
}
void SetTileStayTheater(bool on) { g_tileStayTheater = on; }
void SetTileAtBar(bool on) {
    if (on == g_tileAtBar) return;
    g_tileAtBar = on;
    g_tileRegroup = true;   // (at once while the dashboard is open, else when it opens)
}
namespace {
std::string TileLabel(int i) { return "screen " + std::to_string(i + 1); }
}  // namespace

void TileAnchor(int i, dashtiles::Stay stay, const char *by, int leftOf) {
    auto it = g_screens.find(i);
    if (it == g_screens.end() || g_tiles.Anchored(i)) return;
    EndDrag(it->second);
    // (off the head or a wrist: tiled, it's the tiles' only, user 2026-11-19: Keep in View then On
    // Dashboard left it pinned in view, and the menu, reading it tiled, offered nothing to undo it)
    if (Mat p; it->second.pinned != kNone && it->second.pinned != kDashboard && ScreenPose(it->second, &p)) SetAbsolute(it->second, p);
    const int after = g_tileFocus >= 0 && g_tiles.Tiled(g_tileFocus) ? g_tileFocus : -1;
    const dashtiles::Info info = TileInfo(i);
    g_tileRoom.erase(i);
    g_tiles.Anchor(i, after, stay, leftOf, &info, TileSized(i));
    if (!g_tileGroupSet) g_tileRegroup = true;
    RetileDash();
    RememberTiles();
    std::printf("%s: anchored to the dashboard%s (%s)%s\n", TileLabel(i).c_str(), TileStays(it->second, i) ? ", staying shown" : "", by,
                g_tileGroupSet ? "" : "; it goes on when the dashboard opens");
}
// Off the tiles, left in the room where it is: `off()` takes it off the board (false: it's still
// on, and nothing else changes). True when it went.
template <typename Off>
bool OffTheTiles(Screen &s, Off off) {
    Mat p;
    const bool placed = ScreenPose(s, &p);   // (where it is on the tiles, before they close up)
    if (!off()) return false;
    EndDrag(s);
    if (placed && s.pinned == kDashboard) SetAbsolute(s, p);
    return true;
}
void TileUnanchor(int i, const char *by) {
    auto it = g_screens.find(i);
    if (it == g_screens.end() || !g_tiles.Anchored(i)) return;
    OffTheTiles(it->second, [&] { return g_tiles.TakeOff(i); });
    RetileDash();
    RememberTiles();
    std::printf("%s: not anchored to the dashboard any more (%s)\n", TileLabel(i).c_str(), by);
}
// Placement's In the Room: off the tiles, and its app (a window's) stays in the room next time too.
void TileInRoom(int i, const char *by) {
    TileUnanchor(i, by);
    if (!g_screens.count(i) || !g_tileRoom.insert(i).second) return;
    RememberTiles();
}
void TileStay(int i, bool on, const char *by) {
    if (g_screens[i].steamDash) {   // (Steam paints its window only with its tab showing: black with the dashboard closed)
        std::printf("%s: Steam's window shows only with the dashboard (%s)\n", TileLabel(i).c_str(), by);
        return;
    }
    if (!g_tiles.Anchored(i)) {
        if (on) TileAnchor(i, g_tiles.stayDefault ? dashtiles::Stay::Default : dashtiles::Stay::On, by);
        return;
    }
    if (g_tiles.Stay(i) == on) return;
    g_tiles.SetStay(i, on);
    RememberTiles();
    std::printf("%s: %s with the dashboard closed (%s)\n", TileLabel(i).c_str(), on ? "stays shown" : "hidden", by);
}

// A panel's key is known (made, or ft-floatd said whose window it is): back to its last state.
void TileKnown(int i, const std::string &key) {
    const std::string k = dashtiles::Memory::Clean(key);
    const auto was = g_tileKey.find(i);
    if (was != g_tileKey.end() && was->second == k) return;
    if (was != g_tileKey.end()) {   // another app on the panel now: the last one's state is its own
        if (g_tileMemory.Set(was->second, TileLine(i))) SaveTileMemory();
        g_tileKey.erase(was);
        g_tileRoom.erase(i);
        TileUnanchor(i, "another app on it");   // (left in the room, off the grid; nothing remembered for it)
        g_tiles.Unanchor(i);                    // (nor the centre display's)
    }
    g_tileKey[i] = k;
    BarSideKnown(i, k);   // (its bar's edge is remembered by the same key)
    if (g_tileMemory.Used(k)) SaveTileMemory();   // (its use kept over a restart: the last to go when it's full)
    const std::string line = g_tileMemory.Get(k);
    switch (dashtiles::TileWhenKnown(k, line)) {
        case dashtiles::WhenKnown::Leave:
            if (line == "room") g_tileRoom.insert(i);
            return;
        case dashtiles::WhenKnown::Anchor: return TileAnchor(i, dashtiles::Stay::Default, "a new window: tiled unless put in the room");
        case dashtiles::WhenKnown::Restore: break;
    }
    const int after = g_tileFocus >= 0 && g_tiles.Tiled(g_tileFocus) ? g_tileFocus : -1;
    bool restored = false;
    const auto restore = [&] { return (restored = g_tiles.Restore(i, line, after, k)) && !g_tiles.Anchored(i); };
    const auto sit = g_screens.find(i);
    if (g_tiles.Anchored(i)) {   // (tiled before its app was known, off for it: the others close up)
        if (sit != g_screens.end() ? OffTheTiles(sit->second, restore) : restore()) RetileDash(), RememberTiles();
    } else restore();
    if (!restored) return (void)std::printf("%s: %s's remembered state isn't one: ignored\n", TileLabel(i).c_str(), k.c_str());
    if (g_tiles.Anchored(i)) {
        if (!g_tileGroupSet) g_tileRegroup = true;
        RetileDash();
        std::printf("%s: back on the dashboard, as %s was (%s)\n", TileLabel(i).c_str(), k.c_str(), line.c_str());
    }
}
namespace {
bool g_tilesStopping = false;
}  // namespace
void TilesStopping() { g_tilesStopping = true; }
void SetTileLookFocus(bool on) { g_tileLook = on; }

// The panel goes (its window, its display): what it was is remembered under its key. Its gap is
// filled a moment later (dashtiles.h's Leave and FillGap, from UpdateDashTiles) unless every panel
// is going (a stop: a restart puts them back) or others go with it.
void TileGone(int i) {
    BarSideGone(i);
    g_tileArcOn.erase(i);
    const auto k = g_tileKey.find(i);
    const std::string key = k != g_tileKey.end() ? k->second : "";
    if (k != g_tileKey.end() && g_tileMemory.Set(k->second, TileLine(i))) SaveTileMemory();
    if (k != g_tileKey.end()) g_tileKey.erase(k);
    g_tileRoom.erase(i);
    if (g_tileFocus == i) g_tileFocus = -1;
    if (g_tilesStopping ? g_tiles.Unanchor(i) : g_tiles.Leave(i, NowMs(), key)) RetileDash(), RememberTiles();
}

// Stays shown with the dashboard closed: as set for it or as DASH_TILE_STAY says (on unless set:
// user, 2026-11-18, a click in the room closes the dashboard and the panels went with it), never
// Steam's own window (vrsteamwin.cpp: Steam paints it only while its tab shows).
bool TileStays(const Screen &s, int i) { return !s.steamDash && g_tiles.Stay(i); }

// Anchored panels (dashtiles.h): shown with the dashboard, or always when they stay; the hide key
// (the manual switch in "always") and a panel's own conceal still hide them. The visibility mode
// is the room's panels' (user, 2026-11-16).
// While SteamVR's theater shows Steam's page the tiles step aside, as the rest of the dashboard
// does then (user, 2026-11-18: "Hide while theater"); they're back when it docks again. A game lent
// to the theater from its panel (View in Theater) hides only the staying ones, unless
// DASH_TILE_STAY_THEATER (off unless set: user, 2026-11-18, "its own separate toggle"): opening the
// dashboard over the game shows the tiles with it, Steam's among them (user, 2026-11-18: "even if I
// open the dashboard, our applications don't show up").
bool TileShown(const Screen &s, int i, bool dashUp) {
    const bool hiddenByKey = EffectiveMode() == Mode::Always && g_manual;
    const bool gameTheater = GsInTheater() && !dashUp && !g_tileStayTheater;
    if ((SteamDashInTheater() || gameTheater) && s.drag == Drag::None) return false;
    return !g_paused && s.shown && !s.alone && !hiddenByKey &&
           (TileStays(s, i) || dashUp || s.drag != Drag::None || i == PadPanel());
}

// A dragged tile's middle at (az, el) round the group's centre right on the centre display's (not itself).
bool OverCentre(int i, double az, double el) {
    const int c = g_tiles.Centre();
    const tiling::Placed *p = g_tiles.PlacedOf(c);
    return c >= 0 && c != i && p && !g_tiles.absent.count(c) && centreglow::Over(az, el, p->azDeg, p->elDeg);
}
// The tile the centre's glow is round now (vrtilecentre.cpp): one being dragged right onto the
// centre display (hot: it would take its place), else the centre while the dashboard is open.
void TileGlowTarget(int *on, bool *hot) {
    *on = -1, *hot = false;
    const int c = g_tiles.Centre();
    if (!g_tileGroupSet || c < 0 || g_tiles.absent.count(c)) return;
    const Mat toGroup = Inverse(Mul(DashFrame(), g_tileGroupRel));
    for (const int id : g_tiles.Ids()) {
        const auto it = g_screens.find(id);
        Mat p;
        if (id == c || it == g_screens.end() || it->second.drag != Drag::Move || !ScreenPose(it->second, &p)) continue;
        const Mat q = Mul(toGroup, p);
        const double pt[3] = {q.m[0][3], q.m[1][3], q.m[2][3]};
        double az, el;
        tilecurve::Direction(TileWall(), pt, &az, &el);
        if (OverCentre(id, az, el)) return (void)(*on = id, *hot = true);
    }
    if (vr::VROverlay()->IsDashboardVisible()) *on = c;
}
// The curve button, or the menu's Curved, on the centre display: curved round the arc or flat (user,
// 2026-11-23: "toggle the curve on a per display basis for just that center display"). False for
// any other panel (its own curve, kept for when it leaves the tiles: FlatWhileTiled).
bool TileCurveToggle(const Screen &s) {
    for (const auto &[i, t] : g_screens) {
        if (&t != &s) continue;
        if (i != g_tiles.Centre() || !g_tiles.ToggleArc(i)) return false;
        std::printf("%s: the centre display %s\n", TileLabel(i).c_str(), g_tiles.Arc(i) ? "curved round the arc" : "flat");
        RetileDash();
        RememberTiles();
        return true;
    }
    return false;
}

// Let go after a move: where on the grid it goes; pulled off it, back to its place (dashtiles.h's Drop).
void TileDrop(Screen &s, int i) {
    Mat p;
    if (!g_tileGroupSet || !ScreenPose(s, &p)) return;
    const Mat q = Mul(Inverse(Mul(DashFrame(), g_tileGroupRel)), p);
    const double pt[3] = {q.m[0][3], q.m[1][3], q.m[2][3]};
    double az, el;
    tilecurve::Direction(TileWall(), pt, &az, &el);
    if (OverCentre(i, az, el)) {   // (right on the centre display: it takes its place, the two trade)
        const int was = g_tiles.Centre();
        g_tiles.TakeCentre(i);
        RetileDash();
        std::printf("%s: let go on the centre display (%s): the centre now, the two traded places\n", TileLabel(i).c_str(),
                    TileLabel(was).c_str());
        RememberTiles();
        return;
    }
    const tiling::Drop d = g_tiles.DropFor(i, az, el, TileInfo);
    g_tiles.Drop(i, d, az);
    static const char *kinds[] = {"into column", "a new column at", "pulled off: back in its place, column", "up, a neighbour pulled in under it, column"};
    std::printf("%s: let go at %.0f, %.0f degrees: %s %d", TileLabel(i).c_str(), az, el, kinds[d.kind], d.col + 1);
    RetileDash();
    std::printf("; the strip %.0f..%.0f (turned %.0f from centred)\n", g_tiles.Last().az0, g_tiles.Last().az1, g_tiles.PanDeg());
    RememberTiles();
}
// Resized by its corner on the grid: that size from now on (the wall puts it back to its shape).
// A floating window or a PC window's panel gets new pixels instead (SendStretch), and a tile is as
// big as its pixels (TileInfo): its cell keeps its scale, or the size let go at counts twice (user,
// 2026-11-20: "it will resize either too large or too small and then not properly fit into the tiled
// scheme until you move the display"). The others make room for the stretch now, and the tile takes
// the new pixels' size when they come (the "float" answer, or the stream at that size).
void TileResized(Screen &s, int i) {
    const tiling::Placed *p = g_tiles.PlacedOf(i);
    if (!p || p->widthM <= 0 || p->heightM <= 0) return;
    if (s.floating || IsWindowPanel(s))
        std::printf("%s: resized to %dx%d px on the grid, its scale kept\n", TileLabel(i).c_str(), TilePxW(s), TilePxH(s));
    else g_tiles.Resized(i, std::sqrt(s.metres / p->widthM * s.heightMetres() / p->heightM) / tilecurve::Fit(TileWall(), *p));
    RetileDash();
    RememberTiles();
}

namespace {
// Which way the headset faces (false: no pose).
bool HeadForward(Mat *head, double fwd[3]) {
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, head)) return false;
    fwd[0] = -head->m[0][2], fwd[1] = -head->m[1][2], fwd[2] = -head->m[2][2];
    return true;
}
// The tiling keys move and size an anchored app's tile only: a floating window, a gamescope app or
// a PC's app window. A desktop screen and a remote display are placed by dragging (user,
// 2026-11-23: "screens/monitors and remote displays should require manually dragging, standalone
// apps are the ones that should tile. Whether they are remote apps or in headset"), so on those
// the keys stay the desktop's and the PC's.
bool KeysTile(int i) {
    const auto it = g_screens.find(i);
    if (it == g_screens.end() || !g_tiles.Anchored(i)) return false;
    const Screen &s = it->second;
    return s.floating || s.gsapp || IsWindowPanel(s);
}
// The panel the tiling keys act on: the one a tiling key just moved (g_tileHold: until the 3D mouse
// is back on a panel or, with DASH_TILE_LOOK, you turn to another), else the one under the pointer, else the one you're
// looking at (within 25 degrees of straight ahead; unless DASH_TILE_LOOK is off). The relay is told
// when it's an anchored app's tile (KeysTile; the keys are the desktop's or the PC's otherwise),
// and that one gets a thin border.
void UpdateTileFocus() {
    const int64_t now = NowMs();
    Mat head;
    double fwd[3];
    const bool facing = HeadForward(&head, fwd);
    if (g_tileHold >= 0) {
        const auto h = g_screens.find(g_tileHold);
        const bool known = g_tileHoldFwd[0] != 0 || g_tileHoldFwd[1] != 0 || g_tileHoldFwd[2] != 0;
        const bool turned = g_tileLook && facing && known && Dot3(fwd, g_tileHoldFwd) < std::cos(kTileHoldTurnDeg * M_PI / 180);
        if (h == g_screens.end() || !h->second.visible || !g_tiles.Anchored(g_tileHold)) g_tileHold = -1;
        else if (now - g_tileHoldMs < kTileHoldSettleMs) g_mouseScreen = -1;   // (the tiles moving under the cursor: not you)
        else if (g_mouseScreen >= 0 || turned) g_tileHold = -1;   // the 3D mouse nudged back onto a panel, or you look at another: it leads again
    }
    int f = g_tileHold;
    const auto m = g_screens.find(g_mouseScreen);
    if (f < 0 && m != g_screens.end() && m->second.visible) f = g_mouseScreen;
    if (f < 0 && g_tileLook && facing) {
        double best = std::cos(25 * M_PI / 180);
        for (auto &[i, s] : g_screens) {
            Mat p;
            if (!s.visible || !ScreenPose(s, &p)) continue;
            double to[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
            const double cs = Dot3(to, fwd) / (std::sqrt(Dot3(to, to)) + 1e-9);
            if (cs > best) best = cs, f = i;
        }
    }
    g_tileFocus = f;
    // (1: an app's tile, every tiling key; 2: a screen's or a remote display's, the nudges only)
    const int tile = KeysTile(f) ? 1 : g_tiles.Anchored(f) ? 2 : 0;
    if (tile != g_tileFocusSaid || (tile && now - g_tileFocusSaidMs >= kTileFocusRenewMs)) {
        SendRelay(tile == 1 ? "tilefocus 1" : tile == 2 ? "tilefocus 2" : "tilefocus 0");
        g_tileFocusSaid = tile, g_tileFocusSaidMs = now;
    }
}
}  // namespace

// The border: one overlay, behind the focused tile, a frame its shape. Placed after the tick's
// moves (the end of ft_vr_poll) and whenever the tiles are (RetileDash), so it keeps up with its tile.
void PlaceTileBorder() {
    const int f = g_tileFocus;
    const auto it = g_screens.find(f);
    const bool tile = it != g_screens.end() && it->second.visible && KeysTile(f);
    TileBorder &b = g_tileBorder;
    if (!tile) {
        if (b.on >= 0 && b.overlay != vr::k_ulOverlayHandleInvalid) PinHide(b.overlay);
        b.on = -1;
        return;
    }
    if (b.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay("frametop.tilefocus", "Frametop: the focused dashboard tile", &b.overlay) !=
            vr::VROverlayError_None)
            return (void)(b.overlay = vr::k_ulOverlayHandleInvalid);
        vr::VROverlay()->SetOverlayInputMethod(b.overlay, vr::VROverlayInputMethod_None);
    }
    Screen &s = it->second;
    Mat p;
    if (!ScreenPose(s, &p)) return;
    const double w = s.metres + 2 * kTileBorder, h = s.heightMetres() + 2 * kTileBorder;
    const int tw = 1024, th = std::clamp(int(std::lround(1024 * h / w)), 8, 4096);   // (fine enough for a 3 mm line)
    if (tw != b.texW || th != b.texH) {
        const int edgeX = std::max(1, int(std::lround(tw * kTileBorder / w))), edgeY = std::max(1, int(std::lround(th * kTileBorder / h)));
        std::vector<uint8_t> px(size_t(tw) * th * 4, 0);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                if (x >= edgeX && x < tw - edgeX && y >= edgeY && y < th - edgeY) continue;
                uint8_t *q = &px[(size_t(y) * tw + x) * 4];
                q[0] = 230, q[1] = 236, q[2] = 245, q[3] = 70;   // (a faint light line, not a blue frame)
            }
        overlaytex::Set(b.overlay, px.data(), tw, th);
        b.texW = tw, b.texH = th;
    }
    if (std::fabs(w - b.width) > 1e-4) vr::VROverlay()->SetOverlayWidthInMeters(b.overlay, float(w)), b.width = w;
    const Mat at = Mul(p, Translation(0, 0, -0.002));
    if (b.on != f || std::memcmp(&at, &b.at, sizeof at) != 0) {
        vr::VROverlay()->SetOverlayTransformAbsolute(b.overlay, vr::TrackingUniverseStanding, &at);
        b.at = at;
    }
    if (b.on != f) PinShow(b.overlay);
    b.on = f;
}

namespace {
// ft-screens started with the dashboard closed (a restart): no read of Steam's bar yet, so nothing
// placed the tiles until it opened (user, 2026-11-22: "Tiling doesn't activate properly after a
// restart of Frame Top until you open the Steam menu and then close it"). Till the first read, the
// bar is guessed in front of you and the tiles grouped round you on it, again when the headset goes
// back on (put on after a restart with it off: they'd be round where it lay). The first opening
// groups them at the bar as always (g_tileRegroup is still set).
constexpr long kGuessAgainTicks = 90;   // the head's pose lost this long (about 1 s): off and on again
long g_guessHeadLost = -1;              // ticks the head's pose has been lost (-1: it's valid)
void GuessTiles(bool up) {
    Mat head;
    const bool valid = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    const bool back = valid && g_guessHeadLost >= kGuessAgainTicks;
    g_guessHeadLost = valid ? -1 : g_guessHeadLost + 1;
    if (!valid || up || Dash().ever || g_tiles.Ids().empty() || TileDragging()) return;
    if (Dash().guessed && !back) return;
    if (GuessDash(head)) Regroup(true);
}
}  // namespace

// Each tick, after the dashboard pins have moved.
void UpdateDashTiles() {
    FlattenTiles();   // (a curve given a tiled panel since: its curve button, a layout put back)
    const bool up = vr::VROverlay()->IsDashboardVisible();
    if (up && !g_dashWasUp) g_tileRegroup = true;
    g_dashWasUp = up;
    // Steam's window hides with the dashboard closed (TileShown: Steam paints it only then, and SteamVR
    // won't show its page without the dashboard): the others close up its gap meanwhile (user, 2026-11-19).
    // So do they round a panel with no picture yet or hidden on its own (Board::absent).
    const int steam = SteamDashIndex();
    g_tiles.steam = steam;
    std::set<int> absent;
    if (!up && steam >= 0) absent.insert(steam);
    for (const int id : g_tiles.Ids())
        if (const auto it = g_screens.find(id); it != g_screens.end() && (!it->second.shown || it->second.alone)) absent.insert(id);
    if (absent != g_tiles.absent) {
        g_tiles.absent = std::move(absent);
        if (g_tileGroupSet) RetileDash();
    }
    // (a read that counts: this opening's, or before the group was ever set the last good one)
    const bool fresh = !Dash().why || (!g_tileGroupSet && !std::strcmp(Dash().why, "hidden"));
    // (Steam's window tiled after the dashboard opened, the first time: it goes over the bar then)
    if (up && g_tileWallAtBar && !g_tileSteamOverBar && steam >= 0 && g_tiles.Tiled(steam)) g_tileRegroup = true;
    GuessTiles(up);
    if (g_tileRegroup && Dash().ever && fresh && !g_tiles.Ids().empty() && Regroup()) g_tileRegroup = false;
    // (a reset: now, the dashboard open or not, round where you face: SteamVR doesn't move its bar while
    // the dashboard is open, so at the bar a reset after turning did nothing; ft-layout leaves the tiled
    // panels to this. The next opening puts them at the bar again, which SteamVR opens in front of you.)
    if (g_tileReset && (g_tiles.Ids().empty() || (Dash().Framed() && Regroup(true)))) g_tileReset = false, g_tileRegroup = false;
    const bool settling = !g_tilesStopping && !TileDragging();
    if (settling && g_tiles.FillGap(NowMs())) {
        RetileDash();
        RememberTiles();
        std::printf("tiles: the gap a panel left filled\n");
    }
    if (settling && g_tiles.Settle()) {
        RetileDash();
        RememberTiles();
        std::printf("tiles: a panel too tall for its stack, once its size was known: a column of its own\n");
    }
    UpdateTileFocus();
}

namespace {
// "tiles gaps": the tiled panels as SteamVR shows them (each one's pose, size and curve), seen from
// the group's centre: the smallest gap between neighbours (columns side by side, rows in a column)
// and either side of Steam's slot, then each one's span round you and up. A check in the headset
// that the tiles keep their distance (tiling::Params::gapDeg) and don't run into each other.
void TileGaps(char *reply, int size) {
    if (!g_tileGroupSet) return (void)std::snprintf(reply, size, "error no tiles placed yet");
    const tiling::Layout &l = g_tiles.Last();
    const Mat toGroup = Inverse(Mul(DashFrame(), g_tileGroupRel));
    const tilecurve::Wall wall = TileWall();
    struct Seen {
        int id, col, row;
        double az0, az1, el0, el1, curve;
    };
    std::vector<Seen> seen;
    for (const tiling::Placed &p : l.tiles) {
        const auto it = g_screens.find(p.id);
        Mat pose;
        if (it == g_screens.end() || !ScreenPose(it->second, &pose)) continue;
        const Screen &s = it->second;
        Seen v{p.id, p.col, p.row, 1e9, -1e9, 1e9, -1e9, s.curve};
        const auto extend = [&](const Screen &e, const Mat &at0) {
            for (int i = 0; i <= 16; ++i)   // (its edges and middle line, along the arc when curved)
                for (int j = 0; j <= 2; ++j) {
                    const Mat at = Mul(toGroup, Mul(at0, OnSurface(e, e.metres * (i / 16.0 - 0.5), e.heightMetres() * (j / 2.0 - 0.5), 0)));
                    const double pt[3] = {at.m[0][3], at.m[1][3], at.m[2][3]};
                    double az, el;
                    tilecurve::Direction(wall, pt, &az, &el);
                    az = tiling::WrapNear(az, p.azDeg);
                    v.az0 = std::min(v.az0, az), v.az1 = std::max(v.az1, az), v.el0 = std::min(v.el0, el), v.el1 = std::max(v.el1, el);
                }
        };
        extend(s, pose);
        for (const auto &[gi, g] : g_screens)   // (what's glued to it is part of its tile)
            if (Mat gp; g.glueTo == p.id && g.visible && ScreenPose(g, &gp)) extend(g, gp);
        seen.push_back(v);
    }
    double gap = 1e9, slotL = 1e9, slotR = 1e9;
    std::map<int, double> nearest;   // column -> its nearest approach to the next one, slot out (the widest of these: how loose)
    const double s0 = l.slotAz - g_tiles.params.reserveDeg / 2, s1 = l.slotAz + g_tiles.params.reserveDeg / 2;
    for (const Seen &a : seen) {
        if (l.split >= 0 && a.col == l.split - 1) slotL = std::min(slotL, s0 - a.az1);
        if (l.split >= 0 && a.col == l.split) slotR = std::min(slotR, a.az0 - s1);
        for (const Seen &b : seen) {
            if (b.col == a.col && b.row == a.row + 1) gap = std::min(gap, a.el0 - b.el1);   // (rows top down)
            if (b.col == a.col + 1 && b.el0 < a.el1 && a.el0 < b.el1) {
                const double g = b.az0 - a.az1 - (l.split == b.col ? s1 - s0 + g_tiles.params.gapDeg : 0);
                gap = std::min(gap, g);
                const auto [it, added] = nearest.try_emplace(a.col, g);
                if (!added) it->second = std::min(it->second, g);
            }
        }
    }
    std::string out;
    char part[256];
    double widest = 0;
    for (const auto &[c, g] : nearest) widest = std::max(widest, g);
    std::snprintf(part, sizeof part,
                  "ok smallest gap %.1f degrees (wanted %.1f; none: 1e9), widest between neighbouring columns %.1f, Steam's slot "
                  "clear by %.1f left, %.1f right",
                  gap, g_tiles.params.gapDeg, widest, slotL, slotR);
    out = part;
    for (const Seen &v : seen) {
        std::snprintf(part, sizeof part, "; %d: column %d row %d, %.1f..%.1f round, %.1f..%.1f up, curve %.2f m", v.id + 1, v.col,
                      v.row, v.az0, v.az1, v.el0, v.el1, v.curve);
        out += part;
    }
    std::snprintf(reply, size, "%s", out.c_str());
}
}  // namespace

// The dashboard tiles (dashtiles.h): anchor and stay, the tiling keys, the grid for a profile, and
// whose window a floating panel is (ft-floatd), so it comes back as that app was.
void TileCommand(vrcmd::Verb verb, vrcmd::Cmd &parsed, const char *cmd, char *reply, int size) {
    (void)cmd;
    // "focused" (the panel the keys act on), or a panel's number from 1
    const auto which = [&](const char *word) -> int {
        if (!std::strcmp(word, "focused")) return g_tileFocus;
        return Find(std::atoi(word)) ? std::atoi(word) - 1 : -1;
    };
    // "on", "off" or "toggle" against what it is now; -1 if it's none of those
    const auto setting = [](const char *word, bool now) {
        return !std::strcmp(word, "on") ? 1 : !std::strcmp(word, "off") ? 0 : !std::strcmp(word, "toggle") ? int(!now) : -1;
    };
    switch (verb) {
    case vrcmd::Anchor: case vrcmd::Stay: {
        const int i = which(parsed.word);
        if (i < 0) return (void)std::snprintf(reply, size, "error no such panel");
        const bool anchor = verb == vrcmd::Anchor;
        const int to = setting(parsed.hand, anchor ? g_tiles.Anchored(i) : g_tiles.Stay(i));
        if (to < 0) return (void)std::snprintf(reply, size, "error on, off or toggle");
        if (!anchor) TileStay(i, to, "command");
        else if (to) TileAnchor(i, dashtiles::Stay::Default, "command");
        else TileUnanchor(i, "command");
        std::snprintf(reply, size, "ok %s %s", anchor ? "anchored" : "stay", to ? "on" : "off");
        return;
    }
    case vrcmd::Tile: {
        dashtiles::Key key;
        if (!dashtiles::ParseKey(parsed.word, &key)) return (void)std::snprintf(reply, size, "error no such tiling key");
        const int i = g_tileFocus;
        if (i < 0 || !g_tiles.Tiled(i)) return (void)std::snprintf(reply, size, "ok no tile has the focus");
        const bool nudge = key == dashtiles::Key::NudgeUp || key == dashtiles::Key::NudgeDown;   // (any tile's)
        if (!nudge && !KeysTile(i)) return (void)std::snprintf(reply, size, "ok a screen or a remote display: drag it");
        g_tileHold = i, g_tileHoldMs = NowMs(), g_mouseScreen = -1;   // (the relay hid the 3D mouse: it keeps the focus)
        if (Mat head; !HeadForward(&head, g_tileHoldFwd)) g_tileHoldFwd[0] = g_tileHoldFwd[1] = g_tileHoldFwd[2] = 0;
        if (!g_tiles.Press(i, key)) return (void)std::snprintf(reply, size, "ok nothing to do");
        RetileDash();
        RememberTiles();
        std::snprintf(reply, size, "ok %s", parsed.word);
        return;
    }
    case vrcmd::Tiles: {
        if (!parsed.rest) {   // "ok N STATE; N STATE..." for every anchored panel, by column and row
            std::string out = "ok";
            std::vector<std::pair<std::pair<int, int>, int>> order;
            for (const int id : g_tiles.Ids()) {
                const auto w = g_tiles.Grid().Find(id);
                order.push_back({{w.ok() ? w.col : 1 << 20, w.ok() ? w.row : 0}, id});
            }
            std::sort(order.begin(), order.end());
            for (const auto &o : order)
                out += (out.size() > 2 ? "; " : " ") + std::to_string(o.second + 1) + " " + g_tiles.Remember(o.second);
            if (int(out.size()) >= size) return (void)std::snprintf(reply, size, "error too many tiles to list");
            std::snprintf(reply, size, "%s", out.c_str());
            return;
        }
        if (!std::strcmp(parsed.rest, "gaps")) return TileGaps(reply, size);
        // "centre N": panel N the centre display, as a drop on the one before does; "centre": which it is.
        // "arc N": the centre display N curved round the arc, or flat again.
        if (!std::strncmp(parsed.rest, "centre", 6) || !std::strncmp(parsed.rest, "arc ", 4)) {
            const bool arc = parsed.rest[0] == 'a';
            const char *n = parsed.rest + (arc ? 4 : 6);
            while (*n == ' ') ++n;
            if (!arc && !*n) return (void)std::snprintf(reply, size, "ok %d", g_tiles.Centre() + 1);
            const int i = which(n);
            if (i < 0 || !g_tiles.Tiled(i)) return (void)std::snprintf(reply, size, "error no such tile");
            if (arc && i != g_tiles.Centre()) return (void)std::snprintf(reply, size, "error not the centre display");
            if (arc ? !g_tiles.ToggleArc(i) : !g_tiles.TakeCentre(i)) return (void)std::snprintf(reply, size, "ok nothing to do");
            RetileDash();
            RememberTiles();
            return (void)std::snprintf(reply, size, "ok %s %d%s", arc ? "arc" : "centre", i + 1, arc ? (g_tiles.Arc(i) ? " on" : " off") : "");
        }
        if (!std::strcmp(parsed.rest, "recentre")) {   // (a reset: packed again too, tilepack.h)
            g_tiles.Repack(TileInfo);
            RetileDash();
            RememberTiles();
            std::printf("dashboard tiles: packed again (a reset): %zu columns\n", g_tiles.Grid().Cols().size());
            return g_tileReset = true, (void)std::snprintf(reply, size, "ok");
        }
        if (!std::strcmp(parsed.rest, "clear")) {
            for (const int id : g_tiles.Ids()) TileUnanchor(id, "tiles clear");
            return (void)std::snprintf(reply, size, "ok");
        }
        // "remember": a profile's put back now (no centre of it known yet); "remember KEY STATE": a tile
        // of it, by its memory's key, for when its display comes later (the headset off, its PC asleep).
        if (!std::strncmp(parsed.rest, "remember", 8)) {
            const std::string line = parsed.rest + 8;
            const size_t k0 = line.find_first_not_of(' '), k1 = line.find(' ', k0), v0 = line.find_first_not_of(' ', k1);
            if (k0 == std::string::npos) return g_tiles.profileCentre.clear(), (void)std::snprintf(reply, size, "ok");
            dashtiles::State st;
            if (k0 == 0 || v0 == std::string::npos || !dashtiles::Parse(line.substr(v0), &st))
                return (void)std::snprintf(reply, size, "error tiles remember KEY STATE");
            const std::string key = line.substr(k0, k1 - k0);
            if (st.centre) g_tiles.profileCentre = dashtiles::Memory::Clean(key);   // (as Restore gets it)
            if (g_tileMemory.Set(key, line.substr(v0))) SaveTileMemory();
            return (void)std::snprintf(reply, size, "ok");
        }
        // "set N STATE; N STATE...": each as Remember gave it, in the order given (by column and
        // row); the tiles it doesn't name go after its columns. "add ...": more of the same set
        // (layout_tiles.py splits a long one, sent at once): the whole set is put back again with
        // it. An add more than kTilesAddWindow after the last set or add puts its tiles after the others.
        const bool more = !std::strncmp(parsed.rest, "add", 3);
        if (more && Clock::now() - g_tilesSetAt > kTilesAddWindow) g_tiles.EndRestore();   // (not the set's: after the others)
        g_tilesSetAt = Clock::now();
        int done = 0, bad = 0;
        std::vector<std::pair<int, std::string>> saved;
        std::string list = parsed.rest + 3;
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            std::string part = list.substr(at, end - at);
            at = end + 1;
            const size_t a = part.find_first_not_of(' ');
            if (a == std::string::npos) continue;
            part = part.substr(a, part.find_last_not_of(' ') + 1 - a);
            int n = 0, used = 0;
            const int i = std::sscanf(part.c_str(), "%d %n", &n, &used) == 1 && used > 0 && Find(n) ? n - 1 : -1;
            const std::string state = i < 0 ? "" : part.substr(used);
            if (state == "off") {
                TileUnanchor(i, "tiles set");   // (left in the room where it is)
                ++done;
                continue;
            }
            if (i < 0) {
                ++bad;
                continue;
            }
            saved.push_back({i, state});
        }
        const int taken = g_tiles.RestoreAll(saved, more);
        done += taken, bad += int(saved.size()) - taken;
        if (taken && !g_tileGroupSet) g_tileRegroup = true;
        RetileDash();
        RememberTiles();
        std::snprintf(reply, size, bad ? "ok %d set, %d not" : "ok %d set", done, bad);
        return;
    }
    case vrcmd::FloatApp: {
        Screen *s = Find(parsed.n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no such floating window");
        TileKnown(parsed.n - 1, std::string("app:") + parsed.appId);
        std::snprintf(reply, size, "ok");
        return;
    }
    default:
        std::snprintf(reply, size, "error unknown command");
    }
}

}  // namespace ftvr
