// Where this Frame's own floor lies in the room it is in (room/seats.h: "a guest's own floor is
// laid on their spot"): a place on your floor, turned by YAW radians about the way up and then
// moved by X and Z metres, is that place in the room. Everything of yours that draws the room
// or tells it where you are needs the same answer, so ft-together keeps it: the first program
// to know where you stood when you came in says it (avatar/room/floor.h has the sums), and it
// stands until you leave the room.
//
//   floor            -> "ok X Z YAW", or "ok none" while nobody has said
//   floor X Z YAW    -> said, unless one stands already: "ok" and the one that stands
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
        double x = 0, z = 0, yaw = 0;
        char more = 0;
        const int got = std::sscanf(text.c_str(), "floor %lf %lf %lf %c", &x, &z, &yaw, &more);
        if (text != "floor" && (got != 3 || !(std::fabs(x) <= kFurthest && std::fabs(z) <= kFurthest && std::fabs(yaw) <= 7))) return "error floor [X Z YAW]";
        if (got == 3 && !laid_) laid_ = true, x_ = x, z_ = z, yaw_ = yaw;
        if (!laid_) return "ok none";
        char out[96];
        std::snprintf(out, sizeof out, "ok %.6f %.6f %.6f", x_, z_, yaw_);
        return out;
    }
    // The room was left: the next one's floor is laid afresh.
    void Lift() { laid_ = false; }
    bool laid() const { return laid_; }

private:
    bool laid_ = false;
    double x_ = 0, z_ = 0, yaw_ = 0;
};

}  // namespace local
