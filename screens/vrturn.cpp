// The two-hand turn of a panel a hand ray carries (the pointer helper's twohand.h): while one hand's ray
// drags a panel by its bar, the other hand's pinch held as well turns it as if both hands held it (user,
// 2026-11-21). The helper sends "turn begin", then "turn move" and twelve numbers (rows of the turn R since
// the begin, the move t in the last column), then "turn end": the panel turns about its own centre by R and
// moves by t, like a card in the hand however far off it is. Meanwhile the carry doesn't
// place the panel; at the end it carries on from where the hands left it, keeping their turn (dragTurn,
// from facing the head, which a hand ray's carry keeps).
//   Not a tile (tiled panels sit on the dashboard's grid), a panel glued to another, or the layout's carry.
#include "vrpanel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ftvr {

namespace {

// The panel a hand ray carries now, if one may turn.
Screen *Turnable() {
    for (auto &[index, s] : g_screens)
        if (s.drag == Drag::Move && HandRay(s.dragDevice) && !s.toggleCarry && !s.layoutHeld && s.glueTo < 0 &&
            s.pinTarget == kNone && !TileTiled(index))
            return &s;
    return nullptr;
}

bool ParseMove(const char *words, Mat *out) {
    char *end = nullptr;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            const double v = std::strtod(words, &end);
            if (end == words || !std::isfinite(v) || std::fabs(v) > 100) return false;
            out->m[r][c] = float(v), words = end;
        }
    return true;
}

}  // namespace

void TurnWord(const char *words, char *reply, int size) {
    Screen *s = Turnable();
    if (!s) return (void)std::snprintf(reply, size, "ok nothing carried to turn");
    Mat p;
    if (!std::strcmp(words, "begin")) {
        if (!ScreenPose(*s, &p)) return (void)std::snprintf(reply, size, "error no pose");
        s->turnFrom = p, s->turning = true;
        std::printf("screen %d: two-hand turn\n", ScreenIndexOf(*s) + 1);
    } else if (!std::strcmp(words, "end")) {
        if (!s->turning) return (void)std::snprintf(reply, size, "ok");
        s->turning = false;
        Mat d, head;
        // the carry goes on from here: the same grip on the device, and (facing the head) the hands' turn
        if (DevicePose(s->dragDevice, &d) && ScreenPose(*s, &p)) {
            s->dragRel = Mul(Inverse(d), p);
            if (s->dragFacing && DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) s->dragTurn = FacingTurn(p, head);
        }
        std::printf("screen %d: two-hand turn ended\n", ScreenIndexOf(*s) + 1);
    } else if (!std::strncmp(words, "move ", 5)) {
        Mat mv;
        if (!s->turning) return (void)std::snprintf(reply, size, "ok not turning");
        if (!ParseMove(words + 5, &mv)) return (void)std::snprintf(reply, size, "error turn move: twelve numbers");
        Mat p = s->turnFrom;   // turned about its own centre, then moved
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                p.m[r][c] = mv.m[r][0] * s->turnFrom.m[0][c] + mv.m[r][1] * s->turnFrom.m[1][c] + mv.m[r][2] * s->turnFrom.m[2][c];
        for (int r = 0; r < 3; ++r) p.m[r][3] = s->turnFrom.m[r][3] + mv.m[r][3];
        SetAbsolute(*s, p);
    } else {
        return (void)std::snprintf(reply, size, "error turn begin|move ...|end");
    }
    std::snprintf(reply, size, "ok");
}

}  // namespace ftvr
