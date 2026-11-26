// Shared normalized overlay-space hit regions. Coordinates are mouse/UV coordinates:
// x right, y up, each 0..1. Rectangles and circles match OpenVR's controller masks.
#pragma once
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace overlayregion {
struct Primitive { bool circle = false; double x = 0, y = 0, w = 0, h = 0; };
using Region = std::vector<Primitive>;
inline bool FullRectangle(const Region &region) {
    return region.size() == 1 && !region[0].circle && region[0].x == 0 && region[0].y == 0 && region[0].w == 1 && region[0].h == 1;
}
// OpenVR's controller mask uses image-space top-left coordinates, unlike its
// ray UVs and mouse events, whose Y runs bottom to top. Circle centers and
// rectangle origins need different flips (a rectangle's height is included).
inline Primitive TopLeft(const Primitive &p) {
    Primitive out = p;
    out.y = p.circle ? 1 - p.y : 1 - p.y - p.h;
    return out;
}
inline bool Contains(const Region &region, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    for (const auto &p : region) {
        if (p.circle ? std::hypot(x - p.x, y - p.y) <= p.w
                     : x >= p.x && y >= p.y && x <= p.x + p.w && y <= p.y + p.h) return true;
    }
    return false;
}
inline std::string Encode(const std::string &key, const Region &region) {
    std::ostringstream out;
    out << "region " << key << ' ' << region.size() << std::fixed << std::setprecision(8);
    for (const auto &p : region) out << ' ' << int(p.circle) << ' ' << p.x << ' ' << p.y << ' ' << p.w << ' ' << p.h;
    return out.str();
}
inline bool Decode(const char *text, std::string *key, Region *region) {
    std::istringstream in(text);
    std::string verb, name, extra;
    int count;
    Region parsed;
    if (!(in >> verb >> name >> count) || verb != "region" || name.rfind("frametop.", 0) != 0 ||
        name.size() > 128 || count < 1 || count > 32) return false;
    for (int i = 0; i < count; ++i) {
        Primitive p;
        int type;
        if (!(in >> type >> p.x >> p.y >> p.w >> p.h) || (type != 0 && type != 1)) return false;
        p.circle = type == 1;
        for (double v : {p.x, p.y, p.w, p.h}) if (!std::isfinite(v) || v < 0 || v > 1) return false;
        if (p.w <= 0 || (!p.circle && (p.h <= 0 || p.x + p.w > 1.000001 || p.y + p.h > 1.000001))) return false;
        if (p.circle && (p.x < p.w || p.y < p.w || p.x + p.w > 1.000001 || p.y + p.w > 1.000001)) return false;
        parsed.push_back(p);
    }
    if (in >> extra) return false;
    *key = name, *region = std::move(parsed);
    return true;
}
}  // namespace overlayregion
