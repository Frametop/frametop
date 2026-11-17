// The dashboard tiles' glue on OpenVR (user, 2026-11-16; docs/design.md, Dashboard tiles): panels
// anchored to the SteamVR dashboard. The policy is dashtiles.h's (which are anchored, tiled or
// satellites, the keys, the memory), the places tiling.h's, the panel vrpanel.h's. Here: the group
// frame, in front of you when the dashboard opens (Regroup), held to the bar after that
// (a pin on kDashboard rides on its frame: vr.cpp's MoveFramePins), so a dashboard dragged carries its
// tiles; the panels pinned to their places in it (RetileDash); which panel the tiling keys act on and
// its border (UpdateTileFocus); each app's or display's last state in ~/.config/frametop-tiles.txt;
// and the tiles' commands (TileCommand).

#include "vrpanel.h"
#include "overlaytex.h"
#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ftvr {

dashtiles::Memory g_tileMemory;

namespace {
dashtiles::Board g_tiles;
std::map<int, std::string> g_tileKey;     // panel -> its memory key ("screen:2", "app:ID", "remote:HOST/APP")
Mat g_tileGroupRel = Identity();          // the dashboard's frame (DashFrame) -> the group frame
bool g_tileGroupSet = false;              // ...since the dashboard was first seen with a panel anchored
bool g_tileRegroup = true;                // the dashboard opened: the group goes to where it is now
bool g_dashWasUp = false;
bool g_tileStayDefault = false;           // DASH_TILE_STAY: a panel anchored stays shown with the dashboard closed
int g_tileFocus = -1;                     // the panel the tiling keys act on (UpdateTileFocus)
Clock::time_point g_tilesSetAt;           // the last "tiles set" or "tiles add": an add long after it is one on its own
constexpr std::chrono::seconds kTilesAddWindow{5};
int g_tileFocusSaid = -1;                 // what the relay was told last: 1 an anchored panel has the focus, 0 not
int64_t g_tileFocusSaidMs = 0;
constexpr int64_t kTileFocusRenewMs = 2000;   // said again this often (the relay lapses it after 5 s)
constexpr int64_t kAnchorStayMs = 600;        // an anchor button press this long: stay, not anchor
constexpr double kTileBorder = 0.012;         // the focused tile's border, metres each side
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
// Every panel with a key remembered as it is now (a change to one moves the others on the grid too).
void RememberTiles() {
    bool changed = false;
    for (const auto &[i, key] : g_tileKey) changed |= g_tileMemory.Set(key, g_tiles.Remember(i));
    if (changed) SaveTileMemory();
}
}  // namespace

namespace {
// Its natural size on the wall: its pixels at the wall's density (a floating window's, an app's: as
// many as it has now; the wall doesn't resize them), with what's glued left of it (GluedPx).
dashtiles::Info TileInfo(int id) {
    dashtiles::Info info;
    const auto it = g_screens.find(id);
    if (it == g_screens.end()) return info;
    const int pw = PictureW(it->second) + GluedPx(id), ph = PictureH(it->second);
    const double mpp = tiling::MetresPerPx(g_tiles.params);
    if (pw > 0 && ph > 0) info.widthM = pw * mpp, info.heightM = ph * mpp;
    return info;
}
// The part of a tile's width that's the panel's own (the rest is glued left of it), 0..1.
double OwnShare(int id, const Screen &s) {
    const int own = PictureW(s), glued = GluedPx(id);
    return own > 0 && glued > 0 ? double(own) / (own + glued) : 1;
}
}  // namespace

namespace {
// Tiled panels flat, the others with their own curves back (tiling.h's FlatWhileTiled).
void FlattenTiles() {
    for (auto &[i, s] : g_screens) {
        if (s.overlay == vr::k_ulOverlayHandleInvalid || !tiling::FlatWhileTiled(g_tiles.Tiled(i), &s.curve, &s.tileCurve))
            continue;
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
    FlattenTiles();
    if (!g_tileGroupSet || !Dash().ever) return;
    const auto &l = g_tiles.Arrange(TileInfo);
    for (const tiling::Placed &p : l.tiles) {
        auto it = g_screens.find(p.id);
        if (it == g_screens.end() || it->second.overlay == vr::k_ulOverlayHandleInvalid) continue;
        Screen &s = it->second;
        if (s.drag != Drag::None) continue;
        // (a floating window or an app with a stretch on its way: its width once its pixels are there)
        const bool waits = (s.floating || s.gsapp) && !s.steamDash && s.stretchW > 0;
        const double share = OwnShare(p.id, s), width = p.widthM * share;
        if (!waits && std::fabs(s.metres - width) > 0.002) {
            if (s.floating && PictureW(s) > 0) s.mpp = width / PictureW(s);
            SetWidth(s, width);
        }
        double m[3][4];
        tiling::Pose(p.azDeg, p.elDeg, p.distM, m);
        // (the right of the tile: what's glued to it goes left of it, steamglue.h; the tile is flat)
        for (int r = 0; r < 3; ++r) m[r][3] += m[r][0] * p.widthM * (1 - share) / 2;
        PinIn(s, kDashboard, Mul(g_tileGroupRel, ToMat(m)), levelpin::Rigid);
    }
    for (const int id : g_tiles.Ids()) {
        const dashtiles::Spot *spot = g_tiles.SpotOf(id);
        auto it = g_screens.find(id);
        if (!spot || !spot->satellite || it == g_screens.end() || it->second.drag != Drag::None) continue;
        PinIn(it->second, kDashboard, Mul(g_tileGroupRel, ToMat(spot->rel)), levelpin::Rigid);
    }
    PlaceTileBorder();   // (with its tile, not a tick after it)
}

namespace {
// The group to where the dashboard is now, in front of your head, with Steam's menu a slot in it
// when the strip reaches it (the split kept for where it opened: Board::SlotMoved): false without a head pose.
bool Regroup() {
    Mat head;
    if (!Dash().ever || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return false;
    const Mat bar = DashFrame();
    const double h[3] = {head.m[0][3], head.m[1][3], head.m[2][3]}, b[3] = {bar.m[0][3], bar.m[1][3], bar.m[2][3]},
                 fwd[3] = {-head.m[0][2], -head.m[1][2], -head.m[2][2]}, up[3] = {head.m[0][1], head.m[1][1], head.m[2][1]};
    double g[3][4];
    dashtiles::GroupFrame(h, fwd, up, g);
    const double barW = DashWidth();
    double barAz = 0, slot = 0;
    if (!dashtiles::SteamSlot(g, b, barW, &barAz, &slot)) slot = 0;
    const int steam = SteamDashIndex();
    if (steam >= 0 && g_tiles.Tiled(steam)) slot = 0;   // Steam's window is a tile itself: no slot kept for it
    g_tiles.params.reserveAzDeg = barAz, g_tiles.params.reserveDeg = slot;
    g_tiles.SlotMoved();
    g_tileGroupRel = Mul(Inverse(bar), ToMat(g));
    g_tileGroupSet = true;
    RetileDash();
    const tiling::Layout &l = g_tiles.Last();
    // (the facing in the room, 0 its -Z, right positive; the bar from there)
    std::printf("dashboard tiles: round the head at %.2f %.2f %.2f, facing %.0f degrees; Steam's bar %.0f degrees "
                "round, %.2f m wide, its slot %.0f degrees (0: none), %d columns left of it (-1: the strip doesn't reach it); "
                "the desk %.0f degrees up, the tallest column to %.0f, the strip %.0f..%.0f\n",
                h[0], h[1], h[2], tiling::Deg(std::atan2(-g[0][2], g[2][2])), barAz, barW, slot, l.split, l.el0, l.el1,
                l.az0, l.az1);
    return true;
}
}  // namespace

// The DASH_TILE_* numbers in frametop.conf (vrpanel.h's TileSetting).
std::array<TileSetting, 5> TileSettings() {
    return {{{"DASH_TILE_DESK_DEG", &g_tiles.params.deskDeg, -40, 20},   // the desk line, up from your eyes
             {"DASH_TILE_RADIUS", &g_tiles.params.radius, 0.8, 6},   // the wall: the tiles' corners from you, metres
             {"DASH_TILE_PX_PER_DEG", &g_tiles.params.pxPerDeg, 15, 120},   // a panel's natural size
             {"DASH_TILE_GAP_DEG", &g_tiles.params.gapDeg, 0, 10},   // between tiles, round you and up
             {"DASH_TILE_WALL_DEG", &g_tiles.params.wallDeg, 30, 300}}};   // the desk's width before panels stack
}
// One of them live: false if it isn't one, or out of its range. The tiles go round again.
bool SetTileSetting(const char *name, double value) {
    for (const TileSetting &t : TileSettings()) {
        if (std::strcmp(t.name, name) != 0) continue;
        if (!(value >= t.lo && value <= t.hi)) return false;
        *t.value = value;
        RetileDash();
        return true;
    }
    return false;
}

bool TileAnchored(int i) { return g_tiles.Anchored(i); }
bool TileTiled(int i) { return g_tiles.Tiled(i); }
bool TileStayDefault() { return g_tileStayDefault; }
void SetTileStayDefault(bool on) { g_tileStayDefault = on; }
namespace {
std::string TileLabel(int i) { return "screen " + std::to_string(i + 1); }
}  // namespace

void TileAnchor(int i, bool stay, const char *by, int leftOf) {
    auto it = g_screens.find(i);
    if (it == g_screens.end() || g_tiles.Anchored(i)) return;
    EndDrag(it->second);
    const int after = g_tileFocus >= 0 && g_tiles.Tiled(g_tileFocus) ? g_tileFocus : -1;
    const dashtiles::Info info = TileInfo(i);
    g_tiles.Anchor(i, after, stay, leftOf, &info);
    LightAnchor(it->second);
    if (!g_tileGroupSet) g_tileRegroup = true;
    RetileDash();
    RememberTiles();
    std::printf("%s: anchored to the dashboard%s (%s)%s\n", TileLabel(i).c_str(), stay ? ", staying shown" : "", by,
                g_tileGroupSet ? "" : "; it goes on when the dashboard opens");
}
void TileUnanchor(int i, const char *by) {
    auto it = g_screens.find(i);
    if (it == g_screens.end() || !g_tiles.Anchored(i)) return;
    Screen &s = it->second;
    Mat p;
    const bool placed = ScreenPose(s, &p);
    EndDrag(s);
    g_tiles.Unanchor(i);
    LightAnchor(s);
    if (placed && s.pinned == kDashboard) SetAbsolute(s, p);   // left in the room where it is
    RetileDash();
    RememberTiles();
    std::printf("%s: not anchored to the dashboard any more (%s)\n", TileLabel(i).c_str(), by);
}
namespace {
void TileStay(int i, bool on, const char *by) {
    if (!g_tiles.Anchored(i)) {
        if (on) TileAnchor(i, true, by);
        return;
    }
    if (g_tiles.Stay(i) == on) return;
    g_tiles.SetStay(i, on);
    LightAnchor(g_screens[i]);
    RememberTiles();
    std::printf("%s: %s with the dashboard closed (%s)\n", TileLabel(i).c_str(), on ? "stays shown" : "hidden", by);
}
}  // namespace

// A panel's key is known (made, or ft-floatd said whose window it is): back to its last state.
void TileKnown(int i, const std::string &key) {
    const std::string k = dashtiles::Memory::Clean(key);
    const auto was = g_tileKey.find(i);
    if (was != g_tileKey.end() && was->second == k) return;
    if (was != g_tileKey.end()) {   // another app on the panel now: the last one's state is its own
        if (g_tileMemory.Set(was->second, g_tiles.Remember(i))) SaveTileMemory();
        g_tileKey.erase(was);
        TileUnanchor(i, "another app on it");   // (left in the room, off the grid; nothing remembered for it)
    }
    g_tileKey[i] = k;
    if (g_tileMemory.Used(k)) SaveTileMemory();   // (its use kept over a restart: the last to go when it's full)
    const std::string line = g_tileMemory.Get(k);
    if (line.empty()) return;
    const int after = g_tileFocus >= 0 && g_tiles.Tiled(g_tileFocus) ? g_tileFocus : -1;
    if (!g_tiles.Restore(i, line, after)) return (void)std::printf("%s: %s's remembered state isn't one: ignored\n", TileLabel(i).c_str(), k.c_str());
    LightAnchor(g_screens[i]);
    if (g_tiles.Anchored(i)) {
        if (!g_tileGroupSet) g_tileRegroup = true;
        RetileDash();
        std::printf("%s: back on the dashboard, as %s was (%s)\n", TileLabel(i).c_str(), k.c_str(), line.c_str());
    }
}
// The panel goes (its window, its display): what it was is remembered under its key.
void TileGone(int i) {
    const auto k = g_tileKey.find(i);
    if (k != g_tileKey.end() && g_tileMemory.Set(k->second, g_tiles.Remember(i))) SaveTileMemory();
    if (k != g_tileKey.end()) g_tileKey.erase(k);
    if (g_tileFocus == i) g_tileFocus = -1;
    if (g_tiles.Unanchor(i)) RetileDash(), RememberTiles();
}

// Anchored panels (dashtiles.h): shown with the dashboard, or always when they stay; the hide key
// (the manual switch in "always") and a panel's own conceal still hide them. The visibility mode
// is the room's panels' (user, 2026-11-16).
bool TileShown(const Screen &s, int i, bool dashUp) {
    const bool hiddenByKey = EffectiveMode() == Mode::Always && g_manual;
    return !g_paused && s.shown && !s.alone && !hiddenByKey && (g_tiles.Stay(i) || dashUp || s.drag != Drag::None);
}

// Let go after a move: where on the grid it goes, or torn off as a satellite where it is.
void TileDrop(Screen &s, int i) {
    Mat p;
    if (!g_tileGroupSet || !ScreenPose(s, &p)) return;
    const Mat q = Mul(Inverse(Mul(DashFrame(), g_tileGroupRel)), p);
    const double pt[3] = {q.m[0][3], q.m[1][3], q.m[2][3]};
    double az, el, rel[3][4];
    tiling::Direction(pt, &az, &el);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) rel[r][c] = q.m[r][c];
    const tiling::Drop d = g_tiles.DropFor(i, az, el, TileInfo);
    g_tiles.Drop(i, d, rel);
    static const char *kinds[] = {"into column", "a new column at", "torn off (a satellite) at"};
    std::printf("%s: let go at %.0f, %.0f degrees: %s %d\n", TileLabel(i).c_str(), az, el, kinds[d.kind], d.col + 1);
    RetileDash();
    RememberTiles();
}
// Resized by its corner on the grid: that size from now on (the wall puts it back to its shape).
void TileResized(Screen &s, int i) {
    const tiling::Placed *p = g_tiles.PlacedOf(i);
    if (!p || p->widthM <= 0 || p->heightM <= 0) return;
    g_tiles.Resized(i, std::sqrt(s.metres / p->widthM * s.heightMetres() / p->heightM));
    RetileDash();
    RememberTiles();
}

namespace {
// The panel the tiling keys act on: the one under the pointer, else the one you're looking at
// (within 25 degrees of straight ahead). The relay is told when it's an anchored one (the keys are
// the desktop's otherwise), and it gets a thin border.
void UpdateTileFocus() {
    int f = -1;
    const auto m = g_screens.find(g_mouseScreen);
    if (m != g_screens.end() && m->second.visible) f = g_mouseScreen;
    Mat head;
    if (f < 0 && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        const double fwd[3] = {-head.m[0][2], -head.m[1][2], -head.m[2][2]};
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
    const bool tile = f >= 0 && g_tiles.Anchored(f);
    const int64_t now = NowMs();
    if (int(tile) != g_tileFocusSaid || (tile && now - g_tileFocusSaidMs >= kTileFocusRenewMs)) {
        SendRelay(tile ? "tilefocus 1" : "tilefocus 0");
        g_tileFocusSaid = tile, g_tileFocusSaidMs = now;
    }
}
}  // namespace

// The border: one overlay, behind the focused tile, a frame its shape. Placed after the tick's
// moves (the end of ft_vr_poll) and whenever the tiles are (RetileDash), so it keeps up with its tile.
void PlaceTileBorder() {
    const int f = g_tileFocus;
    const auto it = g_screens.find(f);
    const bool tile = it != g_screens.end() && it->second.visible && g_tiles.Anchored(f);
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
    const int tw = 256, th = std::clamp(int(std::lround(256 * h / w)), 8, 2048);
    if (tw != b.texW || th != b.texH) {
        const int edgeX = std::max(2, int(std::lround(tw * kTileBorder / w))), edgeY = std::max(2, int(std::lround(th * kTileBorder / h)));
        std::vector<uint8_t> px(size_t(tw) * th * 4, 0);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                if (x >= edgeX && x < tw - edgeX && y >= edgeY && y < th - edgeY) continue;
                uint8_t *q = &px[(size_t(y) * tw + x) * 4];
                q[0] = 90, q[1] = 160, q[2] = 255, q[3] = 220;
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

// Each tick, after the dashboard pins have moved.
void UpdateDashTiles() {
    FlattenTiles();   // (a curve given a tiled panel since: its curve button, a layout put back)
    const bool up = vr::VROverlay()->IsDashboardVisible();
    if (up && !g_dashWasUp) g_tileRegroup = true;
    g_dashWasUp = up;
    // (a read that counts: this opening's, or before the group was ever set the last good one)
    const bool fresh = !Dash().why || (!g_tileGroupSet && !std::strcmp(Dash().why, "hidden"));
    if (g_tileRegroup && Dash().ever && fresh && !g_tiles.Ids().empty() && Regroup()) g_tileRegroup = false;
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
                    tiling::Direction(pt, &az, &el);
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
    const double s0 = l.slotAz - g_tiles.params.reserveDeg / 2, s1 = l.slotAz + g_tiles.params.reserveDeg / 2;
    for (const Seen &a : seen) {
        if (l.split >= 0 && a.col == l.split - 1) slotL = std::min(slotL, s0 - a.az1);
        if (l.split >= 0 && a.col == l.split) slotR = std::min(slotR, a.az0 - s1);
        for (const Seen &b : seen) {
            if (b.col == a.col && b.row == a.row + 1) gap = std::min(gap, a.el0 - b.el1);   // (rows top down)
            if (b.col == a.col + 1 && b.el0 < a.el1 && a.el0 < b.el1)
                gap = std::min(gap, b.az0 - a.az1 - (l.split == b.col ? s1 - s0 + g_tiles.params.gapDeg : 0));
        }
    }
    std::string out;
    char part[160];
    std::snprintf(part, sizeof part, "ok smallest gap %.1f degrees (wanted %.1f; none: 1e9), Steam's slot clear by %.1f left, %.1f right",
                  gap, g_tiles.params.gapDeg, slotL, slotR);
    out = part;
    for (const Seen &v : seen) {
        std::snprintf(part, sizeof part, "; %d: column %d row %d, %.1f..%.1f round, %.1f..%.1f up, curve %.2f m", v.id + 1, v.col,
                      v.row, v.az0, v.az1, v.el0, v.el1, v.curve);
        out += part;
    }
    std::snprintf(reply, size, "%s", out.c_str());
}
}  // namespace

// The anchor button: a click anchors or lets go, a long press (kAnchorStayMs) makes it stay shown
// with the dashboard closed, or not (anchoring it if it wasn't).
void AnchorButtonUp(int i, const char *by) {
    auto it = g_screens.find(i);
    if (it == g_screens.end() || it->second.anchorDownMs < 0) return;
    const bool longPress = NowMs() - it->second.anchorDownMs >= kAnchorStayMs;
    it->second.anchorDownMs = -1;
    if (longPress) TileStay(i, !g_tiles.Stay(i), by);
    else if (g_tiles.Anchored(i)) TileUnanchor(i, by);
    else TileAnchor(i, g_tileStayDefault, by);
}

// The anchor button blue while anchored, ringed while it stays shown too.
void LightAnchor(Screen &s) {
    const int i = ScreenIndexOf(s);
    const int lit = !g_tiles.Anchored(i) ? 0 : g_tiles.Stay(i) ? 2 : 1;
    if (s.anchorButton == vr::k_ulOverlayHandleInvalid || s.anchorLit == lit) return;
    s.anchorLit = lit;
    static const std::vector<uint8_t> tex[3] = {AnchorTexture(64, 0), AnchorTexture(64, 1), AnchorTexture(64, 2)};
    overlaytex::Set(s.anchorButton, tex[lit].data(), 64, 64);
}

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
        else if (to) TileAnchor(i, g_tileStayDefault, "command");
        else TileUnanchor(i, "command");
        std::snprintf(reply, size, "ok %s %s", anchor ? "anchored" : "stay", to ? "on" : "off");
        return;
    }
    case vrcmd::Tile: {
        dashtiles::Key key;
        if (!dashtiles::ParseKey(parsed.word, &key)) return (void)std::snprintf(reply, size, "error no such tiling key");
        const int i = g_tileFocus;
        if (i < 0 || !g_tiles.Tiled(i)) return (void)std::snprintf(reply, size, "ok no tile has the focus");
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
        if (!std::strcmp(parsed.rest, "clear")) {
            for (const int id : g_tiles.Ids()) TileUnanchor(id, "tiles clear");
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
        for (const auto &[i, state] : saved) LightAnchor(g_screens[i]);
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
        LightAnchor(*s);
        std::snprintf(reply, size, "ok");
        return;
    }
    default:
        std::snprintf(reply, size, "error unknown command");
    }
}

}  // namespace ftvr
