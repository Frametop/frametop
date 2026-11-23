// The centre display's blue glow on OpenVR (centreglow.h's look; dashtiles.h's Board says which
// tile is the centre, vrtiles.cpp's TileGlowTarget which one glows now): one overlay behind the
// centre tile while the dashboard is open, or, brighter, behind a tile you drag that would take
// the centre's place if let go there. Curved as its tile is. Placed after the tick's moves and
// whenever the tiles are (vrtiles.cpp's RetileDash), as the focus border is.

#include "vrpanel.h"
#include "centreglow.h"
#include "overlaytex.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ftvr {

namespace {
struct CentreGlow {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int on = -1;                  // the panel it's round, or -1 (hidden)
    bool hot = false;             // a drag that would take the centre's place
    int texW = 0, texH = 0;
    bool texHot = false;          // what its texture was drawn for
    double width = 0, curve = -1; // as last set
    Mat at = {};
} g_glow;
}  // namespace

void PlaceCentreGlow() {
    int on = -1;
    bool hot = false;
    TileGlowTarget(&on, &hot);
    const auto it = g_screens.find(on);
    CentreGlow &g = g_glow;
    Mat p;
    if (it == g_screens.end() || !it->second.visible || !ScreenPose(it->second, &p)) {
        if (g.on >= 0 && g.overlay != vr::k_ulOverlayHandleInvalid) PinHide(g.overlay);
        g.on = -1;
        return;
    }
    if (g.overlay == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay("frametop.tilecentre", "Frametop: the centre dashboard tile", &g.overlay) !=
            vr::VROverlayError_None)
            return (void)(g.overlay = vr::k_ulOverlayHandleInvalid);
        vr::VROverlay()->SetOverlayInputMethod(g.overlay, vr::VROverlayInputMethod_None);
    }
    const Screen &s = it->second;
    Mat head;
    double dist = 2;
    if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        const double d[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
        dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    const double band = centreglow::Band(dist), w = s.metres + 2 * band, h = s.heightMetres() + 2 * band;
    const int tw = 1024, th = std::clamp(int(std::lround(1024 * h / w)), 16, 4096);
    if (tw != g.texW || th != g.texH || hot != g.texHot) {
        const int mx = std::max(1, int(std::lround(tw * band / w))), my = std::max(1, int(std::lround(th * band / h)));
        const std::vector<uint8_t> px = centreglow::Texture(tw, th, mx, my, hot ? centreglow::kHotPeak : centreglow::kPeak);
        overlaytex::Set(g.overlay, px.data(), tw, th);
        g.texW = tw, g.texH = th, g.texHot = hot;
    }
    if (std::fabs(w - g.width) > 1e-4) vr::VROverlay()->SetOverlayWidthInMeters(g.overlay, float(w)), g.width = w;
    constexpr double kBehind = 0.004;   // (behind its panel, round the same axis when curved)
    const double curve = s.curve > 0 ? s.curve + kBehind : 0;
    if (std::fabs(curve - g.curve) > 1e-6) {
        vr::VROverlay()->SetOverlayCurvature(g.overlay, float(curve > 0 ? std::clamp(w / (2 * M_PI * curve), 0.0, 1.0) : 0.0));
        g.curve = curve;
    }
    const Mat at = Mul(p, Translation(0, 0, -kBehind));
    if (g.on != on || std::memcmp(&at, &g.at, sizeof at) != 0) {
        vr::VROverlay()->SetOverlayTransformAbsolute(g.overlay, vr::TrackingUniverseStanding, &at);
        g.at = at;
    }
    if (g.on != on) PinShow(g.overlay);
    g.on = on, g.hot = hot;
}

}  // namespace ftvr
