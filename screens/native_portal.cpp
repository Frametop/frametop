#include "native_portal.h"
#include <cstdio>
#include <sstream>
#include <string>
namespace native_portal {
bool Owner::command(const char* cmd, char* out, int size) {
    std::istringstream input(cmd);
    std::string subsystem, action, value, extra;
    input >> subsystem >> action;
    if (subsystem != "passthrough") return false;
    if (action == "publisher") {
        if (!(input >> value) || input >> extra || (value != "0" && value != "1")) {
            std::snprintf(out, size, "error passthrough publisher <0|1>");
        } else {
            publisher_ = value == "1" ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::snprintf(out, size, "ok publisher %s", value.c_str());
        }
        return true;
    }
    if (action != "portal") return false;
    if (!(input >> value) || input >> extra ||
        (value != "on" && value != "off" && value != "status")) {
        std::snprintf(out, size, "error passthrough portal <on|off|status>");
        return true;
    }
    if (value == "on") wanted_ = true;
    if (value == "off") wanted_ = false;
    std::snprintf(out, size, "ok portal {\"wanted\":%s,\"connected\":%s}",
        wanted_ ? "true" : "false", connected() ? "true" : "false");
    return true;
}
}
