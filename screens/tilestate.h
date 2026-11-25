// A dashboard panel's remembered state as a record (dashtiles.h's Board writes and reads it), and
// the batch of them a profile put back last. The text form is what ft-screens' tile memory and the
// `tiles` command carry: "off", "room" (off, and put in the room on purpose: Placement's In the
// Room), "tile STAY COL ROW ALONE SCALE [PLACE]" (PLACE: u or d, above the desk line or under it, then
// l, c or r, the way it leans; left out: "uc") or "sat STAY" and the satellite's 12 numbers. STAY: 0 as DASH_TILE_STAY says, 1 shown with the dashboard closed, 2 hidden then (any
// other number is 1, as it was when STAY was a yes or no: its 0 was "no", the default then). A tile's
// line may end with any of "centre" (the centre display you chose), "arc" (curved round the tiles'
// arc while it's the centre), "row" (it starts a shelf of its own: tilewide.h) and "nudge=DEG"
// (moved up, or down when negative, on its own). Parse takes any whitespace between the words and
// nothing else, and the sphere's older "tile STAY COL ROW ALONE WEIGHT SCALE WIDTH" (its scale taken
// as the size, the rest dropped).
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

// Whether a panel stays shown with the dashboard closed: as the default (DASH_TILE_STAY) says, or
// set for it (the anchor button's long press).
enum class Stay { Default = 0, On = 1, Off = 2 };
inline int StayNumber(Stay s) { return int(s); }
inline Stay StayOf(int n) { return n == 0 ? Stay::Default : n == 2 ? Stay::Off : Stay::On; }

struct State {
    enum class Kind { Off, Tile, Sat };
    Kind kind = Kind::Off;
    Stay stay = Stay::Default;
    int col = 0, row = 0;   // a tile's place on the grid when it was saved (negative: none)
    bool alone = true;      // a column of its own
    double scale = 1;       // its size, times its natural one
    bool below = false;     // hanging under the desk line (tilegrid.h's Col::base)
    int align = 0;          // leaning against its column's left (-1) or right (+1) edge
    double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // a satellite's: group frame -> panel
    bool room = false;      // off: put in the room on purpose (a window stays there next time: TileWhenKnown)
    bool centre = false;    // the centre display, chosen (a drop onto the one before: Board::TakeCentre)
    bool arc = false;       // ...curved round the tiles' arc while it's the centre
    bool ownRow = false;    // starts a shelf (tilegrid.h's Cell::row)
    double nudgeDeg = 0;    // moved up (down: negative) on its own, degrees (Key::NudgeUp, NudgeDown)

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
// (bounded as Int is: a state's numbers are metres, weights and scales, and Text's are short)
inline bool Num(const std::string &w, double *out) {
    char *end = nullptr;
    const double v = std::strtod(w.c_str(), &end);
    if (*end || end == w.c_str() || !std::isfinite(v) || std::fabs(v) > 1e6) return false;
    return *out = v, true;
}
}  // namespace detail

// False (and `out` as it was) for text that isn't one.
inline bool Parse(const std::string &text, State *out) {
    std::vector<std::string> w = detail::Words(text);
    if (w.empty()) return false;
    State s;
    int stay = 0, alone = 1;
    if (w[0] == "off" && w.size() == 1) return *out = s, true;
    if (w[0] == "room" && w.size() == 1) return s.room = true, *out = s, true;
    // (a tile's flags at its end, each once: taken off before its words are counted)
    bool centre = false, arc = false, row = false, nudged = false;
    if (w[0] == "tile")
        while (w.size() > 6) {
            const std::string &f = w.back();
            if (f == "centre" && !centre) centre = true;
            else if (f == "arc" && !arc) arc = true;
            else if (f == "row" && !row) row = true;
            else if (f.compare(0, 6, "nudge=") == 0 && !nudged && detail::Num(f.substr(6), &s.nudgeDeg) && std::fabs(s.nudgeDeg) <= 90) nudged = true;
            else break;
            w.pop_back();
        }
    s.centre = centre, s.arc = arc, s.ownRow = row;
    if (w[0] == "tile" && (w.size() == 6 || w.size() == 7 || w.size() == 8)) {
        double old = 0;   // (the old form's weight and width)
        const bool ok = w.size() != 8 ? detail::Num(w[5], &s.scale)
                                      : detail::Num(w[5], &old) && detail::Num(w[6], &s.scale) && detail::Num(w[7], &old);
        if (w.size() == 7) {
            const std::string &pl = w[6];
            if (pl.size() != 2 || !std::strchr("ud", pl[0]) || !std::strchr("lcr", pl[1])) return false;
            s.below = pl[0] == 'd', s.align = pl[1] == 'l' ? -1 : pl[1] == 'r' ? 1 : 0;
        }
        if (!ok || !detail::Int(w[1], &stay) || !detail::Int(w[2], &s.col) || !detail::Int(w[3], &s.row) ||
            !detail::Int(w[4], &alone))
            return false;
        s.kind = State::Kind::Tile, s.stay = StayOf(stay), s.alone = alone != 0;
        return *out = s, true;
    }
    if (w[0] == "sat" && w.size() == 14) {
        if (!detail::Int(w[1], &stay)) return false;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!detail::Num(w[size_t(2 + r * 4 + c)], &s.rel[r][c])) return false;
        s.kind = State::Kind::Sat, s.stay = StayOf(stay);
        return *out = s, true;
    }
    return false;
}

// Any state's, whatever its numbers (each is formatted on its own: no fixed buffer to fill).
inline std::string Text(const State &s) {
    const auto num = [](const char *format, double v) {
        char b[400];   // (%f of the largest double is 309 digits and the decimals)
        std::snprintf(b, sizeof b, format, v);
        return std::string(b);
    };
    switch (s.kind) {
        case State::Kind::Off: return s.room ? "room" : "off";
        case State::Kind::Tile:
            return "tile " + std::to_string(StayNumber(s.stay)) + " " + std::to_string(s.col) + " " + std::to_string(s.row) + " " +
                   std::to_string(s.alone ? 1 : 0) + num(" %.3f", s.scale) +
                   (s.below || s.align ? std::string(" ") + (s.below ? 'd' : 'u') + (s.align < 0 ? 'l' : s.align > 0 ? 'r' : 'c') : "") +
                   (s.centre ? " centre" : "") + (s.arc ? " arc" : "") + (s.ownRow ? " row" : "") +
                   (std::fabs(s.nudgeDeg) >= 0.05 ? num(" nudge=%.1f", s.nudgeDeg) : "");
        case State::Kind::Sat: {
            std::string t = "sat " + std::to_string(StayNumber(s.stay));
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) t += num(" %.4f", s.rel[r][c]);
            return t;
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
    void SetStay(int id, Stay stay) {
        for (auto &[i, s] : states_)
            if (i == id) s.stay = stay;
    }

private:
    std::vector<std::pair<int, State>> states_;
    Mode mode_ = Mode::None;
};

// A window's panel (a Frame window floating, "app:ID"; a PC window, "remote:HOST/app:APP" or
// "remote:HOST/window:ADDRESS") goes on the tiles when it comes, unless its app was put in the room
// on purpose (user, 2026-11-19: "make the default for windows be tiled mode when they are created
// unless they are configured for free float in the room"), or new windows are set to come in the
// room (NEW_WINDOWS_TILED 0; user, 2026-11-24: "choose whether new windows should default to tiled
// or in the world space"): then only one tiled before goes back on the tiles. Others (screens,
// remote displays) as they were.
inline bool IsWindowKey(const std::string &key) {
    return key.rfind("app:", 0) == 0 ||
           (key.rfind("remote:", 0) == 0 && (key.find("/app:") != std::string::npos || key.find("/window:") != std::string::npos));
}
enum class WhenKnown { Restore, Anchor, Leave };
// What the panel with `key` does when it comes, its app's remembered `line` (empty: none): Restore
// that line, Anchor it as a new tile, or Leave it where it is. `newWindowsTiled`: a window with no
// place of its own goes on the tiles (else it's left in the room).
inline WhenKnown TileWhenKnown(const std::string &key, const std::string &line, bool newWindowsTiled = true) {
    State s;
    const bool parsed = !line.empty() && Parse(line, &s);
    if (parsed && s.kind != State::Kind::Off) return WhenKnown::Restore;
    if (parsed && s.room) return WhenKnown::Leave;
    if (IsWindowKey(key)) return newWindowsTiled ? WhenKnown::Anchor : WhenKnown::Leave;
    return line.empty() ? WhenKnown::Leave : WhenKnown::Restore;   // (a line that isn't one: Restore says so)
}

}  // namespace dashtiles
