// The palm menu's resource line (user, 2026-11-19: "add to the palm menu a resource tracker, memory,
// GPU, CPU, temperature"): what /proc/stat, /proc/meminfo and the thermal zones say, as the card shows it.
// Pure: the parsing and the words (screens/tests/sysstats-test.cpp); vrstats.cpp reads the files, and the
// GPU's share from SteamVR's frame timing (the Frame's GPU has no busy counter: msm's devfreq has no load).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sysstats {

// /proc/stat's "cpu" line: the jiffies spent busy, and all of them.
struct Cpu {
    uint64_t busy = 0, total = 0;
};

// The first line of /proc/stat's text ("cpu  user nice system idle iowait irq softirq steal ..."). Idle
// and iowait are idle; guest time is in user already. False if it isn't one.
inline bool ParseCpu(const char *text, Cpu *c) {
    if (!text || std::strncmp(text, "cpu ", 4) != 0) return false;
    const char *p = text + 4;
    uint64_t v[8] = {};
    int n = 0;
    for (; n < 8; ++n) {
        char *end;
        const unsigned long long x = std::strtoull(p, &end, 10);
        if (end == p) break;
        v[n] = x, p = end;
    }
    if (n < 4) return false;
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += v[i];
    c->total = total, c->busy = total - v[3] - v[4];
    return true;
}

// Busy between two reads, 0..100 (-1: no time passed, or the counters went back).
inline double CpuPercent(const Cpu &a, const Cpu &b) {
    if (b.total <= a.total || b.busy < a.busy) return -1;
    const double pct = 100.0 * double(b.busy - a.busy) / double(b.total - a.total);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// /proc/meminfo's text: in use (MemTotal less MemAvailable) and the total, in GiB. False without both.
inline bool ParseMem(const char *text, double *usedGb, double *totalGb) {
    if (!text) return false;
    long long total = -1, avail = -1;
    for (const char *line = text; line && *line; line = std::strchr(line, '\n'), line = line ? line + 1 : nullptr) {
        long long kb;
        if (std::sscanf(line, "MemTotal: %lld kB", &kb) == 1) total = kb;
        else if (std::sscanf(line, "MemAvailable: %lld kB", &kb) == 1) avail = kb;
    }
    if (total <= 0 || avail < 0 || avail > total) return false;
    *totalGb = total / 1048576.0, *usedGb = (total - avail) / 1048576.0;
    return true;
}

// A thermal zone whose temperature counts: a CPU core's (its type "cpu0-thermal", "cpu3-bottom-thermal"; not
// the cluster's "cpuss0-thermal").
inline bool CoreZone(const char *type) {
    return type && std::strncmp(type, "cpu", 3) == 0 && type[3] >= '0' && type[3] <= '9';
}

// The CPU's temperature from its cores' sensors: their average, -1000 with none. Not the hottest (user,
// 2026-11-24: "we might be using a deceptive temp sensor, the headset doesn't appear to be throttling but
// is showing high 90"): that's one spot of one core, 10 C up or down within a second, and the chip's own
// limiter holds it at 95 for next to no speed while the other cores are near 80.
inline double CpuTemp(const double *cores, int n) {
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += cores[i];
    return n > 0 ? sum / n : -1000;
}

// The card's four readouts (empty: not known yet, shown as a dash).
struct Words {
    std::string cpu, gpu, mem, temp;
};
inline Words Say(double cpuPct, double gpuPct, double memUsedGb, double memTotalGb, double tempC) {
    Words w;
    char b[32];
    if (cpuPct >= 0) std::snprintf(b, sizeof b, "CPU %.0f%%", cpuPct), w.cpu = b;
    if (gpuPct >= 0) std::snprintf(b, sizeof b, "GPU %.0f%%", gpuPct), w.gpu = b;
    if (memTotalGb > 0) std::snprintf(b, sizeof b, "Mem %.1f/%.0f GB", memUsedGb, memTotalGb), w.mem = b;
    if (tempC > -100) std::snprintf(b, sizeof b, "%.0f\xc2\xb0" "C", tempC), w.temp = b;
    return w;
}

// The GPU's share of each frame: its render time against the frame's (SteamVR's timing), 0..100, -1 without.
inline double GpuPercent(double gpuMs, double displayHz) {
    if (!(gpuMs >= 0) || !(displayHz > 0)) return -1;
    const double pct = gpuMs * displayHz / 10.0;   // (gpuMs / (1000 / hz) * 100)
    return pct > 100 ? 100 : pct;
}

}  // namespace sysstats
