#define _GNU_SOURCE
#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

int ft_child_spawn(char *const argv[], const char *log, pid_t *pid) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    if (sv[1] == 3) {  // dup2 onto itself would keep close-on-exec
        const int moved = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
        close(sv[1]);
        sv[1] = moved;
    }
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_addopen(&io, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&io, 1, log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    posix_spawn_file_actions_adddup2(&io, 1, 2);
    posix_spawn_file_actions_adddup2(&io, sv[1], 3);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    const int rc = posix_spawn(pid, argv[0], &io, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&io);
    close(sv[1]);
    if (rc != 0) {
        close(sv[0]);
        errno = rc;
        return -1;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    return sv[0];
}

bool ft_child_path(const char *env, const char *folder, const char *name, char *out, size_t size) {
    const char *named = getenv(env);
    if (named && *named) return snprintf(out, size, "%s", named) < (int)size;
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe)) return false;
    for (int up = 0; up < 3; ++up) {
        char *slash = strrchr(exe, '/');
        if (!slash) return false;
        *slash = 0;
    }
    return snprintf(out, size, "%s/%s/build/%s", exe, folder, name) < (int)size;
}
