// Where the listener's head is, asked of ft-screens (headask.h).
#include "headask.h"

#include "../../conf/ftsock.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace voice {

bool ReadHead(const std::string &reply, double at[3], double &yaw) {
    double v[4];
    char more = 0;
    if (std::sscanf(reply.c_str(), "ok %lf %lf %lf %lf %c", &v[0], &v[1], &v[2], &v[3], &more) != 4) return false;
    for (int i = 0; i < 3; ++i)
        if (!(std::fabs(v[i]) <= 1000)) return false;
    if (!(std::fabs(v[3]) <= 360)) return false;
    at[0] = v[0], at[1] = v[1], at[2] = v[2];
    yaw = v[3] * M_PI / 180;
    return true;
}

bool HeadAsk::Open(const std::string &screens) {
    Close();
    screensLen_ = ft_sock_addr(screens.c_str(), &screens_);
    sock_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un own{};
    own.sun_family = AF_UNIX;   // no name given: the kernel picks one, for the answers to come to
    if (!screensLen_ || sock_ < 0 || bind(sock_, reinterpret_cast<sockaddr *>(&own), sizeof(sa_family_t)) != 0) return Close(), false;
    return true;
}

void HeadAsk::Close() {
    if (sock_ >= 0) close(sock_);
    sock_ = -1, asked_ = heard_ = -1e9;
}

void HeadAsk::Tick(double now) {
    if (sock_ < 0) return;
    char buf[256];
    for (int i = 0; i < 16; ++i) {
        sockaddr_un from;
        socklen_t fromLen = sizeof from;
        const ssize_t n = recvfrom(sock_, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (n < 0) break;
        if (fromLen != screensLen_ || std::memcmp(&from, &screens_, fromLen)) continue;   // not ft-screens' answer
        if (ReadHead(std::string(buf, size_t(n)), at_, yaw_)) heard_ = now;
    }
    if (now - asked_ < kEveryS) return;
    asked_ = now;
    sendto(sock_, "head", 4, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&screens_), screensLen_);
}

}  // namespace voice
