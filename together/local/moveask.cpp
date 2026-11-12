// Moving shared panels behind lines of text (moveask.h).
#include "moveask.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace local {
namespace {

constexpr const char *kUsage = "error move PANEL X Y Z W X Y Z | moves";

// A number, and nothing after it. What it may be is the move's reader's to say.
bool Number(const std::string &s, float &out) {
    char *end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (s.empty() || s.size() > 24 || *end || !std::isfinite(v)) return false;
    return out = v, true;
}

}  // namespace

bool MoveAsk(flow::Moves &moves, const std::string &asked, double now, std::string &reply) {
    if (asked == "moves") return reply = "ok " + moves.Taken(now), true;
    if (asked != "move" && asked.rfind("move ", 0) != 0) return false;
    std::istringstream in(asked);
    std::vector<std::string> w;
    for (std::string word; in >> word;) w.push_back(word);
    proto::Move move;
    float panel = 0;
    bool ok = w.size() == 9 && Number(w[1], panel) && panel >= 0 && panel <= 255 && panel == std::floor(panel);
    for (int i = 0; ok && i < 3; ++i) ok = Number(w[size_t(2 + i)], move.at[i]);
    for (int i = 0; ok && i < 4; ++i) ok = Number(w[size_t(5 + i)], move.turn[i]);
    if (!ok) return reply = kUsage, true;
    move.panel = uint8_t(panel);
    const std::string whyNot = moves.Move(move, now);
    reply = whyNot.empty() ? "ok" : "error " + whyNot;
    return true;
}

}  // namespace local
