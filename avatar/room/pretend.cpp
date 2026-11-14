// Someone in a shared room who isn't there (pretend.h).
#include "pretend.h"

#include "../view/demo.h"
#include "client.h"
#include "wire.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace pretend {

namespace {

constexpr double kTall = 1.6, kAhead = -0.08;   // the pretended wearer's eyes: this high, and this far in front of the middle of them
constexpr double kSendS = 1.0 / 30;

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

}  // namespace

int Run(const Ask &ask) {
    wearer::Seen seen;
    if (!demo::Wearer(ask.pose.c_str(), 0, kTall, kAhead, seen)) return std::fprintf(stderr, "avatars: no pose called %s (%s)\n", ask.pose.c_str(), demo::kNames), 1;
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    roomclient::Client client;
    std::string err;
    if (!client.Open("ft_avatars_pretend_" + std::to_string(getpid()), ask.together, false, err)) return std::fprintf(stderr, "avatars: %s\n", err.c_str()), 1;

    const auto start = std::chrono::steady_clock::now();
    double lastStatus = 0;
    int sent = 0, refused = 0;
    for (int step = 0; !g_stop; ++step) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (ask.seconds > 0 && t >= ask.seconds) break;
        demo::Wearer(ask.pose.c_str(), t, kTall, kAhead, seen);
        const double stands[3] = {seen.eyes.x, seen.eyes.z, roomfloor::Facing(seen.head * avatar::Vec{0, 0, -1})};
        client.Tick(t, stands);
        if (client.people().inRoom() && client.laid()) (client.Send(roomwire::ToRoom(seen, roomfloor::Then(client.floor(), ask.walked))) ? sent : refused) += 1;
        if (ask.status > 0 && t - lastStatus >= ask.status) {
            const roompeople::People &people = client.people();
            if (!people.inRoom()) std::printf("pretending to %s: not in a room\n", ask.pose.c_str());
            else
                std::printf("pretending to %s: in a room as %s, floor %s; %.1f poses/s out%s\n", ask.pose.c_str(), people.person(people.me()).name.c_str(),
                            client.laid() ? "laid" : "not laid yet", sent / (t - lastStatus), refused ? ", some not taken" : "");
            sent = refused = 0, lastStatus = t;
        }
        std::this_thread::sleep_until(start + std::chrono::duration<double>((step + 1) * kSendS));
    }
    client.Close();
    return 0;
}

}  // namespace pretend
