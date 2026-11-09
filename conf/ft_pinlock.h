/* Shared opt-in Steam PIN gate. No credential data, no persistent unlock token.
 * Missing, malformed, stale or pre-suspend evidence denies access. Same-user
 * processes are trusted; this is not an OS login or a sandbox boundary. */
#ifndef FT_PINLOCK_H
#define FT_PINLOCK_H
#include "ft_conf.h"
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FT_PIN_LEASE_MS 750

static inline int64_t ft_pin_ms(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline bool ft_pin_fresh(int64_t boot, int64_t mono, int64_t now_boot, int64_t now_mono) {
    if (boot < 0 || mono < 0 || now_boot < boot || now_mono < mono) return false;
    const int64_t b = now_boot - boot, m = now_mono - mono;
    return b < FT_PIN_LEASE_MS &&
           m < FT_PIN_LEASE_MS && llabs(b - m) < 50;
}

/* Setting is startup-only: changing it requires restarting its consumers. */
static inline bool ft_pin_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        char value[64];   /* (a longer value would read as unset: off, where the session says on) */
        enabled = ft_conf_setting("STEAM_PIN_LOCK", value, sizeof value) && strcmp(value, "0");
    }
    return enabled != 0;
}

static inline bool ft_pin_read(const char *path) {
    char buf[160];
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    const bool safe = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                      !(st.st_mode & 077) && st.st_size > 0 && st.st_size < (off_t)sizeof buf;
    const ssize_t n = safe ? read(fd, buf, sizeof buf - 1) : -1;
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    int allowed, pid, used = 0;
    long long boot, mono;
    if (sscanf(buf, "FTPIN1 %d %lld %lld %d\n%n", &allowed, &boot, &mono, &pid, &used) != 4 ||
        used != n || allowed != 1 || pid <= 1 || kill(pid, 0)) return false;
    return ft_pin_fresh(boot, mono, ft_pin_ms(CLOCK_BOOTTIME), ft_pin_ms(CLOCK_MONOTONIC));
}

/* Read at most once per 10 ms in each program: ft-screens asks per screen and per event,
 * many times a tick. (10 ms on top of the 750 ms lease.) */
static inline bool ft_pin_allowed(void) {
    if (!ft_pin_enabled()) return true;
    static int64_t read_ms = -1;
    static bool allowed = false;
    const int64_t now = ft_pin_ms(CLOCK_MONOTONIC);
    if (read_ms >= 0 && now >= read_ms && now - read_ms < 10) return allowed;
    char path[128];
    snprintf(path, sizeof path, "/run/user/%u/frametop-pin-state", (unsigned)getuid());
    allowed = ft_pin_read(path);
    read_ms = now;
    return allowed;
}
#endif
