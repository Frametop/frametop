// Where this Frame's own floor lies in the room it is in (room/seats.h: "a guest's own floor is
// laid on their spot"): a place on your floor, turned by YAW radians about the way up and then
// moved by X and Z metres, is that place in the room. Everything of yours that draws the room
// or tells it where you are needs the same answer, so ft-together keeps it: the first program
// to know where you stood when you came in says it (avatar/room/floor.h has the sums), and it
// stands until you leave the room, or until you move yourself through the room: then your
// floor moves under you (ft-roompanels' `go` and `pull`).
//
//   floor                 -> "ok X Z YAW", or "ok none" while nobody has said
//   floor X Z YAW         -> said, unless one stands already: "ok" and the one that stands
//   floor move X Z YAW    -> the one that stands is this one from now on: "ok" and it. Refused
//                            while none is laid
//
// The rules only. Header only.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace local {

class Floor {
public:
    static constexpr double kFurthest = 1000;   // metres: a floor laid further off than this isn't one

    // The answer to a `floor` question (the whole of it). `inRoom`: this Frame is in a room now.
    std::string Ask(const std::string &text, bool inRoom) {
        if (!inRoom) return laid_ = false, "error you aren't in a room";
        if (text == "floor") return laid_ ? Said(now_) : "ok none";
        const char *usage = "error floor [[move] X Z YAW]";
        if (text.rfind("floor ", 0) != 0) return usage;
        const bool move = text.rfind("floor move ", 0) == 0;
        Place said;
        char more = 0;
        const int got = std::sscanf(text.c_str() + (move ? 11 : 6), "%lf %lf %lf %c", &said.x, &said.z, &said.yaw, &more);
        if (got != 3 || !(std::fabs(said.x) <= kFurthest && std::fabs(said.z) <= kFurthest && std::fabs(said.yaw) <= 7)) return usage;
        if (move && !laid_) return "error no floor is laid to move";
        if (move) now_ = said;
        else if (!laid_) laid_ = true, now_ = said;
        return Said(now_);
    }
    // The room was left: the next one's floor is laid afresh.
    void Lift() { laid_ = false; }
    bool laid() const { return laid_; }

private:
    struct Place {
        double x = 0, z = 0, yaw = 0;
    };
    static std::string Said(const Place &p) {
        char out[96];
        std::snprintf(out, sizeof out, "ok %.6f %.6f %.6f", p.x, p.z, p.yaw);
        return out;
    }
    bool laid_ = false;
    Place now_;
};

}  // namespace local
