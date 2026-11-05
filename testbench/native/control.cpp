#include "input_state.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace frame {
Control::~Control() { stop(); }
bool Control::start() {
    const char *configured = std::getenv("FRAME_TESTBENCH_SOCKET");
    path_ = configured ? configured : "/run/user/" + std::to_string(getuid()) + "/frame-testbench.sock";
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    if (path_.empty() || path_[0] != '/' || path_.size() >= sizeof(addr.sun_path)) return false;
    // Parent directories must prevent pathname replacement by another user.
    struct stat parent{};
    const auto directory = path_.substr(0, path_.find_last_of('/'));
    if (stat(directory.c_str(), &parent) || parent.st_uid != getuid() || (parent.st_mode & 0022)) return false;
    // Refuse live sockets, symlinks and non-socket files. Reclaim only an owned
    // stale socket after checking its identity again immediately before unlink.
    struct stat stale{};
    if (!lstat(path_.c_str(), &stale)) {
        if (!S_ISSOCK(stale.st_mode) || stale.st_uid != getuid()) return false;
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0) return false;
        memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
        const int result = connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        const int error = errno;
        close(probe);
        if (!result || error != ECONNREFUSED) return false;
        struct stat current{};
        if (lstat(path_.c_str(), &current) || current.st_ino != stale.st_ino || current.st_dev != stale.st_dev
            || unlink(path_.c_str())) return false;
    } else if (errno != ENOENT) return false;
    listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) return false;
    memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    if (bind(listener_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
        close(listener_); listener_ = -1; return false;
    }
    struct stat own{};
    if (lstat(path_.c_str(), &own)) { close(listener_); listener_ = -1; return false; }
    inode_ = own.st_ino; device_ = own.st_dev;
    if (chmod(path_.c_str(), 0600) || listen(listener_, 16)) { stop(); return false; }
    stop_ = false;
    try { worker_ = std::thread(&Control::run, this); }
    catch (...) { stop(); return false; }
    return true;
}
void Control::stop() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) { close(listener_); listener_ = -1; }
    struct stat current{};
    if (inode_ && !lstat(path_.c_str(), &current) && current.st_ino == inode_ && current.st_dev == device_)
        unlink(path_.c_str());
    inode_ = device_ = 0;
}
void Control::run() {
    using Clock = std::chrono::steady_clock;
    struct Client { int fd; std::string request, reply; Clock::time_point deadline; };
    std::vector<Client> clients;
    auto next = Clock::now();
    while (!stop_) {
        auto now = Clock::now();
        if (now >= next) { state_.tick(); next = now + std::chrono::microseconds(11111); }
        std::vector<pollfd> fds{{listener_, POLLIN, 0}};
        for (const auto &c : clients) fds.push_back({c.fd, short(c.reply.empty() ? POLLIN : POLLOUT), 0});
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
        const int result = poll(fds.data(), fds.size(), static_cast<int>(std::max<int64_t>(1, remaining)));
        if (result < 0 && errno != EINTR) break;
        // Traverse clients before accepting so poll indexes cannot become stale.
        for (size_t i = clients.size(); i-- > 0;) {
            auto &c = clients[i]; auto flags = fds[i + 1].revents;
            bool done = Clock::now() > c.deadline || (flags & (POLLERR | POLLNVAL));
            if (!done && c.reply.empty() && (flags & (POLLIN | POLLHUP))) {
                char buffer[1024]; const auto n = recv(c.fd, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    c.request.append(buffer, static_cast<size_t>(n));
                    const auto end = c.request.find('\n');
                    if (c.request.size() > 1024) c.reply = state_.command("invalid oversized request");
                    else if (end != std::string::npos) c.reply = state_.command(c.request.substr(0, end));
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) done = true;
            }
            if (!done && !c.reply.empty()) {
                const auto n = send(c.fd, c.reply.data(), c.reply.size(), MSG_NOSIGNAL);
                if (n > 0) { c.reply.erase(0, static_cast<size_t>(n)); done = c.reply.empty(); }
                else if (n < 0 && errno != EAGAIN && errno != EINTR) done = true;
            }
            if (done) { close(c.fd); clients.erase(clients.begin() + i); }
        }
        if (fds[0].revents & POLLIN) {
            // Bounded accept work prevents clients from starving the 90Hz tick.
            for (int n = 0; n < 16; ++n) {
                const int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                ucred cred{}; socklen_t size = sizeof(cred);
                if (clients.size() >= 32 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != getuid()) close(fd);
                else clients.push_back({fd, {}, {}, Clock::now() + std::chrono::seconds(2)});
            }
        }
    }
    for (const auto &c : clients) close(c.fd);
}
} // namespace frame
