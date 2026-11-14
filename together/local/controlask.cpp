// Control behind lines of text (controlask.h).
#include "controlask.h"

#include "../room/panels.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace local {
namespace {

constexpr const char *kUsage = "error control | control ask PANEL | control give NUMBER PERSON [pc] | control take NUMBER [PERSON] | "
                               "control act PANEL move X Y|button B 0|1|scroll RIGHT DOWN|key CODE 0|1 | control own | control here";

// A whole number from `lo` to `hi`, and nothing after it.
bool Whole(const std::string &s, long lo, long hi, int &out) {
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || s.size() > 8 || *end || v < lo || v > hi) return false;
    return out = int(v), true;
}

std::string Said(const std::string &whyNot) { return whyNot.empty() ? "ok" : "error " + whyNot; }

// PANEL WHAT A B, after `control act`. The numbers only have to be numbers here: what they may
// be is the act's reader's to say (flow::Control::Act).
bool Act(const std::vector<std::string> &w, proto::Act &act) {
    static const char *const kWhat[] = {"", "move", "button", "scroll", "key"};
    int panel = 0, a = 0, b = 0;
    if (w.size() != 6 || !Whole(w[2], 0, 255, panel) || !Whole(w[4], -100000, 100000, a) || !Whole(w[5], -100000, 100000, b)) return false;
    for (uint8_t what = proto::kActMove; what <= proto::kActKey; ++what)
        if (w[3] == kWhat[what]) return act.panel = uint8_t(panel), act.what = what, act.a = a, act.b = b, true;
    return false;
}

}  // namespace

bool ControlAsk(flow::Control &control, const std::string &asked, double now, std::string &reply, const ActGate &gate) {
    if (asked != "control" && asked.rfind("control ", 0) != 0) return false;
    std::istringstream in(asked);
    std::vector<std::string> w;
    for (std::string word; in >> word;) w.push_back(word);
    const std::string verb = w.size() > 1 ? w[1] : "";
    int n = 0, person = -1;
    proto::Act act;
    if (w.size() == 1) reply = "ok " + control.State(now);
    else if (verb == "own" && w.size() == 2) reply = Said(control.Own(now));
    else if (verb == "here" && w.size() == 2) control.Here(now), reply = "ok";
    else if (verb == "ask" && w.size() == 3 && Whole(w[2], 0, 255, n)) reply = Said(control.Ask(n, now));
    else if (verb == "give" && (w.size() == 4 || (w.size() == 5 && w[4] == "pc")) && Whole(w[2], 0, 255, n) && Whole(w[3], 0, 255, person)) {
        // A panel of the PC's is given only when the asker says it is one: the second ask.
        const bool pcs = n >= room::kFramePanels && n < room::kOwnPanels;
        if (pcs && w.size() == 4) reply = "error panel " + w[2] + " is your PC's: whoever has it clicks and types on that PC. \"control give " + w[2] + " " + w[3] + " pc\" gives it";
        else if (!pcs && w.size() == 5) reply = "error panel " + w[2] + " isn't your PC's";
        else reply = Said(control.Give(n, person, now));
    }
    else if (verb == "take" && (w.size() == 3 || w.size() == 4) && Whole(w[2], 0, 255, n) && (w.size() == 3 || Whole(w[3], 0, 255, person))) reply = Said(control.Take(n, person));
    else if (verb == "act" && Act(w, act)) {
        std::vector<proto::Act> go;
        if (gate) gate(act, go);
        else go.push_back(act);
        reply = "ok";   // one that waits at the gate was taken
        for (const proto::Act &a : go) reply = Said(control.Act(a, now));
    }
    else reply = kUsage;
    return true;
}

}  // namespace local
