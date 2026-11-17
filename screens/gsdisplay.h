// Which X display gamescope's apps are on (gsforward.h): `:1` on the Frame today, with Steam on
// `:0`, but gamescope picks them as it starts. The displays to try, in order (AppDisplays): only
// GSAPP_DISPLAY when ft-screens' environment has it; else the one Steam's environment names
// (STEAM_GAME_DISPLAY_0 in /proc/PID/environ: readable only from ft-screens' own user namespace,
// not from its container), then every display with a socket in /tmp/.X11-unix, then `:1`.
// Link::Open takes the first that's a gamescope app display (it forwards windows, and it isn't
// gamescope's first Xwayland, Steam's own: GAMESCOPE_XWAYLAND_SERVER_ID 0). Header only; tested
// by screens/tests/gsforward-test.cpp.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gsforward {

constexpr const char *kDefaultAppDisplay = ":1";

// A local X display name: ":N" or ":N.S".
inline bool DisplayName(const std::string &s) {
    if (s.size() < 2 || s.size() > 16 || s[0] != ':') return false;
    size_t i = 1, digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
    if (!digits) return false;
    if (i == s.size()) return true;
    if (s[i] != '.' || ++i == s.size()) return false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    return i == s.size();
}

// STEAM_GAME_DISPLAY_0's value in an environ (NUL-separated), or "" (none, or not a display name).
inline std::string GameDisplayFrom(const std::string &environ) {
    static const std::string kKey = "STEAM_GAME_DISPLAY_0=";
    for (size_t at = 0; at < environ.size();) {
        size_t end = environ.find('\0', at);
        if (end == std::string::npos) end = environ.size();
        if (environ.compare(at, kKey.size(), kKey) == 0) {
            const std::string v = environ.substr(at + kKey.size(), end - at - kKey.size());
            return DisplayName(v) ? v : "";
        }
        at = end + 1;
    }
    return "";
}

// The displays to try, in order, from what was found: GSAPP_DISPLAY's alone (when it's a display
// name), else Steam's, the sockets' (`X0`, `X1`... in /tmp/.X11-unix, by number) and the default,
// each once.
inline std::vector<std::string> Candidates(const char *env, const std::string &steams,
                                           std::vector<std::string> sockets) {
    if (env && DisplayName(env)) return {env};
    std::vector<int> numbers;
    for (const std::string &s : sockets)
        if (s.size() > 1 && s[0] == 'X' && DisplayName(":" + s.substr(1))) numbers.push_back(std::atoi(s.c_str() + 1));
    std::sort(numbers.begin(), numbers.end());
    std::vector<std::string> out;
    auto add = [&](const std::string &d) {
        if (DisplayName(d) && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };
    add(steams);
    for (int n : numbers) add(":" + std::to_string(n));
    add(kDefaultAppDisplay);
    return out;
}

// A running Steam's STEAM_GAME_DISPLAY_0, or "".
inline std::string SteamGameDisplay() {
    std::string found;
    if (DIR *proc = opendir("/proc")) {
        while (const dirent *d = readdir(proc)) {
            if (!std::isdigit(static_cast<unsigned char>(d->d_name[0]))) continue;
            const std::string dir = std::string("/proc/") + d->d_name;
            std::ifstream comm(dir + "/comm");
            std::string name;
            if (!std::getline(comm, name) || name != "steam") continue;
            std::ifstream env(dir + "/environ", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(env)), std::istreambuf_iterator<char>());
            if (!(found = GameDisplayFrom(text)).empty()) break;
        }
        closedir(proc);
    }
    return found;
}

inline std::vector<std::string> SocketNames() {
    std::vector<std::string> sockets;
    if (DIR *dir = opendir("/tmp/.X11-unix")) {
        while (const dirent *d = readdir(dir)) sockets.push_back(d->d_name);
        closedir(dir);
    }
    return sockets;
}

inline std::vector<std::string> AppDisplays() {
    const char *env = std::getenv("GSAPP_DISPLAY");
    return Candidates(env, env && DisplayName(env) ? "" : SteamGameDisplay(), SocketNames());
}

// The displays to try for Steam's own (gamescope's first Xwayland: Role::Steam takes only that
// one), by number: `:0` on the Frame.
inline std::vector<std::string> SteamDisplays() { return Candidates(nullptr, "", SocketNames()); }

}  // namespace gsforward
