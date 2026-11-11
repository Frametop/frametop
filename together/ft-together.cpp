// ft-together: this Frame's end of Frametop Together (docs/together.md, together/README.md).
// It holds the links to the other Frames and the session on them, and answers the rest of
// Frametop on a local socket, one datagram a question (session/commands.h):
//
//   ft-together [--port N] [--name NAME] [--socket NAME] [--loopback]
//
//   --port N       where the other Frames call (47193)
//   --name NAME    what the others call you (the headset's name on the tailnet)
//   --socket NAME  the local socket, "@NAME" (ft_together)
//   --loopback     for tests: no Tailscale, the links are on 127.0.0.1 only, and everyone
//                  there is let in as "someone@loopback"
//
// It takes only calls that were sent to this Frame's tailnet address, and the session lets in
// only someone tailscaled knows at the address a call came from. While Tailscale isn't running
// it waits, and says so to whoever asks.
//
// One loop, one thread of ours: the links queue what msquic's threads see (net/quiclinks.h).
#include "../conf/ftsock.h"
#include "net/quiclinks.h"
#include "net/tailnet.h"
#include "session/commands.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/random.h>
#include <unistd.h>

namespace {

constexpr double kRetryS = 3;        // between looks at whether Tailscale is up
constexpr double kLeaveS = 0.5;      // what leaving gets to reach the others when the program ends
constexpr int kMostAtOnce = 256;     // events or questions taken before the loop looks at the rest
constexpr size_t kMostQuestion = 512;

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

uint64_t Dice() {
    uint64_t v = 0;
    // an invite that can be guessed is worse than no program
    if (getrandom(&v, sizeof v, 0) != ssize_t(sizeof v)) std::abort();
    return v;
}

struct Args {
    uint16_t port = 47193;
    std::string name, socket = FT_SOCK_TOGETHER;
    bool loopback = false;
};

bool Read(int argc, char **argv, Args &args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--loopback") {
            args.loopback = true;
        } else if (a == "--port" && value) {
            char *end = nullptr;
            const long port = std::strtol(value, &end, 10);
            if (!*value || *end || port < 1 || port > 65535) return false;
            args.port = uint16_t(port), ++i;
        } else if (a == "--name" && value && *value) {
            args.name = value, ++i;
        } else if (a == "--socket" && value && *value) {
            args.socket = value, ++i;
        } else {
            return false;
        }
    }
    return true;
}

// This Frame's end, once the tailnet says who we are: the links, and the session on them.
struct End {
    std::unique_ptr<net::QuicLinks> links;
    std::unique_ptr<session::Session> session;
};

// False with nothing in `err` while Tailscale isn't up, and with the reason when it can't work.
bool Start(const Args &args, session::Tailnet &tailnet, End &end, std::string &err) {
    session::Who who;
    net::QuicLinks::Config links;
    if (!tailnet.Self(who, links.listen)) return false;
    links.port = args.port;
    links.mayCall = [&tailnet](const std::string &address) { return tailnet.MayCall(address); };
    end.links = net::QuicLinks::Open(links, err);
    if (!end.links) return false;
    session::Config config;
    config.name = args.name.empty() ? who.name : args.name, config.port = args.port, config.dice = Dice;
    end.session = std::make_unique<session::Session>(*end.links, tailnet, config);
    std::printf("ft-together: %s (%s) at %s port %d\n", config.name.c_str(), who.login.c_str(), links.listen.c_str(), int(args.port));
    return true;
}

void Tell(session::Session &session, const net::Event &e, double now) {
    switch (e.kind) {
    case net::Event::kUp: session.Up(e.link, e.address, now); break;
    case net::Event::kBytes: session.Bytes(e.link, e.bytes.data(), e.bytes.size(), now); break;
    case net::Event::kDown: session.Down(e.link, now); break;
    case net::Event::kDatagram: break;   // poses and voice: nobody sends them yet
    }
}

// One question from the local socket answered, if one waits.
bool Answer(int sock, End &end, double now) {
    char text[kMostQuestion + 1];
    sockaddr_un from;
    socklen_t fromLen = sizeof from;
    const ssize_t n = recvfrom(sock, text, sizeof text, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (n < 0) return false;
    std::string reply;
    if (size_t(n) > kMostQuestion) reply = "error that is too long";
    else if (!end.session) reply = "error Tailscale isn't running on this headset";
    else reply = session::Command(*end.session, std::string(text, size_t(n)), now);
    // an asker that doesn't wait for its answer doesn't hold up the loop
    if (fromLen > sizeof(sa_family_t)) sendto(sock, reply.data(), reply.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(&from), fromLen);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Args args;
    if (!Read(argc, argv, args)) return std::fprintf(stderr, "usage: ft-together [--port N] [--name NAME] [--socket NAME] [--loopback]\n"), 2;
    struct sigaction stop = {};
    stop.sa_handler = Stop;   // no SA_RESTART: poll comes back, and the loop ends
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr;
    const socklen_t addrLen = ft_sock_addr(args.socket.c_str(), &addr);
    if (sock < 0 || !addrLen || bind(sock, reinterpret_cast<sockaddr *>(&addr), addrLen) != 0)
        return std::fprintf(stderr, "ft-together: bind @%s: %s (already running?)\n", args.socket.c_str(), std::strerror(errno)), 1;

    std::unique_ptr<session::Tailnet> tailnet;
    if (args.loopback) tailnet = std::make_unique<net::Loopback>(args.name.empty() ? "frame" : args.name);
    else tailnet = std::make_unique<net::Tailscale>();

    End end;
    double tried = -1e9, ticked = Now();
    bool waiting = false;
    std::string note;
    while (!g_stop) {
        double now = Now();
        if (!end.session && now - tried >= kRetryS) {
            tried = now;
            std::string err;
            if (!Start(args, *tailnet, end, err)) {
                if (!err.empty()) return std::fprintf(stderr, "ft-together: %s\n", err.c_str()), 1;
                if (!waiting) std::printf("ft-together: waiting for Tailscale\n");
                waiting = true;
            }
        }
        pollfd fds[2] = {{sock, POLLIN, 0}, {end.links ? end.links->fd() : -1, POLLIN, 0}};
        if (poll(fds, 2, 1000) < 0 && errno != EINTR) return std::perror("ft-together: poll"), 1;
        now = Now();
        if (end.session) {
            net::Event e;
            for (int i = 0; i < kMostAtOnce && end.links->Next(e); ++i) Tell(*end.session, e, now);
        }
        for (int i = 0; i < kMostAtOnce && Answer(sock, end, now); ++i) {}
        if (end.session && now - ticked >= 1) {
            ticked = now;
            end.session->Tick(now);
            end.links->Tick();
        }
        if (end.session && end.session->note() != note) {
            note = end.session->note();
            if (!note.empty()) std::printf("ft-together: %s\n", note.c_str());
        }
    }

    if (end.session) {   // the others hear that we left, not ten seconds of nothing
        end.session->Leave();
        for (const double until = Now() + kLeaveS; !end.links->Quiet() && Now() < until;) usleep(10000);
    }
    return 0;
}
