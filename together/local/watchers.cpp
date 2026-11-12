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
    if (watching_.size() >= most_) return false;
    watching_.push_back({from, len, now});
    return true;
}

size_t Watchers::SendAs(uint8_t kind, int sock, int person, const uint8_t *datagram, size_t n, double now) {
    if (watching_.empty()) return 0;
    std::vector<uint8_t> out(n + 2);
    out[0] = kind, out[1] = uint8_t(person);
    std::memcpy(out.data() + 2, datagram, n);
    size_t missed = 0;
    for (size_t i = 0; i < watching_.size();) {
        const Watcher &w = watching_[i];
        const bool sent = now - w.asked <= kKeptS && sendto(sock, out.data(), out.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&w.at), w.len) >= 0;
        const bool gone = now - w.asked > kKeptS || (!sent && errno == ECONNREFUSED);
        missed += !sent && !gone;
        if (gone) watching_.erase(watching_.begin() + long(i));
        else ++i;
    }
    return missed;
}

void Watchers::Tell(int sock, const std::string &text, double now) const {
    for (const Watcher &w : watching_)
        if (now - w.asked <= kKeptS) sendto(sock, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&w.at), w.len);
}

}  // namespace local
