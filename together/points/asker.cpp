// ft-roompoints' socket (asker.h).
#include "asker.h"

#include "../../conf/ftsock.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <unistd.h>

namespace points {

bool Asker::Open(const std::string &own, const std::string &together, const std::string &panels, std::string &err) {
    Close();
    sockaddr_un addr;
    const socklen_t len = ft_sock_addr(own.c_str(), &addr);
    togetherLen_ = ft_sock_addr(together.c_str(), &together_);
    panelsLen_ = ft_sock_addr(panels.c_str(), &panels_);
    if (!len || !togetherLen_ || !panelsLen_) return err = "that isn't a socket's name", false;
    sock_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock_ < 0 || bind(sock_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
        err = "bind @" + own + ": " + std::strerror(errno) + " (already running?)";
        return Close(), false;
    }
    return true;
}

void Asker::Close() {
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
    pointsAsked_ = panelsAsked_ = panelsHeard_ = -1e9;
    questions_.clear();
    pointsIn_ = 0;
}

void Asker::Ask(const sockaddr_un &to, socklen_t len, const char *text) {
    sendto(sock_, text, std::strlen(text), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&to), len);
}

void Asker::Tick(double now, Marks &marks, PanelKeys &keys) {
    if (sock_ < 0) return;
    uint8_t buf[2048];
    for (int i = 0; i < 256; ++i) {
        Question q;
        q.fromLen = sizeof q.from;
        const ssize_t n = recvfrom(sock_, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&q.from), &q.fromLen);
        if (n < 0) break;
        if (q.fromLen == togetherLen_ && !std::memcmp(&q.from, &together_, togetherLen_)) {
            pointsIn_ += marks.Take(buf, size_t(n), now);   // anything else is its "ok", or that there is no room
        } else if (q.fromLen == panelsLen_ && !std::memcmp(&q.from, &panels_, panelsLen_)) {
            if (keys.Read(std::string(reinterpret_cast<const char *>(buf), size_t(n)))) panelsHeard_ = now;
        } else if (questions_.size() < kMostQuestions) {
            q.text.assign(reinterpret_cast<const char *>(buf), size_t(n));
            while (!q.text.empty() && (q.text.back() == '\n' || q.text.back() == ' ')) q.text.pop_back();
            questions_.push_back(q);
        }
    }
    if (now - panelsHeard_ > kSilentS) keys.Forget();
    if (now - pointsAsked_ >= kPointsS) pointsAsked_ = now, Ask(together_, togetherLen_, "points");
    if (now - panelsAsked_ >= kPanelsS) panelsAsked_ = now, Ask(panels_, panelsLen_, "panels");
}

bool Asker::Next(Question &out) {
    if (questions_.empty()) return false;
    out = questions_.front();
    questions_.pop_front();
    return true;
}

void Asker::Reply(const Question &to, const std::string &text) {
    if (sock_ >= 0 && to.fromLen > offsetof(sockaddr_un, sun_path))
        sendto(sock_, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&to.from), to.fromLen);
}

}  // namespace points
