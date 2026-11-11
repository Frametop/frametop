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

// share K W H X Y Z QW QX QY QZ METRES CURVE [NAME...], after its verb. The session checks
// what the numbers say (proto::Fits); here they only have to be numbers.
bool Panel(const std::vector<std::string> &w, int &number, proto::Panel &panel) {
    double v[12];
    if (w.size() < 13) return false;
    for (int i = 0; i < 12; ++i)
        if (!Number(w[size_t(i) + 1], -1e6, 1e6, v[i])) return false;
    if (v[0] != std::floor(v[0]) || v[1] != std::floor(v[1]) || v[2] != std::floor(v[2]) || v[1] < 0 || v[1] > 65535 || v[2] < 0 || v[2] > 65535) return false;
    number = int(v[0]), panel.width = uint16_t(v[1]), panel.height = uint16_t(v[2]);
    for (int i = 0; i < 3; ++i) panel.at[i] = float(v[3 + i]);
    for (int i = 0; i < 4; ++i) panel.turn[i] = float(v[6 + i]);
    panel.metres = float(v[10]), panel.curve = float(v[11]);
    for (size_t i = 13; i < w.size(); ++i) panel.name += (i > 13 ? " " : "") + w[i];
    return true;
}

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
    if (verb == "share") {
        int number = 0;
        proto::Panel panel;
        if (!Panel(w, number, panel)) return "error share K W H X Y Z QW QX QY QZ METRES CURVE [NAME]";
        return Said(session.Share(number, panel));
    }
    if (verb == "unshare" && w.size() == 2) {
        double number = 0;
        if (!Number(w[1], 0, 255, number) || number != std::floor(number)) return "error unshare K";
        return Said(session.Unshare(int(number)));
    }
    return "error state|open [X Z YAW]|invite|join HOST PORT CODE|accept N|decline N|away game|off|back|leave|share ...|unshare K";
}

std::string State(const Session &session) {
    static const char *const kModes[] = {"idle", "hosting", "calling", "joined"};
    const proto::Room &room = session.room();
    std::ostringstream out;
    out << "{\"mode\":\"" << kModes[int(session.mode())] << "\",\"note\":" << Quoted(session.note()) << ",\"me\":" << session.me() << ",\"away\":" << int(session.away()) << ",\"owner\":" << int(room.owner) << ",\"people\":[";
    for (size_t i = 0; i < room.people.size(); ++i) {
        const proto::Person &p = room.people[i];
        out << (i ? "," : "") << "{\"id\":" << int(p.id) << ",\"name\":" << Quoted(p.name) << ",\"login\":" << Quoted(p.login) << ",\"away\":" << int(p.away)
            << ",\"linked\":" << (session.Linked(p.id) ? "true" : "false") << ",\"spot\":[" << p.spot.at[0] << "," << p.spot.at[1] << "," << p.spot.at[2] << "," << p.spot.yaw << "]}";
    }
    out << "],\"panels\":[";
    for (size_t i = 0; i < room.panels.size(); ++i) {
        const proto::Panel &p = room.panels[i];
        out << (i ? "," : "") << "{\"id\":" << int(p.id) << ",\"owner\":" << p.owner() << ",\"number\":" << p.number() << ",\"name\":" << Quoted(p.name) << ",\"size\":[" << p.width << ","
            << p.height << "],\"at\":[" << p.at[0] << "," << p.at[1] << "," << p.at[2] << "],\"turn\":[" << p.turn[0] << "," << p.turn[1] << "," << p.turn[2] << "," << p.turn[3]
            << "],\"metres\":" << p.metres << ",\"curve\":" << p.curve << "}";
    }
    out << "],\"asks\":[";
    const std::vector<Ask> asks = session.asks();
    for (size_t i = 0; i < asks.size(); ++i)
        out << (i ? "," : "") << "{\"id\":" << asks[i].id << ",\"name\":" << Quoted(asks[i].name) << ",\"login\":" << Quoted(asks[i].login) << ",\"address\":" << Quoted(asks[i].address) << "}";
    out << "],\"codes\":" << session.codes() << "}";
    return out.str();
}

}  // namespace session
