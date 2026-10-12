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
        std::string reason, trailing;
        input >> value;
        input >> reason;
        const bool safe = reason.size() <= 96 && reason.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
        if (input >> trailing || (value != "0" && value != "1") || !safe) {
            std::snprintf(out, size, "error passthrough publisher <0|1>");
        } else {
            reason_ = reason.empty() ? "publisher-unavailable" : reason;
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
    std::snprintf(out, size, "ok portal {\"wanted\":%s,\"connected\":%s,\"reason\":\"%s\"}",
        wanted_ ? "true" : "false", connected() ? "true" : "false", connected() ? "" : reason_.c_str());
    return true;
}
}
