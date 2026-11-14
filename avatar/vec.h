// A place or a direction in three dimensions, in doubles: what the importer (import/) and the
// pose solver (pose/) do their sums in. Header only.
#pragma once

#include <cmath>

namespace avatar {

struct Vec {
    double x = 0, y = 0, z = 0;
    Vec operator+(const Vec &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec operator-(const Vec &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const Vec &o) const { return x * o.x + y * o.y + z * o.z; }
    Vec cross(const Vec &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vec unit() const {
        const double n = norm();
        return n > 1e-12 ? *this * (1 / n) : Vec{};
    }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

}  // namespace avatar
