/* ftsock.h: Frametop's datagram sockets for C and C++: their names (in the abstract namespace,
 * "@name" in the docs) and one way to send to them, with the result checked. Header-only.
 *
 *   ft_sock_send(fd, FT_SOCK_FLOAT, msg, strlen(msg), MSG_DONTWAIT)   0, or an errno
 *   static struct ft_sock_log log = {FT_SOCK_FLOAT, 0, 0, 0};
 *   ft_sock_send_logged(fd, &log, msg, len, MSG_DONTWAIT)            ...and logs failures
 *
 * The Python side names the same sockets itself (input/input-relay.py, float/ft-floatd, ...).
 */
#ifndef FTSOCK_H
#define FTSOCK_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>

#define FT_SOCK_RELAY "frametop_relay"         /* the input relay (input/input-relay.py) */
#define FT_SOCK_FLOAT "frametop_float"         /* ft-floatd (float/), floating windows */
#define FT_SOCK_SCREENS "ft_screens"           /* ft-screens' control socket (screens/) */
#define FT_SOCK_POINTER "ft_pointer"           /* the pointer driver (pointer/driver/) */
#define FT_SOCK_POINTER_HELPER "ft_pointer_helper" /* the pointer helper (pointer/helper/) */
#define FT_SOCK_GAZED "ft_gazed"               /* the gaze service (gaze/) */
#define FT_SOCK_GAZEPANEL "ft_gazepanel"       /* the gaze checks' panel (gaze/panel/) */
#define FT_SOCK_HANDVIEW "ft_handview"         /* ft-handview (screens/handview.cpp) */
#define FT_SOCK_HANDPANEL "ft_handpanel"       /* the hand recorder's panel (hands/rec/panel/) */
#define FT_SOCK_FRUNNER "ft_frunner"           /* frunner (frunner/) */
#define FT_SOCK_SPOTPANEL "ft_spotpanel"       /* frunner's panel (frunner/ft-spotpanel.cpp) */
#define FT_SOCK_DASHPATCH "ft_dashpatch"       /* ft-dashpatch (pointer/dashpatch/) */
#define FT_SOCK_POWERD "ft_powerd"             /* ft-powerd (power/) */

/* @name's address; its length, or 0 for an empty name or one too long for an address. */
static inline socklen_t ft_sock_addr(const char *name, struct sockaddr_un *addr) {
    const size_t n = strlen(name);
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (n == 0 || n > sizeof addr->sun_path - 1) return 0;
    memcpy(addr->sun_path + 1, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

/* One datagram to @name from fd, with `flags` (MSG_DONTWAIT not to wait for room). 0 when all of
 * it went; else an errno: ECONNREFUSED (nothing is bound there), EAGAIN (its queue is full),
 * ENAMETOOLONG, EBADF, EMSGSIZE (only part of it went). */
static inline int ft_sock_send(int fd, const char *name, const void *msg, size_t len, int flags) {
    struct sockaddr_un addr;
    const socklen_t alen = ft_sock_addr(name, &addr);
    if (!alen) return ENAMETOOLONG;
    if (fd < 0) return EBADF;
    const ssize_t sent = sendto(fd, msg, len, flags | MSG_NOSIGNAL, (const struct sockaddr *)&addr, alen);
    if (sent < 0) return errno ? errno : EIO;
    return (size_t)sent == len ? 0 : EMSGSIZE;
}

/* A destination whose failures are logged (on stderr) when they start, at most once every 10
 * seconds, and once more when a send there works again. */
struct ft_sock_log {
    const char *name;
    int err;     /* the last send's result */
    int said;    /* its failure was logged */
    long when;   /* when a failure was last logged (CLOCK_MONOTONIC seconds) */
};

static inline int ft_sock_send_logged(int fd, struct ft_sock_log *log, const void *msg, size_t len, int flags) {
    const int err = ft_sock_send(fd, log->name, msg, len, flags);
    if (err && !log->err) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!log->when || now.tv_sec - log->when >= 10) {
            fprintf(stderr, "to @%s: %s\n", log->name, strerror(err));
            log->said = 1, log->when = now.tv_sec ? now.tv_sec : 1;
        }
    } else if (!err && log->err && log->said) {
        fprintf(stderr, "to @%s: works again\n", log->name);
        log->said = 0;
    }
    log->err = err;
    return err;
}

#endif
