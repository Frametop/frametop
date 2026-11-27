// Core dumps off for a Steam game on gamescope's app display (gsforward.h; docs/design.md
// "Gamescope apps").
//
// A game that crashes under FEX dumps its whole address space, 6 to 7 GB, through
// systemd-coredump (SteamOS's developer mode turns core dumps on). That takes about a minute on a
// 16 GB headset, and within seconds of its start vrcompositor logs "WaitForPendingPresent: failed
// to wait for present" and dies of SIGXCPU, taking SteamVR and the whole session with it, which
// SteamOS counts as a failed SteamVR session. It did at each of Slay the Spire 2's five crashes
// that wrote a dump (2026-11-26 and 27); its two crashes with core dumps off left SteamVR alone.
// So the processes Steam launched the game under get a soft core limit of 0: a crashing game ends
// at once. The hard limit stays, so nothing is lost but the dump, and processes they start later
// inherit it.
//
// Under is pure, tested with Off's I/O by screens/tests/gamecores-test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace gamecores {

constexpr size_t kMaxProcesses = 4096;   // (a bound on one game's processes)

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

// The soft core limit of `root` and of every process under it to 0, each hard limit kept; how many
// it changed (0: none to change, or no such process).
int Off(uint32_t root);

}  // namespace gamecores
