// The marks as SteamVR overlays: for each place in the room a small disc in its colour, put on
// the panel its person points at, where they point.
//
// The panels are ft-screens' overlays (or whoever made the overlay with the key): this finds
// each by its key, reads how wide and how bent it is and where it is, and puts the mark on its
// surface (surface.h). OpenVR's overlays can't be hung on one another, so the mark is put again
// every time it is shown, and follows a panel that is moved a frame behind. A panel that isn't
// showing has no mark.
//
// SteamVR is joined when the first mark is wanted, as a background program: it isn't started
// for this, and isn't kept running by it. When SteamVR goes, this lets go and tries again later.
#pragma once

#include "drawer.h"

#include <map>
#include <openvr.h>

namespace points {

class Dots : public Drawer {
public:
    static constexpr double kRetryS = 5;      // between tries to join SteamVR
    static constexpr double kFindS = 1;       // between looks for a panel that wasn't found
    static constexpr double kMeasureS = 0.5;  // between reads of a panel's width and bend
    static constexpr double kLiftM = 0.003;   // a mark's height off its panel
    // Over the panels and their controls (screens/vr.cpp: 5 to 22), under the hands (100).
    static constexpr uint32_t kSortOrder = 30;

    Dots();
    ~Dots() override { Down(); }
    Dots(const Dots &) = delete;
    Dots &operator=(const Dots &) = delete;
    bool Show(int place, const PanelKey &panel, const Mark &mark, double now) override;
    void Hide(int place) override;
    void Tick(double now) override;

private:
    struct Panel {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        bool found = false;
        double looked = -1e9, measured = -1e9;
        float metres = 0, curvature = 0;
    };
    bool Up(double now);
    void Down();
    Panel *Find(const std::string &key, double now);

    bool up_ = false;
    double tried_ = -1e9;
    vr::VROverlayHandle_t mark_[proto::kPeople][2];   // each place's: pointing, and with a button held
    int showing_[proto::kPeople];                     // which of the two shows; -1: neither
    std::map<std::string, Panel> panels_;
};

}  // namespace points
