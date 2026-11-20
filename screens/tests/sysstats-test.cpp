// The palm card's readouts (sysstats.h): /proc/stat's busy share between two reads, /proc/meminfo's
// in use, which thermal zones count, the GPU's share of a frame, and the words the card shows.
#include "../sysstats.h"

#include <cmath>
#include <cstdio>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using namespace sysstats;

    Cpu a, b;
    check("a cpu line", ParseCpu("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3 4\n", &a) && a.total == 1000 && a.busy == 150);
    check("...later", ParseCpu("cpu  200 0 100 1500 100 0 0 0 0 0\n", &b) && b.total == 1900 && b.busy == 300);
    check("busy between them: 150 of 900", std::fabs(CpuPercent(a, b) - 100.0 * 150 / 900) < 1e-9);
    check("no time passed: unknown", CpuPercent(a, a) < 0);
    check("counters back (a restart): unknown", CpuPercent(b, a) < 0);
    Cpu c;
    check("cpu0's line isn't the total", !ParseCpu("cpu0 1 2 3 4\n", &c));
    check("too few numbers", !ParseCpu("cpu  1 2 3\n", &c));
    check("nothing", !ParseCpu(nullptr, &c) && !ParseCpu("", &c));

    double used = 0, total = 0;
    check("meminfo: total less available",
          ParseMem("MemTotal:       16000000 kB\nMemFree:         1000000 kB\nMemAvailable:    6000000 kB\n", &used, &total) &&
              std::fabs(total - 16000000 / 1048576.0) < 1e-9 && std::fabs(used - 10000000 / 1048576.0) < 1e-9);
    check("no MemAvailable: unknown", !ParseMem("MemTotal: 100 kB\nMemFree: 50 kB\n", &used, &total));
    check("more available than there is: unknown", !ParseMem("MemTotal: 100 kB\nMemAvailable: 200 kB\n", &used, &total));
    check("nothing", !ParseMem(nullptr, &used, &total) && !ParseMem("", &used, &total));

    check("the CPU's zones count", CpuZone("cpuss-0") && CpuZone("cpu0-top") && CpuZone("cpu7-bottom\n"));
    check("others don't", !CpuZone("gpuss-0") && !CpuZone("battery") && !CpuZone("") && !CpuZone(nullptr));

    check("GPU: 5.5 ms of a 90 Hz frame's 11.1", std::fabs(GpuPercent(5.5, 90) - 49.5) < 1e-9);
    check("...more than a frame: 100", GpuPercent(20, 90) == 100);
    check("...no rate or no timing: unknown", GpuPercent(5, 0) < 0 && GpuPercent(NAN, 90) < 0 && GpuPercent(-1, 90) < 0);

    const Words w = Say(43.6, 28.2, 10.71, 15.0, 66.4);
    check("the words", w.cpu == "CPU 44%" && w.gpu == "GPU 28%" && w.mem == "Mem 10.7/15 GB" && w.temp == "66\xc2\xb0" "C");
    const Words none = Say(-1, -1, 0, 0, -1000);
    check("unknown ones empty", none.cpu.empty() && none.gpu.empty() && none.mem.empty() && none.temp.empty());

    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
