// A dashboard panel's remembered state as a record (dashtiles.h's Board writes and reads it), and
// the batch of them a profile put back last. The text form is what ft-screens' tile memory and the
// `tiles` command carry: "off", "tile STAY COL ROW ALONE WEIGHT SCALE WIDTH" or "sat STAY" and the
// satellite's 12 numbers. Parse takes any whitespace between the words and nothing else.
// Header only, no OpenVR, so its test runs anywhere (screens/tests/dashtiles-test.cpp).
#pragma once

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dashtiles {

struct State {
    enum class Kind { Off, Tile, Sat };
    Kind kind = Kind::Off;
    bool stay = false;
    int col = 0, row = 0;   // a tile's place on the grid when it was saved (negative: none)
    bool alone = true;      // a column of its own
    double weight = 1, scale = 1, width = 0;
    double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // a satellite's: group frame -> panel

    // A tile with a place on the grid (else it goes where a new one would).
    bool Placed() const { return kind == Kind::Tile && col >= 0 && row >= 0; }
};

namespace detail {
inline std::vector<std::string> Words(const std::string &s) {
    std::vector<std::string> w;
    size_t at = 0;
    while (at < s.size()) {
        while (at < s.size() && std::strchr(" \t\r\n\f\v", s[at])) ++at;
        size_t end = at;
        while (end < s.size() && !std::strchr(" \t\r\n\f\v", s[end])) ++end;
        if (end > at) w.push_back(s.substr(at, end - at));
        at = end;
    }
    return w;
}
inline bool Int(const std::string &w, int *out) {
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(w.c_str(), &end, 10);
    if (errno || *end || end == w.c_str() || v < -1000000 || v > 1000000) return false;
    return *out = int(v), true;
}
inline bool Num(const std::string &w, double *out) {
    char *end = nullptr;
    const double v = std::strtod(w.c_str(), &end);
    if (*end || end == w.c_str() || !std::isfinite(v)) return false;
    return *out = v, true;
}
}  // namespace detail

// False (and `out` as it was) for text that isn't one.
inline bool Parse(const std::string &text, State *out) {
    const std::vector<std::string> w = detail::Words(text);
    if (w.empty()) return false;
    State s;
    int stay = 0, alone = 1;
    if (w[0] == "off" && w.size() == 1) return *out = s, true;
    if (w[0] == "tile" && w.size() == 8) {
        if (!detail::Int(w[1], &stay) || !detail::Int(w[2], &s.col) || !detail::Int(w[3], &s.row) ||
            !detail::Int(w[4], &alone) || !detail::Num(w[5], &s.weight) || !detail::Num(w[6], &s.scale) ||
            !detail::Num(w[7], &s.width))
            return false;
        s.kind = State::Kind::Tile, s.stay = stay != 0, s.alone = alone != 0;
        return *out = s, true;
    }
    if (w[0] == "sat" && w.size() == 14) {
        if (!detail::Int(w[1], &stay)) return false;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!detail::Num(w[size_t(2 + r * 4 + c)], &s.rel[r][c])) return false;
        s.kind = State::Kind::Sat, s.stay = stay != 0;
        return *out = s, true;
    }
    return false;
}

inline std::string Text(const State &s) {
    char b[512];
    switch (s.kind) {
        case State::Kind::Off: return "off";
        case State::Kind::Tile:
            std::snprintf(b, sizeof b, "tile %d %d %d %d %.3f %.3f %.2f", s.stay ? 1 : 0, s.col, s.row, s.alone ? 1 : 0,
                          s.weight, s.scale, s.width);
            return b;
        case State::Kind::Sat: {
            int n = std::snprintf(b, sizeof b, "sat %d", s.stay ? 1 : 0);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) n += std::snprintf(b + n, sizeof b - size_t(n), " %.4f", s.rel[r][c]);
            return b;
        }
    }
    return "off";
}

// The profile a Board put back last (each panel's state, as it came), until a tile is moved,
// resized or torn off (an add after that would undo it) or it's ended. Its mode: whether its
// tiles went first (a set) or after the others (an add on its own).
class Batch {
public:
    enum class Mode { None, First, After };

    Mode mode() const { return mode_; }
    const std::vector<std::pair<int, State>> &States() const { return states_; }
    void Start(std::vector<std::pair<int, State>> states, Mode mode) { states_ = std::move(states), mode_ = mode; }
    void End() { states_.clear(), mode_ = Mode::None; }
    void Forget(int id) {
        for (auto b = states_.begin(); b != states_.end();) b = b->first == id ? states_.erase(b) : std::next(b);
    }
    void SetStay(int id, bool on) {
        for (auto &[i, s] : states_)
            if (i == id) s.stay = on;
    }

private:
    std::vector<std::pair<int, State>> states_;
    Mode mode_ = Mode::None;
};

}  // namespace dashtiles
