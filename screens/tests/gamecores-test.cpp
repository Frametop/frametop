// gamecores.h: the processes under a game's launcher, parents first, and their soft core limit
// set to kNoDump with each hard limit kept.
//
//   g++ -std=c++17 -Iscreens screens/tests/gamecores-test.cpp screens/gamecores.cpp
#include "gamecores.h"

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>

using namespace gamecores;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

static rlim_t Soft(pid_t pid) {
    rlimit now;
    return prlimit(pid, RLIMIT_CORE, nullptr, &now) == 0 ? now.rlim_cur : rlim_t(-2);
}

// A process with a soft core limit of `soft`, and a child of its own that inherits it: both wait to
// be killed. Their pids, or false.
static bool Family(rlim_t soft, pid_t *parent, pid_t *child) {
    int up[2];
    if (pipe(up) != 0) return false;
    *parent = fork();
    if (*parent < 0) return false;
    if (*parent == 0) {
        close(up[0]);
        rlimit lim;
        getrlimit(RLIMIT_CORE, &lim);
        lim.rlim_cur = soft;
        setrlimit(RLIMIT_CORE, &lim);
        pid_t c = fork();
        if (c == 0) {
            close(up[1]);
            for (;;) pause();
        }
        if (write(up[1], &c, sizeof c) != sizeof c) _exit(1);
        close(up[1]);
        for (;;) pause();
    }
    close(up[1]);
    const bool ok = read(up[0], child, sizeof *child) == sizeof *child && *child > 0;
    close(up[0]);
    return ok;
}

int main() {
    // the launcher and all under it, parents before their children; not its siblings or parents
    {
        const std::map<uint32_t, uint32_t> parents = {{1, 0},  {10, 1},  {20, 10}, {21, 10}, {30, 20},
                                                      {31, 30}, {40, 1}, {41, 40}, {50, 21}};
        CHECK((Under(parents, 10) == std::vector<uint32_t>{10, 20, 21, 30, 50, 31}));
        CHECK((Under(parents, 30) == std::vector<uint32_t>{30, 31}));
        CHECK((Under(parents, 31) == std::vector<uint32_t>{31}));
        CHECK((Under(parents, 99) == std::vector<uint32_t>{99}));   // (gone since: Off finds none to change)
        CHECK(Under(parents, 0).empty() && Under(parents, 1).empty());   // (never everything under init)
    }
    // parents that name each other end, each listed once
    {
        const std::map<uint32_t, uint32_t> loop = {{10, 11}, {11, 10}, {12, 11}};
        CHECK((Under(loop, 10) == std::vector<uint32_t>{10, 11, 12}));
    }
    // no more than kMaxProcesses
    {
        std::map<uint32_t, uint32_t> many;
        for (uint32_t pid = 3; pid < 3 + 2 * kMaxProcesses; ++pid) many[pid] = 2;
        CHECK(Under(many, 2).size() == kMaxProcesses);
    }

    // real processes: both get a soft limit of kNoDump and keep their hard limit; this one keeps its own
    rlimit mine;
    getrlimit(RLIMIT_CORE, &mine);
    if (mine.rlim_max == 0) {
        std::printf("gamecores-test: no core limit to lower here (the hard limit is 0): Off not run\n");
    } else {
        const rlim_t soft = mine.rlim_max == RLIM_INFINITY || mine.rlim_max > 4096 ? 4096 : mine.rlim_max;
        pid_t parent = 0, child = 0;
        if (!Family(soft, &parent, &child)) {
            std::printf("FAIL gamecores-test: couldn't start the processes to limit\n"), ++failures;
        } else {
            CHECK(Soft(parent) == soft && Soft(child) == soft);
            CHECK(Off(uint32_t(parent)) == 2);
            rlimit p, c;
            CHECK(prlimit(parent, RLIMIT_CORE, nullptr, &p) == 0 && p.rlim_cur == kNoDump && p.rlim_max == mine.rlim_max);
            CHECK(prlimit(child, RLIMIT_CORE, nullptr, &c) == 0 && c.rlim_cur == kNoDump && c.rlim_max == mine.rlim_max);
            CHECK(Off(uint32_t(parent)) == 0);   // (none left to change)
            CHECK(Soft(getpid()) == mine.rlim_cur);
        }
        if (child > 0) kill(child, SIGKILL);
        if (parent > 0) kill(parent, SIGKILL), waitpid(parent, nullptr, 0);
        // only the child, not its parent
        if (Family(soft, &parent, &child)) {
            CHECK(Off(uint32_t(child)) == 1 && Soft(child) == kNoDump && Soft(parent) == soft);
            kill(child, SIGKILL), kill(parent, SIGKILL), waitpid(parent, nullptr, 0);
        }
        // a limit of 0 still pipes the dump: to kNoDump too
        if (Family(0, &parent, &child)) {
            CHECK(Off(uint32_t(parent)) == 2 && Soft(parent) == kNoDump && Soft(child) == kNoDump);
            kill(child, SIGKILL), kill(parent, SIGKILL), waitpid(parent, nullptr, 0);
        }
    }
    CHECK(Off(0) == 0 && Off(1) == 0);

    if (failures) return std::printf("gamecores-test: %d failed\n", failures), 1;
    std::printf("gamecores-test: ok\n");
    return 0;
}
