// ft-roomvoice: the voices of a Together room on this Frame (docs/together.md, "Sound"). While
// ft-together says this Frame is in a room, it sends the wearer's voice to the others and
// plays theirs, each from where their head is in the room.
//
//   ft-roomvoice [--socket NAME] [--together NAME] [--screens NAME] [--muted]
//                [--hum HZ] [--probe]
//
//   --socket NAME     its own socket's name (ft_roomvoice)
//   --together NAME   ft-together's (ft_together)
//   --screens NAME    ft-screens' (ft_screens): asked where the wearer's head is
//   --muted           start with the microphone muted
//   --hum HZ          for tests: no microphone, the wearer hums at HZ
//   --probe           for tests: no loudspeakers, what would be played is measured (`state`)
//
// Questions, one datagram each on its socket:
//
//   mute, unmute   the wearer's own voice
//   state          -> "ok " and {"room", "muted", "mic", "sent", "level", "playing", "voices"}
//   quit
//
// The microphone is open only in a room, unmuted, with the headset on the wearer's head: away
// in a game the voice stays on, with the headset off it doesn't. The others' voices come as
// datagrams from ft-together (`listen`), their heads' places with the poses it passes
// (`watch`), and the wearer's own floor in the room from it too; where the wearer's head is on
// that floor is ft-screens' to say. Someone whose place isn't known is heard in the middle.
//
// What a peer sends is decoded here and nowhere else (voice/codec.h).
#include "../avatar/room/client.h"
#include "../conf/ftsock.h"
#include "voice/headask.h"
#include "voice/player.h"
#include "voice/pretend.h"
#include "voice/sender.h"
#include "voice/voices.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>

namespace {

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

struct Args {
    std::string socket = FT_SOCK_ROOMVOICE, together = FT_SOCK_TOGETHER, screens = FT_SOCK_SCREENS;
    bool muted = false, probe = false;
    double hum = 0;
};

bool Read(int argc, char **argv, Args &args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--muted") args.muted = true;
        else if (a == "--probe") args.probe = true;
        else if (a == "--socket" && next) args.socket = next, ++i;
        else if (a == "--together" && next) args.together = next, ++i;
        else if (a == "--screens" && next) args.screens = next, ++i;
        else if (a == "--hum" && next && (args.hum = std::atof(next)) >= 50 && args.hum <= 2000) ++i;
        else return false;
    }
    return true;
}

// The others' heads and the wearer's own, told to the mix as the room has them now.
void Place(voice::Voices &voices, const roomclient::Client &client, const voice::HeadAsk &head, unsigned since[], double now) {
    const roompeople::People &people = client.people();
    for (int place = 0; place < proto::kPeople; ++place) {
        const roompeople::Person &p = people.person(place);
        const bool other = people.inRoom() && p.here && place != people.me();
        if (!other || p.since != since[place]) voices.Forget(place);   // gone, or someone else has their place now
        since[place] = p.since;
        if (other) voices.Place(place, people.Drawn(place, now), p.pose.eyes);
    }
    const bool known = people.inRoom() && client.laid() && head.known(now);
    double ear[3] = {};
    if (known) {
        const avatar::Vec at = roomfloor::ToRoom(client.floor(), {head.at()[0], head.at()[1], head.at()[2]});
        ear[0] = at.x, ear[1] = at.y, ear[2] = at.z;
    }
    voices.Listener(known, ear, head.yaw() + client.floor().yaw);
}

std::string State(const roomclient::Client &client, voice::Voices &voices, const voice::Sender &sender, const voice::Player &player, bool muted,
                  const std::shared_ptr<voice::Probed> &probed) {
    char text[160];
    std::snprintf(text, sizeof text, "{\"room\":%s,\"muted\":%s,\"mic\":\"%s\",\"sent\":%u,\"level\":%.3f,\"playing\":%s,\"voices\":[", client.people().inRoom() ? "true" : "false",
                  muted ? "true" : "false", sender.open() ? "open" : sender.failed() ? "failed" : "closed", sender.sent(), double(sender.level()),
                  player.open() ? "true" : "false");
    std::string out = text;
    bool first = true;
    for (int place = 0; place < proto::kPeople; ++place) {
        const roompeople::Person &p = client.people().person(place);
        if (!client.people().inRoom() || !p.here || place == client.people().me()) continue;
        const voice::Voices::Told t = voices.told(place);
        std::snprintf(text, sizeof text, "%s{\"person\":%d,\"speaking\":%s,\"placed\":%s,\"level\":%.3f,\"played\":%u,\"lost\":%u,\"late\":%u}", first ? "" : ",", place,
                      t.speaking ? "true" : "false", t.placed ? "true" : "false", double(t.level), t.played, t.lost, t.late);
        out += text, first = false;
    }
    out += "]";
    if (probed) {
        std::lock_guard<std::mutex> lock(probed->mu);
        std::snprintf(text, sizeof text, ",\"out\":{\"left\":%.4f,\"right\":%.4f,\"hz\":%.1f,\"frames\":%u}", double(probed->left), double(probed->right), double(probed->hz),
                      probed->frames);
        out += text;
    }
    return out + "}";
}

}  // namespace

int main(int argc, char **argv) {
    Args args;
    if (!Read(argc, argv, args))
        return std::fprintf(stderr, "usage: ft-roomvoice [--socket NAME] [--together NAME] [--screens NAME] [--muted] [--hum HZ] [--probe]\n"), 2;
    struct sigaction stop = {};
    stop.sa_handler = Stop;   // no SA_RESTART: poll comes back, and the loop ends
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    roomclient::Client client;
    voice::HeadAsk head;
    std::string err;
    if (!client.Open(args.socket, args.together, true, err)) return std::fprintf(stderr, "ft-roomvoice: %s\n", err.c_str()), 1;
    if (!head.Open(args.screens)) return std::fprintf(stderr, "ft-roomvoice: @%s isn't a socket's name\n", args.screens.c_str()), 1;

    const auto voices = std::make_shared<voice::Voices>();
    client.Listen([&voices](int person, const uint8_t *datagram, size_t n) { voices->Heard(person, datagram, n); });
    const std::shared_ptr<voice::Probed> probed = args.probe ? std::make_shared<voice::Probed>() : nullptr;
    voice::Sender sender;
    voice::Player player;
    const voice::OpenSource mic = args.hum > 0 ? voice::OpenSource([hz = args.hum](std::string &) { return std::make_unique<voice::Hum>(hz); }) : voice::Microphone;
    const voice::OpenSink out = probed ? voice::OpenSink([probed](std::string &) { return std::make_unique<voice::Probe>(probed); }) : voice::Loudspeakers;
    if (!sender.Start(mic, args.together)) return std::fprintf(stderr, "ft-roomvoice: @%s isn't a socket's name\n", args.together.c_str()), 1;
    player.Start(out, voices);

    bool muted = args.muted, inRoom = false, micOpen = false;
    unsigned since[proto::kPeople] = {};
    while (!g_stop) {
        pollfd fds[2] = {{client.fd(), POLLIN, 0}, {head.fd(), POLLIN, 0}};
        if (poll(fds, 2, 20) < 0 && errno != EINTR) return std::perror("ft-roomvoice: poll"), 1;
        const double now = Now();
        head.Tick(now);
        const double stands[3] = {head.at()[0], head.at()[2], head.yaw()};
        client.Tick(now, head.known(now) ? stands : nullptr);

        const roompeople::People &people = client.people();
        const bool worn = people.inRoom() && !(people.person(people.me()).away & proto::kAwayOff);
        sender.Want(worn && !muted);
        player.Want(worn);
        Place(*voices, client, head, since, now);
        if (people.inRoom() != inRoom) inRoom = people.inRoom(), std::printf("ft-roomvoice: %s\n", inRoom ? "in a room" : "in no room");
        if (sender.open() != micOpen) micOpen = sender.open(), std::printf("ft-roomvoice: the microphone is %s\n", micOpen ? "open" : "closed");

        roomclient::Question q;
        while (client.Next(q)) {
            if (q.text == "state") client.Reply(q, "ok " + State(client, *voices, sender, player, muted, probed));
            else if (q.text == "mute" || q.text == "unmute") muted = q.text == "mute", client.Reply(q, "ok");
            else if (q.text == "quit") client.Reply(q, "ok"), g_stop = 1;
            else client.Reply(q, "error state|mute|unmute|quit");
        }
    }
    sender.Stop();
    player.Stop();
    return 0;
}
