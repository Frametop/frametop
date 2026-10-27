#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
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
        && !strcmp(path, real) && vrserver()) return frame_real_dlopen(proxy, flags);
    return frame_real_dlopen(path, flags);
}
