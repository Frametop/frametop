// Core dumps off for a Steam game on gamescope's app display (gsforward.h; docs/design.md
// "Gamescope apps").
//
// A game that crashes under FEX dumps its whole address space, 6 to 7 GB, through
// systemd-coredump (SteamOS's developer mode turns core dumps on). That takes about a minute on a
// 16 GB headset, and within seconds of its start vrcompositor logs "WaitForPendingPresent: failed
// to wait for present" and dies of SIGXCPU, taking SteamVR and the whole session with it, which
// SteamOS counts as a failed SteamVR session. It did at each of Slay the Spire 2's five crashes
// that wrote a dump (2026-11-26 and 27); its four crashes with core dumps off left SteamVR alone.
// So the processes Steam launched the game under get a soft core limit of 1 (kNoDump). The hard
// limit stays, and processes they start later inherit it. What's lost with the dump is
// systemd-coredump's record of the crash: at 1 the kernel never starts the handler, so
// coredumpctl lists nothing for the game. The kernel's own lines in the journal (the signal, the
// thread's name, "aborting core") are what a crash leaves.
//
// 1, not 0: SteamOS pipes core dumps to a program (kernel.core_pattern), and for a pipe the kernel
// ignores the limit, 0 too, except 1, which it takes as "no dump" and ends the process at once
// (core(5); the kernel logs "RLIMIT_CORE is set to 1, aborting core"). At 0 it still pipes the
// whole dump to SteamOS's handler, which reads it to throw it away: the dead game's window stayed
// 39 to 43 s at each of four crashes (2026-11-27), frozen. A 1 GB test process was gone 1.5 to
// 1.7 s after its signal at 0, and 6 ms after it at 1. Where dumps go to a file, 1 byte is under
// the smallest one the kernel writes, so none either.
//
// Under is pure, tested with Off's I/O by screens/tests/gamecores-test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace gamecores {

constexpr size_t kMaxProcesses = 4096;   // (a bound on one game's processes)
constexpr uint64_t kNoDump = 1;          // the soft core limit that writes and pipes no dump

// `root` and every process under it, parents before their children, from each process's parent.
// None for no root, or init.
inline std::vector<uint32_t> Under(const std::map<uint32_t, uint32_t> &parents, uint32_t root) {
    std::vector<uint32_t> out;
    if (root <= 1) return out;
    std::map<uint32_t, std::vector<uint32_t>> children;
    for (const auto &[pid, parent] : parents)
        if (pid != root) children[parent].push_back(pid);
    out.push_back(root);
    for (size_t i = 0; i < out.size() && out.size() < kMaxProcesses; ++i)
        if (const auto c = children.find(out[i]); c != children.end())
            for (const uint32_t pid : c->second)
                if (out.size() < kMaxProcesses) out.push_back(pid);
    return out;
}

// The soft core limit of `root` and of every process under it to kNoDump (to 0 where the hard limit
// is 0), each hard limit kept; how many it changed (0: none to change, or no such process).
int Off(uint32_t root);

}  // namespace gamecores
