// gamecores.h's I/O: the processes under a game's launcher (/proc), and their core limits.
#include "gamecores.h"

#include <dirent.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gamecores {

namespace {
// Each process's parent, from /proc/*/stat.
std::map<uint32_t, uint32_t> Parents() {
    std::map<uint32_t, uint32_t> parents;
    DIR *proc = opendir("/proc");
    if (!proc) return parents;
    while (const dirent *e = readdir(proc)) {
        char *end = nullptr;
        const unsigned long pid = std::strtoul(e->d_name, &end, 10);
        if (!pid || *end) continue;
        std::FILE *f = std::fopen(("/proc/" + std::string(e->d_name) + "/stat").c_str(), "r");
        if (!f) continue;
        char buf[512];
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[n] = 0;
        // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so after its last ')'
        const char *close = std::strrchr(buf, ')');
        char state = 0;
        unsigned ppid = 0;
        if (close && std::sscanf(close + 1, " %c %u", &state, &ppid) == 2) parents[uint32_t(pid)] = ppid;
    }
    closedir(proc);
    return parents;
}

// The soft core limit of each of `pids` that has one to 0; how many it changed.
int OffEach(const std::vector<uint32_t> &pids) {
    int changed = 0;
    for (const uint32_t p : pids) {
        const pid_t pid = pid_t(p);
        rlimit now;
        if (prlimit(pid, RLIMIT_CORE, nullptr, &now) != 0 || now.rlim_cur == 0) continue;
        const rlimit off = {0, now.rlim_max};
        if (prlimit(pid, RLIMIT_CORE, &off, nullptr) == 0) ++changed;
    }
    return changed;
}
}  // namespace

int Off(uint32_t root) {
    // Parents first, so a process one starts after its turn inherits the limit. One started just
    // before its parent's turn isn't in that list: read again until a pass changes none.
    int changed = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const int n = OffEach(Under(Parents(), root));
        changed += n;
        if (!n) break;
    }
    return changed;
}

}  // namespace gamecores
