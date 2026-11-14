// The address a call goes to (lookup.h).
#include "lookup.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

namespace net {

std::string Lookup(const std::string &host, const std::function<bool(const std::string &address)> &allowed) {
    addrinfo hints = {};
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0) return "";
    std::string out;
    for (int family : {AF_INET, AF_INET6})
        for (const addrinfo *a = found; a && out.empty(); a = a->ai_next) {
            if (a->ai_family != family) continue;
            char buf[INET6_ADDRSTRLEN] = "";
            if (family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(a->ai_addr)->sin_addr, buf, sizeof buf);
            else inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(a->ai_addr)->sin6_addr, buf, sizeof buf);
            if (buf[0] && allowed && allowed(buf)) out = buf;
        }
    freeaddrinfo(found);
    return out;
}

}  // namespace net
