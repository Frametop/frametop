// A media program's end of ft-together's socket (mediawire.h): ft-sharepanel's, which hands over
// the pictures it encodes, and ft-peerpanel's, which is handed a panel's pictures to show.
//
// Nothing here waits: what can't be written yet is kept, up to kMostWaiting bytes, and a message
// past that is refused, so its sender drops it and says so (a picture dropped means the next has
// to be a keyframe). Header only.
#pragma once

#include "mediawire.h"

#include "../../conf/ftsock.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace local {

class MediaClient {
public:
    static constexpr size_t kMostWaiting = 4u << 20;

    // `most`: the longest message this program is ever sent (a source hears a few bytes, a sink
    // whole pictures).
    explicit MediaClient(size_t most) : reader_(most) {}
    ~MediaClient() { Close(); }
    MediaClient(const MediaClient &) = delete;
    MediaClient &operator=(const MediaClient &) = delete;

    // Connects to "@name" and says what this program is: `kind` kMediaSource with the panel's
    // number, or kMediaSink with the panel's id.
    bool Connect(const std::string &name, uint8_t kind, uint8_t which) {
        Close();
        sockaddr_un addr;
        const socklen_t len = ft_sock_addr(name.c_str(), &addr);
        if (!len) return errno = ENAMETOOLONG, false;
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        if (connect(fd_, reinterpret_cast<sockaddr *>(&addr), len) != 0) return Close(), false;
        const int flags = fcntl(fd_, F_GETFL);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        std::vector<uint8_t> hello;
        PutMedia(hello, kind, which);
        return Send(std::move(hello)) && Flush();
    }
    void Close() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1, out_.clear(), sent_ = 0;
    }
    bool open() const { return fd_ >= 0; }
    pollfd Poll() const { return {fd_, short(POLLIN | (sent_ < out_.size() ? POLLOUT : 0)), 0}; }

    // Takes what ft-together sent. False when it is gone, or sent what isn't a message.
    bool Read() {
        if (fd_ < 0) return false;
        for (;;) {
            uint8_t buf[65536];
            const ssize_t n = recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n <= 0 || !reader_.Feed(buf, size_t(n))) return false;
            if (size_t(n) < sizeof buf) return true;
        }
    }
    // The next whole message of what Read took.
    bool Next(uint8_t &kind, std::vector<uint8_t> &body) { return reader_.Next(kind, body); }

    // One or more whole messages to ft-together. False when too much already waits: nothing of
    // these is sent.
    bool Send(std::vector<uint8_t> &&bytes) {
        if (fd_ < 0 || out_.size() - sent_ + bytes.size() > kMostWaiting) return false;
        if (sent_ == out_.size()) out_ = std::move(bytes), sent_ = 0;
        else out_.insert(out_.end(), bytes.begin(), bytes.end());
        return true;
    }
    // Writes what the socket takes now. False when ft-together is gone.
    bool Flush() {
        while (fd_ >= 0 && sent_ < out_.size()) {
            const ssize_t n = send(fd_, out_.data() + sent_, out_.size() - sent_, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n <= 0) return false;
            sent_ += size_t(n);
        }
        if (sent_ == out_.size()) out_.clear(), sent_ = 0;
        return fd_ >= 0;
    }
    size_t waiting() const { return out_.size() - sent_; }

private:
    int fd_ = -1;
    MediaReader reader_;
    std::vector<uint8_t> out_;
    size_t sent_ = 0;
};

}  // namespace local
