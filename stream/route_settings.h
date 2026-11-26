// The ways to a host and a remote display's stream settings for each (Route, WatchDirect and main
// in ft-stream.cpp and ft-appless.cpp). A host's entry in ~/.config/frametop-layout.json has its
// "route" (auto, network or dongle) and its "direct" addresses; a display's entry can give fps and
// bitrate per route:
//   "routes": {"direct": {"fps": 60, "bitrate": 15000}, "network": {"bitrate": 4000}}
// "direct" is a stream through one of the host's direct addresses (its dongle, or a second link
// such as its Ethernet), "network" one through its own address. What a route leaves out stays as
// ft-screens passed it, and bitrate 0 is the default for the size and fps (main). A value out of
// range is ignored. ft-layout's "remote set ID direct.bitrate=KBPS" writes them.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

constexpr int kRouteFpsMax = 240, kRouteBitrateMax = 500000;  // kbit/s; ft-layout checks the same

// A host's route (auto when it has none) and its direct addresses; known is false without an entry.
struct HostRoute {
    bool known = false;
    std::string mode = "auto";
    std::vector<std::string> direct;
};

inline HostRoute HostRouteOf(const nlohmann::json &layout, const std::string &host) {
    HostRoute out;
    if (!layout.is_object() || !layout.contains("hosts") || !layout["hosts"].is_array()) return out;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host) continue;
        out.known = true;
        if (h.contains("route") && h["route"].is_string()) out.mode = h["route"].get<std::string>();
        if (h.contains("direct") && h["direct"].is_array())
            for (const auto &d : h["direct"])
                if (d.is_string()) out.direct.push_back(d.get<std::string>());
        break;
    }
    return out;
}

// While a stream goes through the host's own address with route auto, a direct address that
// starts answering takes it over (WatchDirect): once it has answered this many checks in a row,
// kRerouteCheckSeconds apart, so a link that's still coming up isn't taken.
constexpr int kRerouteAnswers = 2, kRerouteCheckSeconds = 10;

struct RerouteWatch {
    int streak = 0;
    // One check, whether a direct address answered: true when it's time to move.
    bool Check(bool answered) {
        streak = answered ? streak + 1 : 0;
        return streak >= kRerouteAnswers;
    }
};

// Applies the route's settings to *fps and *bitrate. True if the display has settings for it.
inline bool RouteSettings(const nlohmann::json &layout, const std::string &host, const std::string &id,
                          const std::string &via, int *fps, int *bitrate) {
    if (!layout.is_object() || !layout.contains("hosts") || !layout["hosts"].is_array()) return false;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host || !h.contains("displays") || !h["displays"].is_array())
            continue;
        for (const auto &d : h["displays"]) {
            if (!d.is_object() || d.value("client", "") != id) continue;
            if (!d.contains("routes") || !d["routes"].is_object() || !d["routes"].contains(via)) return false;
            const nlohmann::json &r = d["routes"][via];
            if (!r.is_object()) return false;
            const auto take = [&r](const char *key, long long lo, long long hi, int *out) {
                if (!r.contains(key) || !r[key].is_number_integer()) return;
                const long long v = r[key].get<long long>();
                if (v >= lo && v <= hi) *out = int(v);
            };
            take("fps", 1, kRouteFpsMax, fps);
            take("bitrate", 0, kRouteBitrateMax, bitrate);
            return true;
        }
    }
    return false;
}
