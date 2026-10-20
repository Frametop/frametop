#include "framekeyboard/instance.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace framekeyboard {
namespace {
constexpr char request[] = "recenter-v1";
constexpr char response[] = "accepted";

void fail(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}
void require_private_directory(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) < 0) {
        fail("inspect runtime directory");
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 0077) != 0) {
        throw std::runtime_error("keyboard runtime directory must be private to this user");
    }
}
sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.size() >= sizeof(result.sun_path)) {
        throw std::runtime_error("runtime socket path is too long");
    }
    std::memcpy(result.sun_path, name.c_str(), name.size() + 1);
    return result;
}
// Keep each connection bounded: a local client cannot stall rendering indefinitely.
bool readable(int fd, int timeout_ms) {
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & POLLIN);
}
} // namespace
std::filesystem::path VrInstance::runtime_directory() {
    std::filesystem::path base = "/run/user/" + std::to_string(getuid());
    // Prefer the canonical login-session directory so SSH and a desktop launch
    // find each other even when their environments differ.
    if (!std::filesystem::exists(base)) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            throw std::runtime_error("no user runtime directory is available");
        }
        base = runtime;
    }
    require_private_directory(base);
    return base / "framekeyboard";
}
VrInstance::VrInstance(const std::filesystem::path& directory, bool notify_existing)
    : socket_path_(directory / "vr.sock") {
    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
        fail("create runtime directory");
    }
    require_private_directory(directory);
    lock_ = open((directory / "vr.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) {
        fail("open keyboard instance lock");
    }
    try {
        struct stat info{};
        if (fstat(lock_, &info) < 0) {
            fail("inspect instance lock");
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || info.st_nlink != 1) {
            throw std::runtime_error("invalid keyboard instance lock");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            if (flock(lock_, LOCK_EX | LOCK_NB) == 0) {
                owner_ = true;
                start_listener();
                return;
            }
            if (errno != EWOULDBLOCK) {
                fail("lock keyboard instance");
            }
            if (!notify_existing) {
                return;
            }
            if (notify_owner()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("the running keyboard did not answer the recenter request");
    } catch (...) {
        if (listener_ >= 0) {
            close(listener_);
        }
        if (owner_) {
            unlink(socket_path_.c_str());
        }
        close(lock_);
        throw;
    }
}
void VrInstance::start_listener() {
    const auto endpoint = address(socket_path_);
    listener_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) {
        fail("create keyboard control socket");
    }
    // A crash leaves the path behind, but releases flock. Only the new lock owner
    // may remove that stale path; never unlink the lock file itself.
    if (unlink(socket_path_.c_str()) < 0 && errno != ENOENT) {
        fail("remove stale control socket");
    }
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        fail("bind keyboard control socket");
    }
    if (listen(listener_, 8) < 0) {
        fail("listen on keyboard control socket");
    }
}
bool VrInstance::notify_owner() {
    const auto endpoint = address(socket_path_);
    const int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (client < 0) {
        fail("create recenter client");
    }
    bool accepted = false;
    if (connect(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
        send(client, request, sizeof(request), MSG_NOSIGNAL) == sizeof(request) &&
        readable(client, 500)) {
        char message[64]{};
        const auto count = recv(client, message, sizeof(message), MSG_DONTWAIT);
        accepted = count == sizeof(response) && std::memcmp(message, response, sizeof(response)) == 0;
    }
    close(client);
    return accepted;
}
bool VrInstance::poll_recenter() {
    if (!owner_) {
        return false;
    }
    bool requested = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            fail("accept recenter request");
        }
        ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == getuid() &&
            readable(client, 20)) {
            char message[64]{};
            const auto count = recv(client, message, sizeof(message), MSG_DONTWAIT);
            if (count == sizeof(request) && std::memcmp(message, request, sizeof(request)) == 0) {
                // Acknowledge only requests actually consumed by the render loop.
                requested = true;
                send(client, response, sizeof(response), MSG_NOSIGNAL);
            }
        }
        close(client);
    }
    return requested;
}
VrInstance::~VrInstance() {
    if (listener_ >= 0) {
        close(listener_);
    }
    if (owner_) {
        unlink(socket_path_.c_str());
    }
    if (lock_ >= 0) {
        close(lock_);
    }
}
} // namespace framekeyboard
