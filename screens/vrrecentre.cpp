// "recentre [N BEARING]": the quick reset (ft-layout reset) on the room's panels, by recentre.h's rules. N
// is the panel they go by, BEARING where the layout has it round you (degrees, left positive); without them
// (no screen of the layout in the room: desktopless) it's the nearest you see, and the middle of what you
// see comes straight ahead. All of the room goes, shown or not: screens, remote displays, floating windows,
// apps' panels, the front slot's home and a stopped remote display's place. What's pinned to your head, a
// wrist or the dashboard stays on its pin (the tiles: "tiles recentre", vrtiles.cpp), and one held stays in
// the hand.

#include "vrpanel.h"
#include "recentre.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ftvr {

namespace {
bool InRoom(int i, const Screen &s) {
    return s.pinned == kNone && s.glueTo < 0 && (!s.floating || s.floatOn) && !HeldElsewhere(s, i);
}

double FromHead(const Screen &s, const Mat &head) {
    return std::hypot(s.pose.m[0][3] - head.m[0][3], s.pose.m[1][3] - head.m[1][3], s.pose.m[2][3] - head.m[2][3]);
}

// The panel asked for, else the nearest shown, else the nearest; -1: none in the room.
int LeadOf(int asked, const Mat &head) {
    const auto it = g_screens.find(asked);
    if (it != g_screens.end() && InRoom(asked, it->second)) return asked;
    int best = -1;
    for (const bool shown : {true, false}) {
        double nearest = 0;
        for (const auto &[i, s] : g_screens) {
            if (!InRoom(i, s) || (shown && !s.visible)) continue;
            const double d = FromHead(s, head);
            if (best < 0 || d < nearest) best = i, nearest = d;
        }
        if (best >= 0) break;
    }
    return best;
}
}  // namespace

void RecentreCommand(const vrcmd::Cmd &c, char *reply, int size) {
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head))
        return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
    LayoutLetGo("a reset");
    SettleForReset();
    const int lead = LeadOf(c.n - 1, head);
    if (lead >= 0 && lead == c.n - 1 && g_screens.at(lead).unplaced)   // (nothing of yours to keep: ft-layout arranges)
        return (void)std::snprintf(reply, size, "error not arranged yet");
    int moved = 0;
    if (lead >= 0) {
        const Screen &l = g_screens.at(lead);
        recentre::Lead<Mat> by;
        by.pose = l.pose;
        by.from = l.from;
        by.planned = lead == c.n - 1;
        by.bearing = c.yaw * M_PI / 180;
        std::vector<Mat> seen;
        for (const auto &[i, s] : g_screens)
            if (s.visible && InRoom(i, s)) seen.push_back(s.pose);
        const Mat step = recentre::Step(by, head, seen);
        const recentre::Stood here = recentre::StoodAt(head);
        for (auto &[i, s] : g_screens) {
            if (!InRoom(i, s)) continue;
            SpinForget(i);   // (turning round where you were: it stays where this puts it)
            SetAbsolute(s, Mul(step, s.pose), UniverseNow());
            s.from = here, s.unplaced = false;
            ++moved;
        }
        StepPlaces(step, &here);
        ArrangeDesktopSoon();   // KWin's outputs follow where the screens are now
        std::printf("recentred by screen %d: %d panel(s) moved %.2f m, turned %.0f degrees\n", lead + 1, moved,
                    std::hypot(l.pose.m[0][3] - by.pose.m[0][3], l.pose.m[1][3] - by.pose.m[1][3], l.pose.m[2][3] - by.pose.m[2][3]),
                    recentre::YawOf(step) * 180 / M_PI);
    }
    std::snprintf(reply, size, "ok %d %.4f %.4f %.4f %.2f", moved, head.m[0][3], head.m[1][3], head.m[2][3],
                  recentre::YawOf(head) * 180 / M_PI);
}

}  // namespace ftvr
