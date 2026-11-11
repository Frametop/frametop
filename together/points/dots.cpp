// The marks as SteamVR overlays (dots.h).
#include "dots.h"

#include "../../avatar/abstract/colour.h"
#include "dotimage.h"
#include "surface.h"

#include <cstdio>

namespace points {

namespace {

Place FromVr(const vr::HmdMatrix34_t &m) {
    Place p;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) p.m[i][j] = m.m[i][j];
    return p;
}

vr::HmdMatrix34_t ToVr(const Place &p) {
    vr::HmdMatrix34_t m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m.m[i][j] = float(p.m[i][j]);
    return m;
}

// The overlay with `key`, made or, when an earlier run of this program left it, taken over.
bool Make(const std::string &key, vr::VROverlayHandle_t &out) {
    vr::IVROverlay *ov = vr::VROverlay();
    vr::EVROverlayError err = ov->CreateOverlay(key.c_str(), "Frametop Together pointer", &out);
    if (err == vr::VROverlayError_KeyInUse) err = ov->FindOverlay(key.c_str(), &out);
    return err == vr::VROverlayError_None;
}

}  // namespace

Dots::Dots() {
    for (int place = 0; place < proto::kPeople; ++place) mark_[place][0] = mark_[place][1] = vr::k_ulOverlayHandleInvalid, showing_[place] = -1;
}

bool Dots::Up(double now) {
    if (up_) return true;
    if (now - tried_ < kRetryS) return false;
    tried_ = now;
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) return false;   // SteamVR isn't running: tried again later
    up_ = true;
    for (int place = 0; place < proto::kPeople; ++place)
        for (int held = 0; held < 2; ++held) {
            // Each picture is given once: SteamVR never gives back what SetOverlayRaw takes
            // (screens/overlaytex.h), so there is one overlay for each, not one whose picture changes.
            const std::string key = "frametop.roompoint." + std::to_string(place + 1) + (held ? ".held" : "");
            std::vector<uint8_t> px = DotImage(abstract::Colour(place), held != 0);
            vr::VROverlayHandle_t &mark = mark_[place][held];
            if (!Make(key, mark) || vr::VROverlay()->SetOverlayRaw(mark, px.data(), kDotPixels, kDotPixels, 4) != vr::VROverlayError_None) {
                std::fprintf(stderr, "ft-roompoints: SteamVR gave no overlay for %s\n", key.c_str());
                return Down(), false;
            }
            vr::VROverlay()->SetOverlaySortOrder(mark, kSortOrder);
        }
    std::printf("ft-roompoints: drawing in SteamVR\n");
    return true;
}

void Dots::Down() {
    if (!up_) return;
    for (int place = 0; place < proto::kPeople; ++place) {
        for (vr::VROverlayHandle_t &mark : mark_[place]) {
            if (mark != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(mark);
            mark = vr::k_ulOverlayHandleInvalid;
        }
        showing_[place] = -1;
    }
    panels_.clear();
    vr::VR_Shutdown();
    up_ = false;
    std::printf("ft-roompoints: let go of SteamVR\n");
}

void Dots::Tick(double now) {
    if (!up_) return;
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType != vr::VREvent_Quit) continue;
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        Down();
        tried_ = now;   // SteamVR is on its way out: not joined again at once
        return;
    }
}

// The panel with `key` as SteamVR has it now, or nullptr when it has none.
Dots::Panel *Dots::Find(const std::string &key, double now) {
    vr::IVROverlay *ov = vr::VROverlay();
    Panel &p = panels_[key];
    if (!p.found) {
        if (now - p.looked < kFindS) return nullptr;
        p.looked = now, p.measured = -1e9;
        if (ov->FindOverlay(key.c_str(), &p.handle) != vr::VROverlayError_None) return nullptr;
        p.found = true;
    }
    if (now - p.measured >= kMeasureS) {
        p.measured = now;
        // An overlay its maker took away, and perhaps made again: looked for anew.
        if (ov->GetOverlayWidthInMeters(p.handle, &p.metres) != vr::VROverlayError_None || ov->GetOverlayCurvature(p.handle, &p.curvature) != vr::VROverlayError_None)
            return p.found = false, p.looked = now, nullptr;
    }
    return &p;
}

bool Dots::Show(int place, const PanelKey &panel, const Mark &mark, double now) {
    if (place < 0 || place >= proto::kPeople || !Up(now)) return false;
    vr::IVROverlay *ov = vr::VROverlay();
    const Panel *p = Find(panel.key, now);
    vr::VROverlayTransformType type = vr::VROverlayTransform_Invalid;
    if (!p || p->metres <= 0 || !ov->IsOverlayVisible(p->handle) || ov->GetOverlayTransformType(p->handle, &type) != vr::VROverlayError_None) return Hide(place), false;

    Surface surface;
    surface.metres = p->metres, surface.curvature = p->curvature, surface.width = panel.width, surface.height = panel.height;
    const Place on = On(surface, mark.x, mark.y, kLiftM);
    const int held = mark.pressed ? 1 : 0;
    const vr::VROverlayHandle_t dot = mark_[place][held];
    vr::HmdMatrix34_t at = {};
    vr::EVROverlayError err = vr::VROverlayError_InvalidParameter;
    if (type == vr::VROverlayTransform_Absolute) {
        vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
        if ((err = ov->GetOverlayTransformAbsolute(p->handle, &origin, &at)) == vr::VROverlayError_None) {
            const vr::HmdMatrix34_t to = ToVr(Then(FromVr(at), on));
            err = ov->SetOverlayTransformAbsolute(dot, origin, &to);
        }
    } else if (type == vr::VROverlayTransform_TrackedDeviceRelative) {   // a panel pinned to the view
        vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid;
        if ((err = ov->GetOverlayTransformTrackedDeviceRelative(p->handle, &device, &at)) == vr::VROverlayError_None) {
            const vr::HmdMatrix34_t to = ToVr(Then(FromVr(at), on));
            err = ov->SetOverlayTransformTrackedDeviceRelative(dot, device, &to);
        }
    }
    if (err != vr::VROverlayError_None) return Hide(place), false;
    ov->SetOverlayWidthInMeters(dot, float(MarkMetres(p->metres)));
    if (showing_[place] != held) {
        ov->ShowOverlay(dot);
        if (showing_[place] >= 0) ov->HideOverlay(mark_[place][showing_[place]]);
        showing_[place] = held;
    }
    return true;
}

void Dots::Hide(int place) {
    if (!up_ || place < 0 || place >= proto::kPeople || showing_[place] < 0) return;
    vr::VROverlay()->HideOverlay(mark_[place][showing_[place]]);
    showing_[place] = -1;
}

}  // namespace points
