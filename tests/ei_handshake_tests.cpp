#include <cstdio>
#include <initializer_list>
#include <libei.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    // These endpoints never connect to a compositor or announce a device.
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    for (const bool peer_closes : {false, true}) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
            return 1;
        }
        auto* context = ei_new_sender(nullptr);
        ei_configure_name(context, "private handshake failure test");
        if (ei_setup_backend_fd(context, pair[0]) != 0) {
            return 2;
        }
        if (peer_closes) {
            close(pair[1]);
            ei_dispatch(context);
        } else {
            ei_disconnect(context);
        }
        bool disconnected = false;
        while (auto* event = ei_get_event(context)) {
            disconnected |= ei_event_get_type(event) == EI_EVENT_DISCONNECT;
            ei_event_unref(event);
        }
        ei_unref(context);
        if (!peer_closes) {
            close(pair[1]);
        }
        if (!disconnected) {
            std::fputs("Aborted negotiation did not report disconnect.\n", stderr);
            return 3;
        }
    }
    std::puts("Peer EOF and explicit cleanup during EI negotiation passed.");
}
