/* ftsock.h: addresses, a send that's checked, and logging a failing destination. Built as C and
 * as C++. Its receiver binds a name of its own (with the pid in it), never one of Frametop's. */
#include "../ftsock.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures = 0;
static void check(int ok, const char *what) {
    if (!ok) printf("FAILED: %s\n", what), ++failures;
}

int main(void) {
    struct sockaddr_un addr;
    check(ft_sock_addr("frametop_relay", &addr) == offsetof(struct sockaddr_un, sun_path) + 1 + 14,
          "an address's length: the leading zero and the name");
    check(addr.sun_path[0] == 0 && memcmp(addr.sun_path + 1, "frametop_relay", 14) == 0, "abstract: a zero, then the name");
    check(ft_sock_addr("", &addr) == 0, "an empty name: no address");
    char longname[sizeof addr.sun_path + 1];
    memset(longname, 'x', sizeof longname - 1), longname[sizeof longname - 1] = 0;
    check(ft_sock_addr(longname, &addr) == 0, "a name too long: no address");
    longname[sizeof addr.sun_path - 1] = 0;
    check(ft_sock_addr(longname, &addr) != 0, "the longest name that fits");

    char name[64];
    snprintf(name, sizeof name, "ft_sock_test_%d", (int)getpid());
    const int rx = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    const int tx = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    const socklen_t len = ft_sock_addr(name, &addr);
    if (rx < 0 || tx < 0 || bind(rx, (const struct sockaddr *)&addr, len) != 0) return perror("socket"), 2;

    check(ft_sock_send(tx, name, "hello", 5, MSG_DONTWAIT) == 0, "a send to a bound name");
    char buf[16] = {0};
    check(recv(rx, buf, sizeof buf, 0) == 5 && memcmp(buf, "hello", 5) == 0, "...arrives whole");

    char nobody[64];
    snprintf(nobody, sizeof nobody, "ft_sock_test_nobody_%d", (int)getpid());
    check(ft_sock_send(tx, nobody, "x", 1, MSG_DONTWAIT) == ECONNREFUSED, "nothing bound: ECONNREFUSED");
    check(ft_sock_send(-1, name, "x", 1, MSG_DONTWAIT) == EBADF, "no socket: EBADF");
    check(ft_sock_send(tx, longname, "x", 1, MSG_DONTWAIT) == 0 ||
              ft_sock_send(tx, longname, "x", 1, MSG_DONTWAIT) == ECONNREFUSED,
          "the longest name: sent or refused, not a bad address");
    longname[sizeof addr.sun_path - 1] = 'x';
    check(ft_sock_send(tx, longname, "x", 1, MSG_DONTWAIT) == ENAMETOOLONG, "a name too long: ENAMETOOLONG");

    /* A full queue: EAGAIN, without waiting. */
    int full = 0;
    for (int i = 0; i < 100000 && !full; ++i) full = ft_sock_send(tx, name, "x", 1, MSG_DONTWAIT) == EAGAIN;
    check(full, "a full queue: EAGAIN");
    while (recv(rx, buf, sizeof buf, 0) > 0) {
    }

    /* The log: a failure is said when it starts, not again while it lasts, and "works again"
     * after; one more failure within 10 s isn't said. */
    struct ft_sock_log log = {nobody, 0, 0, 0};
    check(ft_sock_send_logged(tx, &log, "x", 1, MSG_DONTWAIT) == ECONNREFUSED && log.said && log.when > 0,
          "the first failure is logged");
    const long when = log.when;
    ft_sock_send_logged(tx, &log, "x", 1, MSG_DONTWAIT);
    check(log.when == when && log.err == ECONNREFUSED, "...and not again while it lasts");
    log.name = name;
    check(ft_sock_send_logged(tx, &log, "x", 1, MSG_DONTWAIT) == 0 && !log.said && log.err == 0, "works again");
    log.name = nobody;
    ft_sock_send_logged(tx, &log, "x", 1, MSG_DONTWAIT);
    check(!log.said && log.when == when, "a new failure within 10 s isn't logged");

    close(rx), close(tx);
    if (failures) return EXIT_FAILURE;
    printf("ftsock: ok\n");
    return 0;
}
