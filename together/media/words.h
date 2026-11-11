// A media program's words with ft-screens: short text messages on a SOCK_SEQPACKET socket, one
// a packet, some with DMA-BUF fds beside them (screenfeed.h, ft-peerpanel.cpp). Header only.
#pragma once

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace media {

constexpr int kMostWordFds = 4;

// One message, with `nfds` fds (at most kMostWordFds). It doesn't wait: false when the socket
// won't take it now, or its far end is gone.
inline bool SendWords(int fd, const std::string &msg, const int *fds = nullptr, int nfds = 0) {
    iovec iov = {const_cast<char *>(msg.data()), msg.size()};
    msghdr m = {};
    m.msg_iov = &iov, m.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int) * kMostWordFds)] = {};
    if (nfds > 0 && nfds <= kMostWordFds) {
        m.msg_control = control, m.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
        cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
        std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
    }
    return sendmsg(fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) == ssize_t(msg.size());
}

// The next message that waits: 1 with it in `msg` and its fds in `fds` (room for kMostWordFds;
// they are the caller's to close), 0 when none waits, -1 when the far end is gone.
inline int TakeWords(int fd, std::string &msg, int *fds, int &nfds) {
    nfds = 0;
    for (;;) {
        char buf[768];
        char control[CMSG_SPACE(sizeof(int) * kMostWordFds)];
        iovec iov = {buf, sizeof buf};
        msghdr m = {};
        m.msg_iov = &iov, m.msg_iovlen = 1, m.msg_control = control, m.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;
        for (cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const int got = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int k = 0; k < got; ++k) {
                int f;
                std::memcpy(&f, CMSG_DATA(c) + size_t(k) * sizeof(int), sizeof f);
                if (nfds < kMostWordFds) fds[nfds++] = f;
                else close(f);
            }
        }
        msg.assign(buf, size_t(n));
        return 1;
    }
}

}  // namespace media
