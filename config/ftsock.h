/* FrameHands overlay's abstract Unix datagram socket. */
#ifndef FRAMEHANDS_SOCK_H
#define FRAMEHANDS_SOCK_H
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define FT_SOCK_HANDVIEW "framehands_view"
#define FT_SOCK_POINTER "framehands_pointer"

static inline socklen_t ft_sock_addr(const char *name, struct sockaddr_un *addr) {
    const size_t n = strlen(name);
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (n == 0 || n > sizeof addr->sun_path - 1) return 0;
    memcpy(addr->sun_path + 1, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}
#endif
