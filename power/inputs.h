// ft-powerd's view of the input devices: which of them sent a key, a button or relative motion.
// Every /dev/input device with keys or relative axes (mice, keyboards, the headset's buttons,
// the relay's virtual devices), read without grabbing and rescanned every few seconds for new
// ones. Keyboards the relay grabs for the desktop aren't seen, but typing in the headset moves it
// anyway.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ft_power {

struct InputDev {
    int fd;  // -1: not open (no keys or relative axes, or it stopped reading)
    std::string name;
    bool ignored;  // no keys or relative axes: not opened again
};

inline bool HasBit(const unsigned long *bits, int bit) {
    return bits[bit / (8 * sizeof(long))] & (1UL << (bit % (8 * sizeof(long))));
}

// Opens /dev/input/event* nodes we don't have open yet that have keys or relative axes.
inline void ScanInputs(std::map<std::string, InputDev> &devs) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent *e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + e->d_name;
        const auto known = devs.find(path);
        if (known != devs.end() && (known->second.fd >= 0 || known->second.ignored)) continue;
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ev[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !(HasBit(ev, EV_KEY) || HasBit(ev, EV_REL))) {
            close(fd);
            devs[path] = {-1, name, true};
            continue;
        }
        devs[path] = {fd, name, false};
        std::printf("watching %s (%s)\n", path.c_str(), name);
    }
    closedir(dir);
    // Forget nodes that went away, so a new device reusing the number gets opened.
    for (auto it = devs.begin(); it != devs.end();) {
        if (access(it->first.c_str(), F_OK) != 0) {
            if (it->second.fd >= 0) close(it->second.fd);
            it = devs.erase(it);
        } else {
            ++it;
        }
    }
    std::fflush(stdout);
}

// Reads everything waiting; returns the name of a device that sent a key or relative
// motion, or "".
inline std::string DrainInputs(std::map<std::string, InputDev> &devs) {
    std::string used;
    input_event evs[64];
    for (auto &[path, d] : devs) {
        if (d.fd < 0) continue;
        while (true) {
            const ssize_t n = read(d.fd, evs, sizeof evs);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN) {  // unplugged: the next scan forgets it or opens it again
                    close(d.fd);
                    d.fd = -1;
                }
                break;
            }
            for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i)
                if (evs[i].type == EV_KEY || evs[i].type == EV_REL) used = d.name;
        }
    }
    return used;
}

}  // namespace ft_power
