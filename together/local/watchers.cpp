// Who gets the room's poses, or its voices, on this Frame (watchers.h).
#include "watchers.h"

#include <cerrno>
#include <cstring>

namespace local {

bool Watchers::Add(const sockaddr_un &from, socklen_t len, double now) {
    if (len <= offsetof(sockaddr_un, sun_path) || len > sizeof(sockaddr_un)) return false;
    for (Watcher &w : watching_)
        if (w.len == len && !std::memcmp(&w.at, &from, len)) return w.asked = now, true;
    for (size_t i = 0; i < watching_.size();)   // room for a new one where an old one stopped asking
        if (now - watching_[i].asked > kKeptS) watching_.erase(watching_.begin() + long(i));
        else ++i;
    if (watching_.size() >= kMost) return false;
    watching_.push_back({from, len, now});
    return true;
}

void Watchers::Send(int sock, int person, const uint8_t *datagram, size_t n, double now) {
    if (watching_.empty()) return;
    std::vector<uint8_t> out(n + 2);
    out[0] = kind_, out[1] = uint8_t(person);
    std::memcpy(out.data() + 2, datagram, n);
    for (size_t i = 0; i < watching_.size();) {
        const Watcher &w = watching_[i];
        const bool gone = now - w.asked > kKeptS ||
                          (sendto(sock, out.data(), out.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&w.at), w.len) < 0 && errno == ECONNREFUSED);
        if (gone) watching_.erase(watching_.begin() + long(i));
        else ++i;
    }
}

}  // namespace local
