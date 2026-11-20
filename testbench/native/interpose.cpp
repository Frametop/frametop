#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

namespace {
using Open = void *(*)(const char *, int);
Open original() {
    // We interpose dlopen only, never dlsym. Resolving RTLD_NEXT therefore has
    // no factory/dlsym recursion and works on both glibc x86_64 and AArch64.
    static Open fn = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "dlopen"));
    return fn;
}
bool vrserver() {
    char exe[PATH_MAX]; const auto n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return false;
    exe[n] = 0;
    const auto *base = strrchr(exe, '/');
    return base && !strcmp(base + 1, "vrserver");
}
// SteamOS's health check (/usr/share/deckard/steamvr-health-check) appends "frog:glasses:"
// here for every failed SteamVR session and clears it after a good one. It repairs Steam
// after three and boots the other OS slot after four, so after a failure SteamVR starts
// once without the proxy: a proxy that crashes vrserver costs one failed session, not four.
// The boot race's failure (every boot's first start: HmdNotFound) doesn't count:
// steamvr-forgive keeps how many of the tracker's failures that is in frame-testbench-forgiven.
int read_file(const char *dir, const char *name, char *text, int size) {
    char path[PATH_MAX];
    if (dir && dir[0] == '/') snprintf(path, sizeof(path), "%s/%s", dir, name);
    else snprintf(path, sizeof(path), "/run/user/%u/%s", getuid(), name);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const auto n = read(fd, text, size - 1);
    close(fd);
    text[n > 0 ? n : 0] = 0;
    return n > 0 ? int(n) : 0;
}
bool failure_counted() {
    const char *dir = std::getenv("XDG_RUNTIME_DIR");
    char text[128];
    int frogs = 0;
    if (read_file(dir, "steamvr-short-session-tracker", text, sizeof(text)))
        for (const char *p = text; (p = strstr(p, "frog")); p += 4) ++frogs;
    char num[16];
    const int forgiven = read_file(dir, "frame-testbench-forgiven", num, sizeof(num)) ? atoi(num) : 0;
    return frogs > (forgiven >= 0 && forgiven <= frogs ? forgiven : 0);
}
}
extern "C" __attribute__((visibility("default")))
void *frame_real_dlopen(const char *path, int flags) {
    const auto fn = original();
    return fn ? fn(path, flags) : nullptr;
}
extern "C" __attribute__((visibility("default")))
void *dlopen(const char *path, int flags) {
    const char *real = std::getenv("FRAME_TESTBENCH_REAL_DRIVER");
    if (!real) real = "/opt/steamvr/drivers/cv/bin/linuxarm64/driver_cv.so";
    const char *proxy = std::getenv("FRAME_TESTBENCH_PROXY");
    if (path && real[0] == '/' && proxy && proxy[0] == '/' && strcmp(real, proxy)
        && !strcmp(path, real) && vrserver()) {
        if (!failure_counted()) return frame_real_dlopen(proxy, flags);
        fputs("frame-testbench: SteamOS counts a failed SteamVR session, so this one runs without the testbench\n", stderr);
    }
    return frame_real_dlopen(path, flags);
}
