// Where the wearer looks, behind lines of text (gazeask.h).
#include "gazeask.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace local {
namespace {

constexpr const char *kUsage = "error gaze | gaze on | gaze off | gaze say YAW PITCH | gaze pointer eyes|hand";

// A number, and nothing after it. `s`: a word, so not empty.
bool Number(const std::string &s, double &out) {
    char *end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return s.size() <= 16 && !*end;
}

const char *Is(bool b) { return b ? "true" : "false"; }

}  // namespace

bool GazeAsk(Gaze &gaze, const std::string &asked, bool inRoom, double now, std::string &reply) {
    if (asked != "gaze" && asked.rfind("gaze ", 0) != 0) return false;
    std::istringstream in(asked);
    std::vector<std::string> w;
    for (std::string word; in >> word;) w.push_back(word);
    const std::string verb = w.size() > 1 ? w[1] : "";
    double yaw = 0, pitch = 0;
    if (w.size() == 1) {
        reply = std::string("ok {\"shared\":") + Is(gaze.shared()) + ",\"fresh\":" + Is(gaze.fresh(now)) + ",\"quiet\":" + Is(gaze.quiet(now)) + "}";
    } else if (verb == "on" && w.size() == 2) {
        if (inRoom) gaze.Share(true);
        reply = inRoom ? "ok" : "error you aren't in a room";
    } else if (verb == "off" && w.size() == 2) {
        gaze.Share(false), reply = "ok";
    } else if (verb == "say" && w.size() == 4 && Number(w[2], yaw) && Number(w[3], pitch)) {
        reply = gaze.Say(yaw, pitch, now) ? "ok" : gaze.shared() ? "error that is no direction" : "error you don't share where you look";
    } else if (verb == "pointer" && w.size() == 3 && (w[2] == "eyes" || w[2] == "hand")) {
        gaze.Pointer(w[2] == "eyes", now), reply = "ok";
    } else {
        reply = kUsage;
    }
    return true;
}

}  // namespace local
