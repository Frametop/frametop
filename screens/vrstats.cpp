// The palm card's readouts (sysstats.h): read at most once a second, and only while the card asks
// (it shows). CPU from /proc/stat, memory from /proc/meminfo, the CPU cores' thermal zones, the battery and
// charger (battery.h) from /sys/class/power_supply, and the GPU's share of each frame from SteamVR's frame
// timing (the last kFrames frames' render time, compositor and app, against the display's frame: the
// Frame's GPU has no busy counter to read).
#include "vrpanel.h"
#include "sysstats.h"

#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ftvr {
namespace {

constexpr int64_t kEveryMs = 1000;
constexpr uint32_t kFrames = 30;

struct {
    int64_t readMs = -1;
    sysstats::Cpu cpu;
    bool haveCpu = false;
    std::vector<std::string> zones;   // the CPU's thermal zones' temp files
    bool zonesFound = false;
    sysstats::Words words;
    battery::Reading battery;
} g_stats;

// A small file's text (up to `n` bytes), or empty.
std::string Slurp(const char *path, size_t n) {
    std::string out(n, '\0');
    FILE *f = std::fopen(path, "re");
    if (!f) return {};
    out.resize(std::fread(&out[0], 1, n, f));
    std::fclose(f);
    return out;
}

void FindZones() {
    g_stats.zonesFound = true;
    char path[96];
    for (int i = 0; i < 256; ++i) {
        std::snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        const std::string type = Slurp(path, 64);
        if (type.empty()) {
            if (i > 0) break;   // (zones are numbered from 0 with no holes)
            continue;
        }
        if (!sysstats::CoreZone(type.c_str())) continue;
        std::snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        g_stats.zones.push_back(path);
    }
}

double CpuC() {
    if (!g_stats.zonesFound) FindZones();
    std::vector<double> cores;
    for (const std::string &z : g_stats.zones) {
        const std::string t = Slurp(z.c_str(), 16);
        if (!t.empty()) cores.push_back(std::atof(t.c_str()) / 1000.0);
    }
    return sysstats::CpuTemp(cores.data(), int(cores.size()));
}

// The headset's battery and whether any charger of its own is online, each time from the folder: its
// supplies' names are the drivers'.
battery::Reading ReadBattery() {
    const std::string root = "/sys/class/power_supply/";
    battery::Reading r;
    DIR *d = opendir(root.c_str());
    if (!d) return r;
    while (const dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string dir = root + e->d_name + "/";
        auto text = [&](const char *f) { return Slurp((dir + f).c_str(), 32); };
        if (!battery::Headset(text("scope").c_str())) continue;
        if (!battery::IsBattery(text("type").c_str())) {
            r.plugged |= std::atoi(text("online").c_str()) == 1;
            continue;
        }
        const std::string pct = text("capacity"), uv = text("voltage_now");
        std::string ua = text("current_avg");
        if (ua.empty()) ua = text("current_now");
        if (!pct.empty()) r.percent = std::atoi(pct.c_str());
        if (!uv.empty() && !ua.empty()) r.watts = battery::Watts(std::atof(uv.c_str()), std::atof(ua.c_str()));
        r.full = text("status").compare(0, 4, "Full") == 0;
    }
    closedir(d);
    return r;
}

double GpuNow() {
    if (!vr::VRCompositor() || !vr::VRSystem()) return -1;
    vr::Compositor_FrameTiming t[kFrames] = {};
    for (auto &f : t) f.m_nSize = sizeof f;
    const uint32_t n = vr::VRCompositor()->GetFrameTimings(t, kFrames);
    if (!n) return -1;
    double ms = 0;
    for (uint32_t i = 0; i < n; ++i) ms += t[i].m_flTotalRenderGpuMs;
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err);
    return err == vr::TrackedProp_Success ? sysstats::GpuPercent(ms / n, hz) : -1;
}

}  // namespace

const sysstats::Words &SystemStats(int64_t nowMs) {
    if (g_stats.readMs >= 0 && nowMs - g_stats.readMs < kEveryMs && nowMs >= g_stats.readMs) return g_stats.words;
    g_stats.readMs = nowMs;
    sysstats::Cpu cpu;
    double cpuPct = -1;
    if (sysstats::ParseCpu(Slurp("/proc/stat", 256).c_str(), &cpu)) {
        if (g_stats.haveCpu) cpuPct = sysstats::CpuPercent(g_stats.cpu, cpu);
        g_stats.cpu = cpu, g_stats.haveCpu = true;
    }
    double used = 0, total = 0;
    if (!sysstats::ParseMem(Slurp("/proc/meminfo", 4096).c_str(), &used, &total)) total = 0;
    sysstats::Words w = sysstats::Say(cpuPct, GpuNow(), used, total, CpuC());
    if (w.cpu.empty()) w.cpu = g_stats.words.cpu;   // (the first read has nothing to compare with)
    g_stats.words = w;
    g_stats.battery = ReadBattery();
    return g_stats.words;
}

const battery::Reading &BatteryNow(int64_t nowMs) {
    SystemStats(nowMs);
    return g_stats.battery;
}

void ForgetStats() { g_stats.readMs = -1, g_stats.haveCpu = false; }

}  // namespace ftvr
