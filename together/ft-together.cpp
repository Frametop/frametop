// ft-together: this Frame's end of Frametop Together (docs/together.md, together/README.md).
// It holds the links to the other Frames and the session on them, and answers the rest of
// Frametop on a local socket, one datagram a question: the session's (session/commands.h), and
//
//   watch            -> the asker gets the other people's poses from now on (local/watchers.h)
//   listen           -> the asker gets their voices
//   floor [X Z YAW]  -> where this Frame's own floor lies in the room (local/floor.h)
//   media            -> which shared panels have a program here, and how their video goes
//   here             -> where the others call this Frame: its address, port and name
//   wear [NAME]      -> the avatar the wearer has on, by its model's name: the others may have
//                       its file if it is one that may be sent (local/wardrobe.h)
//   looks            -> what of that the others see, and where the files of their avatars are
//
// A datagram there that is a pose or a voice (local/datagrams.h) is the wearer's own: it goes
// to everyone in the room, and theirs come back to whoever watches or listens. One that is the
// wearer's mouth goes out in their poses (local/mouth.h). The shared panels' video comes from
// and goes to this Frame's media programs on a second socket, "@NAME_media" (local/media.h), and
// between the Frames on the links, a stream a picture (flow/video.h). An avatar's file goes to
// whoever in the room asks its wearer for it, on a stream too (flow/looks.h).
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
#include "flow/looks.h"
#include "flow/video.h"
#include "local/floor.h"
#include "local/media.h"
#include "local/mouth.h"
#include "local/posegate.h"
#include "local/voicegate.h"
#include "local/wardrobe.h"
#include "local/watchers.h"
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
#include <vector>

namespace {

constexpr double kRetryS = 3;        // between looks at whether Tailscale is up
constexpr double kLeaveS = 0.5;      // what leaving gets to reach the others when the program ends
constexpr int kMostAtOnce = 256;     // events or questions taken before the loop looks at the rest
constexpr size_t kMostQuestion = 512;
constexpr int kSendingMs = 20;       // between looks at the links while pictures are on their way
constexpr int kMediaMs = 100;        // ...and at the video's rules while a media program is here

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

// The links, as the video sends its pictures on them and the avatars their files.
struct StreamWires : flow::Wires, flow::FileWires {
    net::QuicLinks &links;
    explicit StreamWires(net::QuicLinks &l) : links(l) {}
    bool Frame(int link, std::vector<uint8_t> bytes, uint32_t tag) override { return links.Frame(link, std::move(bytes), tag); }
    void Await(int link, size_t bytes) override { links.AwaitFile(link, bytes); }
    bool File(int link, const flow::FileBytes &file) override { return links.File(link, file); }
};

// This Frame's end, once the tailnet says who we are: the links, the session on them, the
// shared panels' video between the links and this Frame's programs, and the avatars' files.
struct End {
    std::unique_ptr<net::QuicLinks> links;
    std::unique_ptr<session::Session> session;
    std::unique_ptr<StreamWires> wires;
    std::unique_ptr<flow::Video> video;
    std::unique_ptr<flow::Looks> looks;
    std::string here;   // where the others call this Frame: "ADDRESS PORT NAME"
    bool inRoom() const { return session && (session->mode() == session::Mode::kHosting || session->mode() == session::Mode::kJoined); }
};

// What this Frame's own programs are given of the room, and give it (together/local).
struct Local {
    local::PoseGate gate;
    local::PoseCount count;
    local::Mouth mouth;   // the wearer's own, as the program that hears them tells it
    local::Watchers watchers;
    local::VoiceGate voices;
    local::Watchers listeners{local::kVoiceToListener};
    local::Floor floor;
    local::Media media;
    local::Wardrobe wardrobe;
    explicit Local(const std::string &name) : wardrobe(name) {}
};

// False with nothing in `err` while Tailscale isn't up, and with the reason when it can't work.
bool Start(const Args &args, session::Tailnet &tailnet, End &end, Local &local, std::string &err) {
    session::Who who;
    net::QuicLinks::Config links;
    if (!tailnet.Self(who, links.listen)) return false;
    links.port = args.port;
    links.mayCall = [&tailnet](const std::string &address) { return tailnet.MayCall(address); };
    end.links = net::QuicLinks::Open(links, err);
    if (!end.links) return false;
    session::Config config;
    config.name = args.name.empty() ? who.name : args.name, config.port = args.port, config.dice = Dice;
    config.watched = [&end](int link, const proto::Watch &watch, double now) { end.video->Watch(link, watch, now); };
    config.wanted = [&end](int link, const proto::Want &want, double now) { end.looks->Wanted(link, want, now); };
    end.session = std::make_unique<session::Session>(*end.links, tailnet, config);
    end.wires = std::make_unique<StreamWires>(*end.links);
    end.video = std::make_unique<flow::Video>(*end.session, *end.wires, local.media);
    end.looks = std::make_unique<flow::Looks>(*end.session, *end.wires, local.wardrobe);
    end.here = links.listen + " " + std::to_string(args.port) + " " + config.name;
    std::printf("ft-together: %s (%s) at %s port %d\n", config.name.c_str(), who.login.c_str(), links.listen.c_str(), int(args.port));
    return true;
}

// A datagram from a link: a pose from someone in the room goes to this Frame's watchers, their
// voice to its listeners, and anything else is dropped.
void Datagram(int sock, const End &end, Local &local, const net::Event &e, double now) {
    const int person = end.session->PersonOn(e.link);
    if (person < 0 || e.bytes.empty()) return;
    const uint8_t *data = e.bytes.data();
    const size_t n = e.bytes.size();
    if (data[0] == proto::kVoiceDatagram) {
        if (local.voices.Take(e.link, data, n, now)) local.listeners.Send(sock, person, data, n, now);
    } else if (local.gate.Take(e.link, data, n, now)) {
        local.watchers.Send(sock, person, data, n, now);
    }
}

// What a link saw. A picture goes to the video's rules, a file to the avatars'.
void Tell(int sock, End &end, Local &local, const net::Event &e, double now) {
    session::Session &session = *end.session;
    switch (e.kind) {
    case net::Event::kUp: session.Up(e.link, e.address, now); break;
    case net::Event::kBytes: session.Bytes(e.link, e.bytes.data(), e.bytes.size(), now); break;
    case net::Event::kDown:
        session.Down(e.link, now);
        local.gate.Forget(e.link);
        local.voices.Forget(e.link);
        end.looks->Down(e.link);
        break;
    case net::Event::kDatagram: Datagram(sock, end, local, e, now); break;
    case net::Event::kFrame: end.video->Frame(e.link, e.bytes.data(), e.bytes.size(), now); break;
    case net::Event::kFrameLost: end.video->Lost(e.link, e.tag, now); break;
    case net::Event::kFile: end.looks->File(e.link, e.bytes, now); break;
    }
}

// How the shared panels' video goes: {"sources":[...],"sinks":[...],"bitrate":[...],"viewers":[...],"dropped":N}.
std::string MediaState(const End &end, const Local &local) {
    std::string state = local.media.State(), rates, viewers;
    for (int number = 0; number < room::kFramePanels; ++number) {
        rates += (number ? "," : "") + std::to_string(end.video->bitrate(number));
        viewers += (number ? "," : "") + std::to_string(end.video->viewers(number));
    }
    state.pop_back();
    return state + ",\"bitrate\":[" + rates + "],\"viewers\":[" + viewers + "],\"dropped\":" + std::to_string(end.video->dropped()) + "}";
}

// The wearer's own pose, from one of this Frame's programs, to everyone in the room, with their
// mouth as the program that hears them has it.
void Mine(End &end, Local &local, const uint8_t *data, size_t n, double now) {
    std::vector<uint8_t> out;
    if (!end.inRoom() || !local.count.Stamp(data, n, local.mouth, now, out)) return;
    for (int link : end.session->MemberLinks()) end.links->Datagram(link, out);
}

// The wearer's own voice, from this Frame's program that hears them, to everyone in the room.
void MyVoice(End &end, const uint8_t *data, size_t n) {
    proto::Voice voice;
    if (!end.inRoom() || !proto::ReadVoice(data, n, voice)) return;
    const std::vector<uint8_t> out(data, data + n);
    for (int link : end.session->MemberLinks()) end.links->Datagram(link, out);
}

// The avatar the wearer has on, from the program that draws them. The room hears of it only if
// it is one the others may have.
std::string Wear(End &end, Local &local, const std::string &name, double now) {
    proto::Look look;
    const std::string whyNot = local.wardrobe.Wear(name, look);
    end.session->Wear(look, now);
    return whyNot.empty() ? "ok" : "error " + whyNot;
}

// One datagram from the local socket seen to, if one waits: a question answered, a pose or a
// voice sent on, or the wearer's mouth kept for their poses.
bool Answer(int sock, End &end, Local &local, double now) {
    char text[kMostQuestion + 1];
    sockaddr_un from;
    socklen_t fromLen = sizeof from;
    const ssize_t n = recvfrom(sock, text, sizeof text, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (n < 0) return false;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text);
    if (n > 0 && bytes[0] == proto::kPoseDatagram) return Mine(end, local, bytes, size_t(n), now), true;
    if (n > 0 && bytes[0] == proto::kVoiceDatagram) return MyVoice(end, bytes, size_t(n)), true;
    if (n > 0 && bytes[0] == local::kMouthToTogether) return local.mouth.Take(bytes, size_t(n), now), true;
    const std::string asked(text, size_t(n));
    std::string reply;
    if (size_t(n) > kMostQuestion) reply = "error that is too long";
    else if (!end.session) reply = "error Tailscale isn't running on this headset";
    else if (asked == "watch") reply = local.watchers.Add(from, fromLen, now) ? "ok" : "error too many programs are watching";
    else if (asked == "listen") reply = local.listeners.Add(from, fromLen, now) ? "ok" : "error too many programs are listening";
    else if (asked == "floor" || asked.rfind("floor ", 0) == 0) reply = local.floor.Ask(asked, end.inRoom());
    else if (asked == "media") reply = "ok " + MediaState(end, local);
    else if (asked == "here") reply = "ok " + end.here;
    else if (asked == "wear" || asked.rfind("wear ", 0) == 0) reply = Wear(end, local, asked.size() > 5 ? asked.substr(5) : "", now);
    else if (asked == "looks") reply = "ok " + local.wardrobe.State(end.session->room(), end.session->me());
    else reply = session::Command(*end.session, asked, now);
    if (!end.inRoom()) local.floor.Lift();   // the room was left: the next one's floor is its own
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
    Local local(args.socket);   // (after the socket was bound: no other ft-together has this name's folder)
    if (!local.media.Listen(args.socket + "_media"))
        return std::fprintf(stderr, "ft-together: listen @%s_media: %s\n", args.socket.c_str(), std::strerror(errno)), 1;
    std::vector<pollfd> fds;
    double tried = -1e9, ticked = Now();
    bool waiting = false;
    std::string note;
    while (!g_stop) {
        double now = Now();
        if (!end.session && now - tried >= kRetryS) {
            tried = now;
            std::string err;
            if (!Start(args, *tailnet, end, local, err)) {
                if (!err.empty()) return std::fprintf(stderr, "ft-together: %s\n", err.c_str()), 1;
                if (!waiting) std::printf("ft-together: waiting for Tailscale\n");
                waiting = true;
            }
        }
        fds.assign({{sock, POLLIN, 0}, {end.links ? end.links->fd() : -1, POLLIN, 0}});
        local.media.Fds(fds);
        // pictures on their way are given up in time only if the links are looked at often
        const int wait = end.links && end.links->Sending() ? kSendingMs : local.media.clients() ? kMediaMs : 1000;
        if (poll(fds.data(), fds.size(), wait) < 0 && errno != EINTR) return std::perror("ft-together: poll"), 1;
        now = Now();
        if (end.session) {
            net::Event e;
            for (int i = 0; i < kMostAtOnce && end.links->Next(e); ++i) Tell(sock, end, local, e, now);
            if (!end.inRoom()) local.floor.Lift();
        }
        for (int i = 0; i < kMostAtOnce && Answer(sock, end, local, now); ++i) {}
        local.media.Serve(end.video.get(), now);
        if (end.session) {
            end.links->Tick();
            end.video->Tick(now);
        }
        if (end.session && now - ticked >= 1) {
            ticked = now;
            end.session->Tick(now);
            end.looks->Tick(now);
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
