// ft-screens' log file (--log PATH; the session's is ~/.local/state/frametop/screens.log).
//
// It was /tmp/frametop-screens.log: in RAM on the Frame, and emptied at every desktop start, so
// the lines about a screen jump (DEE-206's trackwatch) were gone after a restart or a reboot. Now
// it's kept and appended to, and rotated by size instead: past FT_LOG_CAP bytes it becomes
// PATH.1 (the one before PATH.2, and so on to FT_LOG_KEEP), so the log takes at most about
// (FT_LOG_KEEP + 1) x FT_LOG_CAP. Checked at start and every few seconds (ft_log_check).
//
// Header only, plain POSIX: screens/tests/logfile-test.c runs it on a temporary folder.
#pragma once

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define FT_LOG_CAP (8L * 1024 * 1024)
#define FT_LOG_KEEP 2

// PATH past `cap` bytes: PATH.(keep-1) -> PATH.keep, ..., PATH -> PATH.1 (keep >= 1). 1 if it rotated.
static inline int ft_log_rotate(const char *path, long cap, int keep) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= cap) return 0;
    char from[4096], to[4096];
    for (int i = keep - 1; i >= 1; --i) {
        snprintf(from, sizeof from, "%s.%d", path, i);
        snprintf(to, sizeof to, "%s.%d", path, i + 1);
        rename(from, to);  // (missing: nothing to move)
    }
    snprintf(to, sizeof to, "%s.1", path);
    return rename(path, to) == 0;
}

// stdout and stderr to PATH, appended to (what's buffered goes to the old file first). 0 if done.
static inline int ft_log_open(const char *path) {
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    fflush(stdout), fflush(stderr);
    const int ok = dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0;
    close(fd);
    return ok ? 0 : -1;
}

// Rotate PATH once it's past `cap`, and go on in a new one. Cheap: one stat.
static inline void ft_log_check(const char *path, long cap, int keep) {
    if (path && ft_log_rotate(path, cap, keep)) ft_log_open(path);
}
