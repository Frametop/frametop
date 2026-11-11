// A session behind lines of text (commands.h).
#include "commands.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace session {
namespace {

std::vector<std::string> Words(const std::string &text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

bool Number(const std::string &s, double lo, double hi, double &out) {
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || *end || !std::isfinite(v) || v < lo || v > hi) return false;
    return out = v, true;
}

// Text a peer sent is text (wire::IsText): only the quote and the backslash need care.
std::string Quoted(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

std::string Said(const std::string &whyNot) { return whyNot.empty() ? "ok" : "error " + whyNot; }

}  // namespace

std::string Command(Session &session, const std::string &text, double now) {
    const std::vector<std::string> w = Words(text);
    const std::string verb = w.empty() ? "" : w[0];
    if (verb == "state" && w.size() == 1) return "ok " + State(session);
    if (verb == "open" && (w.size() == 1 || w.size() == 4)) {
        double x = 0, z = 0, yaw = 0;
        if (w.size() == 4 && !(Number(w[1], -1e6, 1e6, x) && Number(w[2], -1e6, 1e6, z) && Number(w[3], -1e6, 1e6, yaw))) return "error open [X Z YAW]";
        proto::Spot spot;
        spot.at[0] = float(x), spot.at[2] = float(z), spot.yaw = float(yaw);
        return Said(session.Open(spot));
    }
    if (verb == "invite" && w.size() == 1) {
        std::string code;
        const std::string whyNot = session.Invite(now, code);
        return whyNot.empty() ? "ok " + code : Said(whyNot);
    }
    if (verb == "join" && w.size() == 4) {
        double port = 0;
        if (!Number(w[2], 1, 65535, port) || port != std::floor(port)) return "error join HOST PORT CODE";
        return Said(session.Join(w[1], uint16_t(port), w[3], now));
    }
    if ((verb == "accept" || verb == "decline") && w.size() == 2) {
        double ask = 0;
        if (!Number(w[1], 1, 1e9, ask)) return "error no such ask";
        return Said(session.Answer(int(ask), verb == "accept"));
    }
    if (verb == "away" && w.size() == 2 && (w[1] == "game" || w[1] == "off" || w[1] == "back"))
        return Said(session.SetAway(w[1] == "game" ? proto::kAwayGame : w[1] == "off" ? proto::kAwayOff : 0));
    if (verb == "leave" && w.size() == 1) return Said(session.Leave());
    return "error state|open [X Z YAW]|invite|join HOST PORT CODE|accept N|decline N|away game|off|back|leave";
}

std::string State(const Session &session) {
    static const char *const kModes[] = {"idle", "hosting", "calling", "joined"};
    const proto::Room &room = session.room();
    std::ostringstream out;
    out << "{\"mode\":\"" << kModes[int(session.mode())] << "\",\"note\":" << Quoted(session.note()) << ",\"me\":" << session.me() << ",\"owner\":" << int(room.owner) << ",\"people\":[";
    for (size_t i = 0; i < room.people.size(); ++i) {
        const proto::Person &p = room.people[i];
        out << (i ? "," : "") << "{\"id\":" << int(p.id) << ",\"name\":" << Quoted(p.name) << ",\"login\":" << Quoted(p.login) << ",\"away\":" << int(p.away)
            << ",\"linked\":" << (session.Linked(p.id) ? "true" : "false") << ",\"spot\":[" << p.spot.at[0] << "," << p.spot.at[1] << "," << p.spot.at[2] << "," << p.spot.yaw << "]}";
    }
    out << "],\"asks\":[";
    const std::vector<Ask> asks = session.asks();
    for (size_t i = 0; i < asks.size(); ++i)
        out << (i ? "," : "") << "{\"id\":" << asks[i].id << ",\"name\":" << Quoted(asks[i].name) << ",\"login\":" << Quoted(asks[i].login) << ",\"address\":" << Quoted(asks[i].address) << "}";
    out << "],\"codes\":" << session.codes() << "}";
    return out.str();
}

}  // namespace session
