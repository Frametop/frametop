// A program's end of ft-together's local socket (client.h).
#include "client.h"

#include "../../conf/ftsock.h"
#include "../../together/local/datagrams.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace roomclient {

bool Client::Open(const std::string &own, const std::string &together, bool watch, std::string &err) {
    Close();
    sockaddr_un addr;
    const socklen_t len = ft_sock_addr(own.c_str(), &addr);
    togetherLen_ = ft_sock_addr(together.c_str(), &together_);
    if (!len || !togetherLen_) return err = "that isn't a socket's name", false;
    sock_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock_ < 0 || bind(sock_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
        err = "bind @" + own + ": " + std::strerror(errno) + " (already running?)";
        return Close(), false;
    }
    watch_ = watch;
    return true;
}

void Client::Close() {
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
    people_.Gone();
    watch_ = laid_ = none_ = false;
    asked_ = watched_ = floored_ = heard_ = -1e9;
    questions_.clear();
    posesIn_ = 0;
}

void Client::Ask(const std::string &text) {
    sendto(sock_, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&together_), togetherLen_);
}

bool Client::Send(const proto::Pose &pose) {
    const std::vector<uint8_t> bytes = proto::Pack(pose);
    return sock_ >= 0 &&
           sendto(sock_, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&together_), togetherLen_) == ssize_t(bytes.size());
}

// One of ft-together's answers. They aren't numbered: each kind is told by how it reads.
void Client::Answer(const std::string &text, double now) {
    if (text.rfind("ok {", 0) == 0) {   // state
        people_.Room(text.substr(3));
        heard_ = now;
    } else if (text == "ok none") {     // floor: none laid yet
        none_ = true, laid_ = false;
    } else if (text.rfind("ok ", 0) == 0) {   // floor: the one that stands
        roomfloor::Floor f;
        char more = 0;
        if (std::sscanf(text.c_str(), "ok %lf %lf %lf %c", &f.x, &f.z, &f.yaw, &more) == 3 && std::isfinite(f.x) && std::isfinite(f.z) && std::isfinite(f.yaw))
            floor_ = f, laid_ = true, none_ = false;
    }
    // "ok" alone is `watch` heard, and an error says what the next state will
}

void Client::Take(double now) {
    uint8_t buf[2048];
    for (int i = 0; i < 256; ++i) {
        Question q;
        q.fromLen = sizeof q.from;
        const ssize_t n = recvfrom(sock_, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&q.from), &q.fromLen);
        if (n < 0) return;
        if (q.fromLen == togetherLen_ && !std::memcmp(&q.from, &together_, togetherLen_)) {
            if (n > 0 && buf[0] == local::kPoseToWatcher) posesIn_ += people_.Pose(buf, size_t(n), now);
            else Answer(std::string(reinterpret_cast<const char *>(buf), size_t(n)), now);
        } else if (questions_.size() < kMostQuestions) {
            q.text.assign(reinterpret_cast<const char *>(buf), size_t(n));
            while (!q.text.empty() && (q.text.back() == '\n' || q.text.back() == ' ')) q.text.pop_back();
            questions_.push_back(q);
        }
    }
}

void Client::Tick(double now, const double *head) {
    if (sock_ < 0) return;
    Take(now);
    if (now - heard_ > kSilentS) people_.Gone();
    if (!people_.inRoom()) laid_ = none_ = false;
    if (now - asked_ >= kStateS) asked_ = now, Ask("state");
    if (!people_.inRoom()) return;
    if (watch_ && now - watched_ >= kWatchS) watched_ = now, Ask("watch");
    if (now - floored_ < (laid_ ? kFloorKnownS : kFloorS)) return;
    floored_ = now;
    if (none_ && head) {   // nobody has laid it: where the wearer stands now is their spot
        const roompeople::Person &me = people_.person(people_.me());
        const roomfloor::Floor f = roomfloor::Lay(me.spot[0], me.spot[1], me.spot[2], head[0], head[1], head[2]);
        char text[96];
        std::snprintf(text, sizeof text, "floor %.6f %.6f %.6f", f.x, f.z, f.yaw);
        Ask(text);
    } else {
        Ask("floor");
    }
}

bool Client::Next(Question &out) {
    if (questions_.empty()) return false;
    out = questions_.front();
    questions_.pop_front();
    return true;
}

void Client::Reply(const Question &to, const std::string &text) {
    if (sock_ >= 0 && to.fromLen > offsetof(sockaddr_un, sun_path))
        sendto(sock_, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&to.from), to.fromLen);
}

}  // namespace roomclient
