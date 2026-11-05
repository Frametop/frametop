// ft-powerd: turns the headset's displays off while nobody is using it (OpenVR background
// client, runs in the dev container as frametop-power.service).
//
// SteamVR turns the displays off 5 s after the proximity sensor says the headset came off
// (power.turnOffScreensTimeout). With something in front of the sensor, like a display
// mount, that never happens: the displays stay on all night, and Steam, which then thinks
// someone is wearing the headset, never puts it to sleep either. ft-powerd goes by use
// instead of the sensor. After DISPLAY_OFF_MIN minutes in which neither the headset nor a
// controller moved and no input device sent a key, button, or motion, it turns the
// backlight off, and it turns it back on at the next movement or input.
//
// Moving: more than DISPLAY_MOVE_MM (5 mm) or DISPLAY_MOVE_DEG (0.5 degrees) within 10 s.
// On a mount the headset's pose jitters by about 0.5 mm and 0.1 degrees, and its position
// drifts about 1 mm in two minutes (tracking), so a fixed reference would count the drift
// as moving sooner or later; a 10 s window never does, and anyone wearing the headset
// moves that much in 10 s now and then. The headset, the Frame controllers, trackers, and
// the 3D mouse's virtual controller count; the last moves only when the mouse or the gaze
// moves it.
// Input: every /dev/input device with keys or relative axes (mice, keyboards, the headset's
// buttons, the relay's virtual devices), read without grabbing and rescanned every few
// seconds for new ones. Keyboards the relay grabs for the desktop aren't seen, but typing
// in the headset moves it anyway.
//
// The displays: /sys/class/backlight/ae94000.dsi.0/brightness, the file SteamVR's own driver
// writes for standby ("cv: Set displays off" in vrserver.txt); 0 turns both panels'
// backlights off. The value from before goes back when they wake. The GPU keeps rendering
// and tracking keeps running, which is why they can wake the moment the headset moves, but
// it also means this saves the panels' power and light, not the whole headset's.
// If something else turns the backlight back on while it's off (SteamVR leaving standby,
// the brightness setting), that counts as use. While SteamVR has the headset in standby
// (taken off), SteamVR owns the displays and ft-powerd waits. The value to put back is
// also kept in ~/.cache/frametop/powerd-brightness until the displays wake, so a crash or
// a stop while they're off doesn't leave them dark: the next start puts it back.
//
// Control (datagrams on @ft_powerd; the reply goes to the sender's address):
//   status -> "ok on|off|away <seconds since use> <timeout seconds, 0 = never>"
//             (away: SteamVR has the headset in standby)
//   off    -> "ok": the displays off now, to try it (for 2 s nothing counts as use)
//   on     -> "ok": back on
// Settings (~/.config/frametop.conf, re-read when the file changes): DISPLAY_OFF_MIN (0 =
// never, the default), DISPLAY_MOVE_MM (5), DISPLAY_MOVE_DEG (0.5).
#include <openvr.h>

#include "../conf/ftsock.h"
#include "../conf/ft_conf.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

const char *kBacklight = "/sys/class/backlight/ae94000.dsi.0/brightness";

std::string Home() {
    const char *home = std::getenv("HOME");
    return home ? home : "";
}

std::string ConfPath() { return Home() + "/.config/frametop.conf"; }
std::string SavedPath() { return Home() + "/.cache/frametop/powerd-brightness"; }

std::map<std::string, std::string> ReadConfig() { return ft_conf::Read(ConfPath()); }

double ConfDouble(const std::map<std::string, std::string> &c, const char *key, double fallback) {
    return ft_conf::Double(c, key, fallback);
}

int ReadInt(const std::string &path, int fallback) {
    std::ifstream in(path);
    int v;
    return in >> v ? v : fallback;
}

bool WriteInt(const std::string &path, int v) {
    std::ofstream out(path);
    out << v << "\n";
    out.flush();
    return bool(out);
}

// --- input devices ---------------------------------------------------------------------

struct InputDev {
    int fd;  // -1: not open (no keys or relative axes, or it stopped reading)
    std::string name;
    bool ignored;  // no keys or relative axes: not opened again
};

bool HasBit(const unsigned long *bits, int bit) {
    return bits[bit / (8 * sizeof(long))] & (1UL << (bit % (8 * sizeof(long))));
}

// Opens /dev/input/event* nodes we don't have open yet that have keys or relative axes.
void ScanInputs(std::map<std::string, InputDev> &devs) {
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
std::string DrainInputs(std::map<std::string, InputDev> &devs) {
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

// --- poses ---------------------------------------------------------------------------------

// Where a device was at the start of the current window.
struct Anchor {
    bool set = false;
    float m[3][4];
    std::chrono::steady_clock::time_point at;
};
constexpr auto kMoveWindow = std::chrono::seconds(10);

// How far a pose is from an anchor: metres and degrees.
void Distance(const Anchor &a, const vr::HmdMatrix34_t &p, double &metres, double &degrees) {
    const double dx = p.m[0][3] - a.m[0][3], dy = p.m[1][3] - a.m[1][3], dz = p.m[2][3] - a.m[2][3];
    metres = std::sqrt(dx * dx + dy * dy + dz * dz);
    double trace = 0;  // trace(A^T P): the angle of the rotation between them
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) trace += a.m[r][c] * p.m[r][c];
    degrees = std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0)) * 180 / M_PI;
}

void SetAnchor(Anchor &a, const vr::HmdMatrix34_t &p, std::chrono::steady_clock::time_point now) {
    std::memcpy(a.m, p.m, sizeof a.m);
    a.set = true;
    a.at = now;
}

// --- the displays ----------------------------------------------------------------------

volatile std::sig_atomic_t stopSignal = 0;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int s : {SIGTERM, SIGINT, SIGHUP}) std::signal(s, [](int sig) { stopSignal = sig; });

    // Put back a value left behind by a run that ended with the displays off.
    const int saved = ReadInt(SavedPath(), -1);
    if (saved > 0) {
        if (ReadInt(kBacklight, -1) == 0) {
            WriteInt(kBacklight, saved);
            std::printf("displays back on (brightness %d, left off by an earlier run)\n", saved);
        }
        std::remove(SavedPath().c_str());
    }

    const int ctl = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    {
        sockaddr_un addr;
        if (bind(ctl, reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(FT_SOCK_POWERD, &addr)) != 0) {
            std::perror("bind @ft_powerd (already running?)");
            return 1;
        }
    }

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::IVRSystem *sys = vr::VRSystem();

    double offAfter = 0, moveMetres = 0.005, moveDegrees = 0.5;
    timespec confTime{};
    auto loadConfig = [&](bool force) {
        struct stat st{};
        const bool exists = stat(ConfPath().c_str(), &st) == 0;
        if (!force && exists && st.st_mtim.tv_sec == confTime.tv_sec && st.st_mtim.tv_nsec == confTime.tv_nsec) return;
        confTime = exists ? st.st_mtim : timespec{};
        const auto conf = ReadConfig();
        const double was = offAfter;
        offAfter = std::max(0.0, ConfDouble(conf, "DISPLAY_OFF_MIN", 0)) * 60;
        moveMetres = std::max(0.001, ConfDouble(conf, "DISPLAY_MOVE_MM", 5) / 1000);
        moveDegrees = std::max(0.05, ConfDouble(conf, "DISPLAY_MOVE_DEG", 0.5));
        if (force || was != offAfter)
            std::printf("displays off after %s; moving means %.1f mm or %.2f deg\n",
                        offAfter > 0 ? (std::to_string(int(offAfter)) + " s without use").c_str() : "never (DISPLAY_OFF_MIN=0)",
                        moveMetres * 1000, moveDegrees);
    };
    loadConfig(true);

    std::map<std::string, InputDev> inputs;
    ScanInputs(inputs);

    Anchor anchors[vr::k_unMaxTrackedDeviceCount];
    vr::ETrackedDeviceClass classes[vr::k_unMaxTrackedDeviceCount] = {};
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];

    auto now = Clock::now();
    auto lastUse = now, lastScan = now, lastClasses = now - std::chrono::hours(1), lastConf = now;
    auto graceUntil = now;  // after "off", nothing counts as use until then
    auto awaySince = now;  // when the headset's activity level last agreed with `away`
    bool off = false, away = false;
    int restore = 0;  // the brightness to put back

    auto turnOff = [&](const char *why) {
        const int b = ReadInt(kBacklight, -1);
        if (b <= 0) return;  // already dark (SteamVR standby) or unreadable
        mkdir((Home() + "/.cache").c_str(), 0755);
        mkdir((Home() + "/.cache/frametop").c_str(), 0755);
        WriteInt(SavedPath(), b);
        if (!WriteInt(kBacklight, 0)) {
            std::printf("couldn't write %s\n", kBacklight);
            std::remove(SavedPath().c_str());
            return;
        }
        restore = b;
        off = true;
        std::printf("displays off: %s (brightness was %d)\n", why, b);
    };
    auto turnOn = [&](const std::string &why) {
        if (!off) return;
        off = false;
        if (ReadInt(kBacklight, -1) == 0) WriteInt(kBacklight, restore);
        std::remove(SavedPath().c_str());
        std::printf("displays on: %s\n", why.c_str());
    };

    // SteamVR is asked every 100 ms (events, the activity level, poses: IPC calls to
    // vrserver). Input wakes the loop too, to wake the displays at once, but a moving mouse
    // sends hundreds of events a second, so those wakes only drain the devices.
    constexpr auto kTick = std::chrono::milliseconds(100);
    auto lastVr = now - kTick;
    std::vector<pollfd> fds;
    while (!stopSignal) {
        fds.clear();
        fds.push_back({ctl, POLLIN, 0});
        for (auto &[path, d] : inputs)
            if (d.fd >= 0) fds.push_back({d.fd, POLLIN, 0});
        const auto wait = std::chrono::ceil<std::chrono::milliseconds>(kTick - (Clock::now() - lastVr));
        poll(fds.data(), fds.size(), std::clamp<int>(wait.count(), 0, kTick.count()));
        now = Clock::now();
        const bool grace = now < graceUntil;
        std::string use;  // why this tick counts as use

        const std::string dev = DrainInputs(inputs);
        if (!dev.empty() && !grace) use = "input from " + dev;
        if (now - lastScan > std::chrono::seconds(3)) {
            ScanInputs(inputs);
            lastScan = now;
        }
        if (now - lastConf > std::chrono::seconds(2)) {
            loadConfig(false);
            lastConf = now;
        }

        if (now - lastVr >= kTick) {
            lastVr = now;
            vr::VREvent_t ev;
            while (sys->PollNextEvent(&ev, sizeof ev)) {
                if (ev.eventType == vr::VREvent_Quit) {
                    stopSignal = SIGTERM;
                    sys->AcknowledgeQuit_Exiting();
                } else if (ev.eventType == vr::VREvent_TrackedDeviceActivated ||
                           ev.eventType == vr::VREvent_TrackedDeviceDeactivated) {
                    lastClasses = now - std::chrono::hours(1);
                }
            }
            if (now - lastClasses > std::chrono::seconds(5)) {
                for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
                    classes[i] = sys->GetTrackedDeviceClass(i);
                lastClasses = now;
            }

            // SteamVR's standby (headset taken off) turns the displays off itself; putting the
            // headset on again (or the mount covering the sensor again) counts as use.
            // A change counts once it has held for a second (the first reading after connecting
            // is Idle).
            const auto level = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            const bool awayNow = level == vr::k_EDeviceActivityLevel_Idle || level == vr::k_EDeviceActivityLevel_Standby ||
                                 level == vr::k_EDeviceActivityLevel_Idle_Timeout;
            if (awayNow == away) awaySince = now;
            if (awayNow != away && now - awaySince >= std::chrono::seconds(1)) {
                away = awayNow;
                std::printf("SteamVR: headset %s\n", away ? "off (SteamVR's standby has the displays)" : "on");
                if (!away) use = "headset on again";
            }

            // Raw poses: recentering or a new play area doesn't move anything.
            sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0, poses, vr::k_unMaxTrackedDeviceCount);
            for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                const auto c = classes[i];
                if (c != vr::TrackedDeviceClass_HMD && c != vr::TrackedDeviceClass_Controller &&
                    c != vr::TrackedDeviceClass_GenericTracker)
                    continue;
                const auto &p = poses[i];
                if (!p.bPoseIsValid || p.eTrackingResult != vr::TrackingResult_Running_OK) {
                    anchors[i].set = false;  // tracking back or a controller turned on: start over
                    continue;
                }
                if (!anchors[i].set || grace || now - anchors[i].at > kMoveWindow) {
                    SetAnchor(anchors[i], p.mDeviceToAbsoluteTracking, now);
                    continue;
                }
                double metres, degrees;
                Distance(anchors[i], p.mDeviceToAbsoluteTracking, metres, degrees);
                if (metres > moveMetres || degrees > moveDegrees) {
                    SetAnchor(anchors[i], p.mDeviceToAbsoluteTracking, now);
                    if (use.empty()) {
                        char why[96];
                        std::snprintf(why, sizeof why, "%s %u moved %.1f mm, %.2f deg",
                                      c == vr::TrackedDeviceClass_HMD ? "headset" : "device", i, metres * 1000, degrees);
                        use = why;
                    }
                }
            }

            if (off && ReadInt(kBacklight, -1) > 0) {
                // Something else lit them (SteamVR leaving standby, the brightness setting).
                off = false;
                std::remove(SavedPath().c_str());
                use = "turned on elsewhere";
                std::printf("displays on: turned on elsewhere\n");
            }
        }
        if (!use.empty()) {
            lastUse = now;
            turnOn(use);
        }
        if (!off && !away && offAfter > 0 && now - lastUse >= std::chrono::duration<double>(offAfter)) {
            char why[64];
            if (offAfter >= 60) std::snprintf(why, sizeof why, "%g min without use", offAfter / 60);
            else std::snprintf(why, sizeof why, "%.0f s without use", offAfter);
            turnOff(why);
            if (!off) lastUse = now;  // couldn't: try again after another timeout, not every tick
        }

        // Control socket.
        char buf[256];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(ctl, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            std::string cmd(buf), reply = "ok";
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == ' ')) cmd.pop_back();
            if (cmd == "status") {
                char s[96];
                std::snprintf(s, sizeof s, "ok %s %.0f %.0f", off ? "off" : away ? "away" : "on",
                              std::chrono::duration<double>(now - lastUse).count(), offAfter);
                reply = s;
            } else if (cmd == "off") {
                graceUntil = now + std::chrono::seconds(2);
                turnOff("asked to");
                if (!off) reply = "error the displays are already off, or the backlight can't be written";
            } else if (cmd == "on") {
                lastUse = now;
                turnOn("asked to");
            } else {
                reply = "error unknown command: " + cmd;
            }
            if (fromLen > offsetof(sockaddr_un, sun_path))
                sendto(ctl, reply.data(), reply.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
    }

    turnOn("stopping");
    std::printf("stopped (signal %d)\n", int(stopSignal));
    vr::VR_Shutdown();
    return 0;
}
