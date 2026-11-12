// This Frame's media programs on their sockets (media.h).
#include "media.h"

#include "../../conf/ftsock.h"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace local {

Media::~Media() {
    for (const Client &c : clients_) close(c.fd);
    if (listen_ >= 0) close(listen_);
}

bool Media::Listen(const std::string &name) {
    sockaddr_un addr;
    const socklen_t len = ft_sock_addr(name.c_str(), &addr);
    if (!len) return errno = ENAMETOOLONG, false;
    listen_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    return listen_ >= 0 && bind(listen_, reinterpret_cast<sockaddr *>(&addr), len) == 0 && listen(listen_, 8) == 0;
}

void Media::Fds(std::vector<pollfd> &fds) const {
    fds.push_back({listen_, POLLIN, 0});
    for (const Client &c : clients_) fds.push_back({c.fd, short(POLLIN | (c.sent < c.out.size() ? POLLOUT : 0)), 0});
}

void Media::Accept(bool serving, double now) {
    for (;;) {
        const int fd = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        ucred who = {};
        socklen_t len = sizeof who;
        const bool ours = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &who, &len) == 0 && who.uid == geteuid();
        if (!serving || !ours || clients_.size() >= kMostClients) {
            close(fd);
            continue;
        }
        Client c;
        c.fd = fd, c.since = now;
        clients_.push_back(std::move(c));
    }
}

Media::Client *Media::Find(Role role, int which) {
    for (Client &c : clients_)
        if (!c.gone && c.role == role && c.which == which) return &c;
    return nullptr;
}

void Media::Handle(Client &c, uint8_t kind, const std::vector<uint8_t> &body, flow::Video &video, double now) {
    switch (c.role) {
    case Role::kNone: {
        const bool source = kind == kMediaSource && body.size() == 1 && body[0] < room::kFramePanels;
        const bool sink = kind == kMediaSink && body.size() == 1 && body[0] < proto::kPeople * proto::kPanelNumbers;
        if (!source && !sink) break;
        const Role role = source ? Role::kSource : Role::kSink;
        // The new one takes the old one's place: also of one that went in this same pass and
        // isn't cleared away yet (its going must not take the new one's watch with it).
        for (Client &old : clients_)
            if (old.role == role && old.which == body[0]) old.role = Role::kNone, old.gone = true;
        c.role = role, c.which = body[0];
        if (source) video.SourceCame(c.which);
        else video.SinkCame(c.which, now);
        return;
    }
    case Role::kSource:
        if (kind != kMediaUnit || body.size() < 2 || body[0] > 1) break;
        video.Unit(c.which, body[0] == 1, body.data() + 1, body.size() - 1, now);
        return;
    case Role::kSink:
        if (kind == kMediaAttention && body.size() == 2 && body[0] <= proto::kFocused && body[1] <= 1) {
            video.Attention(c.which, body[0], body[1] == 1, now);
            return;
        }
        if (kind == kMediaDecoder && body.size() == 1 && body[0] <= kDecoderRefused) {
            if (body[0] == kDecoderRefused) video.SinkRefused(c.which, now);
            else video.SinkDecodes(c.which, body[0] == kDecoderHas, now);
            return;
        }
        break;
    }
    c.gone = true;   // it said what it may not
}

void Media::Serve(flow::Video *video, double now) {
    Accept(video != nullptr, now);
    uint8_t buf[65536];
    std::vector<uint8_t> body;
    // (nothing joins clients_ from here on, so `c` stays where it is while the video's rules
    // call back with what is to be said to another one)
    for (Client &c : clients_) {
        for (size_t taken = 0; video && !c.gone && taken < kMostAPass;) {
            const ssize_t n = recv(c.fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n <= 0 || !c.in.Feed(buf, size_t(n))) {
                c.gone = true;
                break;
            }
            taken += size_t(n);
            uint8_t kind = 0;
            while (!c.gone && c.in.Next(kind, body)) Handle(c, kind, body, *video, now);
            c.gone = c.gone || c.in.broken();
        }
        if (c.role == Role::kNone && now - c.since > kHelloS) c.gone = true;
    }
    for (Client &c : clients_)
        if (!c.gone) Flush(c);
    for (size_t i = 0; i < clients_.size();) {
        Client &c = clients_[i];
        if (!c.gone) {
            ++i;
            continue;
        }
        const bool sink = c.role == Role::kSink;
        const uint8_t which = c.which;
        close(c.fd);
        clients_.erase(clients_.begin() + long(i));
        if (sink && video) video->SinkWent(which);
    }
}

// Whether `more` bytes may join what waits for the program. One that doesn't take what it is
// sent is let go.
bool Media::Room(Client &c, size_t more) {
    if (c.out.size() - c.sent + more <= kMostOut) return true;
    c.gone = true;
    return false;
}

void Media::Flush(Client &c) {
    while (c.sent < c.out.size()) {
        const ssize_t n = send(c.fd, c.out.data() + c.sent, c.out.size() - c.sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) {
            c.gone = true;
            return;
        }
        c.sent += size_t(n);
    }
    if (c.sent == c.out.size()) c.out.clear(), c.sent = 0;
    else if (c.sent > (1u << 20)) c.out.erase(c.out.begin(), c.out.begin() + long(c.sent)), c.sent = 0;
}

void Media::Rate(int number, int bitrate) {
    Client *c = Find(Role::kSource, number);
    if (!c || !Room(*c, 9)) return;
    PutMediaRate(c->out, uint32_t(bitrate));
    Flush(*c);
}

void Media::Full(int number, int aSecond) { Count(number, kMediaFull, aSecond); }

void Media::Pictures(int number, int aSecond) { Count(number, kMediaPictures, aSecond); }

// One of the words that are a number of pictures a second, to a source.
void Media::Count(int number, uint8_t kind, int aSecond) {
    Client *c = Find(Role::kSource, number);
    if (!c || !Room(*c, 6)) return;
    PutMedia(c->out, kind, uint8_t(std::clamp(aSecond, 1, 255)));
    Flush(*c);
}

void Media::Key(int number) {
    Client *c = Find(Role::kSource, number);
    if (!c || !Room(*c, 5)) return;
    PutMedia(c->out, kMediaKey);
    Flush(*c);
}

void Media::Watched(int number, bool watched) {
    Client *c = Find(Role::kSource, number);
    if (!c || !Room(*c, 6)) return;
    PutMedia(c->out, kMediaWatched, uint8_t(watched ? 1 : 0));
    Flush(*c);
}

void Media::Show(uint8_t panel, bool key, const std::vector<uint8_t> &unit) {
    Client *c = Find(Role::kSink, panel);
    if (!c || unit.empty() || unit.size() > kMostMediaUnit || !Room(*c, 6 + unit.size())) return;
    PutMediaUnit(c->out, key, unit.data(), unit.size());
    Flush(*c);
}

void Media::Decode(uint8_t panel, bool decode) {
    Client *c = Find(Role::kSink, panel);
    if (!c || !Room(*c, 6)) return;
    PutMedia(c->out, kMediaDecoder, uint8_t(decode ? 1 : 0));
    Flush(*c);
}

std::string Media::State() const {
    std::string sources, sinks;
    for (const Client &c : clients_) {
        if (c.gone || c.role == Role::kNone) continue;
        std::string &list = c.role == Role::kSource ? sources : sinks;
        list += (list.empty() ? "" : ",") + std::to_string(int(c.which));
    }
    return "{\"sources\":[" + sources + "],\"sinks\":[" + sinks + "]}";
}

}  // namespace local
