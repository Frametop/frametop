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
bool failure_counted() {
    char path[PATH_MAX];
    const char *dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0] == '/') snprintf(path, sizeof(path), "%s/steamvr-short-session-tracker", dir);
    else snprintf(path, sizeof(path), "/run/user/%u/steamvr-short-session-tracker", getuid());
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char text[64];
    const auto n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) return false;
    text[n] = 0;
    return strstr(text, "frog");
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
