// Whether gaze mode is on, for the palm menu's Quick Eye Calibrate row (vrquick.cpp): the pointer
// helper's answer to "gaze ?" ("ok on" or "ok off"), asked about every 2 s from a socket of our own
// (autobound, so the answer has somewhere to go). No answer for 5 s (no helper): off.
#include "vrpanel.h"
#include "../conf/ftsock.h"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ftvr {
namespace {

constexpr int64_t kAskMs = 2000, kStaleMs = 5000;

struct {
    int fd = -1;
    int64_t askedMs = INT64_MIN / 2, heardMs = INT64_MIN / 2;
    bool on = false;
} g_mode;

bool Open() {
    if (g_mode.fd >= 0) return true;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    sockaddr_un me{};
    me.sun_family = AF_UNIX;
    if (bind(fd, reinterpret_cast<sockaddr *>(&me), sizeof(sa_family_t)) != 0) {   // (autobind)
        close(fd);
        return false;
    }
    g_mode.fd = fd;
    return true;
}

}  // namespace

bool GazeModeOn() {
    const int64_t now = NowMs();
    if (Open()) {
        char buf[64];
        ssize_t n;
        while ((n = recv(g_mode.fd, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
            buf[n] = 0;
            if (std::strncmp(buf, "ok ", 3) == 0) g_mode.on = std::strncmp(buf + 3, "on", 2) == 0, g_mode.heardMs = now;
        }
        if (now - g_mode.askedMs >= kAskMs) {
            g_mode.askedMs = now;
            static const char kAsk[] = "gaze ?";
            ft_sock_send(g_mode.fd, FT_SOCK_POINTER_HELPER, kAsk, sizeof kAsk - 1, MSG_DONTWAIT);
        }
    }
    return g_mode.on && now - g_mode.heardMs < kStaleMs;
}

}  // namespace ftvr
