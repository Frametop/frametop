#pragma once
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace framekeyboard {
// Unix sockets can block in connect when a compositor's accept queue is full.
// Both transports retry later instead of spending a VR frame waiting there.
inline int connect_local_socket(std::filesystem::path path) {
    if (path.is_relative()) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            errno = ENOENT;
            return -1;
        }
        path = std::filesystem::path(runtime) / path;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.empty() || name.size() >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd >= 0 && connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
} // namespace framekeyboard
