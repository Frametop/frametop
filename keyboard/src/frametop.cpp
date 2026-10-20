#include "framekeyboard/frametop.hpp"
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace framekeyboard {
namespace {
sockaddr_un abstract(const std::string& name, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (name.size() + 1 > sizeof(address.sun_path)) {
        throw std::runtime_error("Frametop socket name is too long");
    }
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return address;
}
} // namespace

FrametopLink::FrametopLink(std::string relay_name, Clock clock)
    : relay_(std::move(relay_name)), clock_(clock ? std::move(clock) : Clock([] {
          return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      })) {
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot create the Frametop socket");
    }
    // Autobind an abstract address for the relay's replies, and have the kernel tell us
    // who sent each one.
    const int on = 1;
    sockaddr_un self{};
    self.sun_family = AF_UNIX;
    if (setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0 ||
        bind(fd_, reinterpret_cast<const sockaddr*>(&self), sizeof(sa_family_t)) < 0) {
        close(fd_);
        throw std::runtime_error("Cannot bind the Frametop socket");
    }
}
FrametopLink::~FrametopLink() {
    if (fd_ >= 0) {
        close(fd_);
    }
}
bool FrametopLink::fresh() const {
    return clock_() - heard_ < stale_after;
}
bool FrametopLink::send(const std::string& message) {
    socklen_t length{};
    const auto address = abstract(relay_, length);
    return sendto(fd_, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                  reinterpret_cast<const sockaddr*>(&address),
                  length) == static_cast<ssize_t>(message.size());
}
FrametopLink::Request FrametopLink::take_request() {
    return std::exchange(request_, Request::None);
}
void FrametopLink::poll() {
    const double now = clock_();
    if (now - hello_ >= 1) {
        hello_ = now;
        send("kbhello");
    }
    socklen_t relay_length{};
    const auto relay = abstract(relay_, relay_length);
    for (;;) {
        char data[256];
        sockaddr_un from{};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(ucred))];
        iovec io{data, sizeof(data) - 1};
        msghdr message{};
        message.msg_name = &from;
        message.msg_namelen = sizeof(from);
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        const auto count = recvmsg(fd_, &message, MSG_DONTWAIT);
        if (count < 0) {
            return;
        }
        const ucred* credentials = nullptr;
        for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS) {
                credentials = reinterpret_cast<const ucred*>(CMSG_DATA(header));
            }
        }
        if (!credentials || credentials->uid != getuid() || message.msg_namelen != relay_length ||
            std::memcmp(&from, &relay, relay_length) != 0) {
            continue; // Not our user's relay: ignore it.
        }
        const std::string text(data, static_cast<std::size_t>(count));
        int desktop = 0, surface = 0;
        if (text == "kb show") {
            request_ = Request::Show;
        } else if (text == "kb hide") {
            request_ = Request::Hide;
        } else if (text == "kb toggle") {
            request_ = Request::Toggle;
        } else if (std::sscanf(text.c_str(), "kb %d %d", &desktop, &surface) == 2) {
            desktop_ = desktop != 0;
            surface_ = surface != 0;
            heard_ = clock_();
        }
    }
}

FrametopSink::~FrametopSink() {
    for (const auto& [code, desktop] : held_) {
        if (desktop) {
            link_.send("kbkey " + std::to_string(code) + " 0");
        }
    }
}
void FrametopSink::send(int code, int value) {
    if (value == 2) {
        return; // The receiving desktop and gamescope repeat held keys themselves.
    }
    if (value == 0) {
        const auto held = held_.find(code);
        const bool desktop = held != held_.end() ? held->second : link_.desktop();
        if (held != held_.end()) {
            held_.erase(held);
        }
        if (desktop) {
            link_.send("kbkey " + std::to_string(code) + " 0");
        } else {
            steam_.send(code, 0);
        }
        return;
    }
    const bool desktop = link_.desktop();
    if (!held_.emplace(code, desktop).second) {
        return;
    }
    if (desktop) {
        link_.send("kbkey " + std::to_string(code) + " 1");
    } else {
        steam_.send(code, 1);
    }
}
bool FrametopSink::pump() {
    link_.poll();
    const bool steam = steam_.pump();
    return link_.desktop() || steam;
}
bool FrametopSink::commit_text(const std::string& text) {
    if (link_.desktop()) {
        return link_.send("kbtext " + text);
    }
    return steam_.commit_text(text);
}
} // namespace framekeyboard
