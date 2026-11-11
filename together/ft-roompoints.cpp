// ft-roompoints: where the others in a Together room point, drawn on this Frame (docs/together.md,
// "Control and moving": everyone sees everyone's pointer, in its owner's colour). While someone
// points at a panel that is shown here, a small mark in their place's colour is on it where they
// point: a ring, and a whole disc while they hold a button down.
//
//   ft-roompoints [--socket NAME] [--together NAME] [--panels NAME] [--probe]
//
//   --socket NAME     its own socket's name (ft_roompoints)
//   --together NAME   ft-together's (ft_together): asked for the others' pointers
//   --panels NAME     ft-roompanels' (ft_roompanels): asked which panel here shows each of the room's
//   --probe           for tests: no SteamVR, what would be drawn is told (`state`)
//
// Questions, one datagram each on its socket:
//
//   state   -> "ok " and {"panels", "heard", "marks": [{"person", "panel", "key", "x", "y", "pressed", "drawn"}]}
//   quit
//
// Your own pointer isn't drawn: ft-together sends this Frame only the others'. Where someone
// points says nothing of whether they may click there (proto/control.h), and nothing a peer
// sends is drawn but a disc: the datagram's numbers are bounded as they are read
// (proto/pointwire.h), and a panel this Frame doesn't show has no mark.
#include "../conf/ftsock.h"
#include "points/asker.h"
#include "points/dots.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <memory>
#include <poll.h>
#include <string>

namespace {

constexpr double kFrameS = 1.0 / 60;   // between drawings while a mark shows
constexpr int kIdleMs = 250;           // the loop's sleep while none does

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

struct Args {
    std::string socket = FT_SOCK_ROOMPOINTS, together = FT_SOCK_TOGETHER, panels = FT_SOCK_ROOMPANELS;
    bool probe = false;
};

bool Read(int argc, char **argv, Args &args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--probe") args.probe = true;
        else if (a == "--socket" && next) args.socket = next, ++i;
        else if (a == "--together" && next) args.together = next, ++i;
        else if (a == "--panels" && next) args.panels = next, ++i;
        else return false;
    }
    return true;
}

std::string State(const points::Marks &marks, const points::PanelKeys &keys, const bool drawn[], unsigned heard, double now) {
    int known = 0;
    for (int id = 0; id < points::kPanels; ++id) known += keys.of(id).known;
    char text[224];
    std::snprintf(text, sizeof text, "{\"panels\":%d,\"heard\":%u,\"marks\":[", known, heard);
    std::string out = text;
    bool first = true;
    for (int place = 0; place < proto::kPeople; ++place) {
        const points::Mark m = marks.At(place, now);
        if (!m.shown) continue;
        std::snprintf(text, sizeof text, "%s{\"person\":%d,\"panel\":%d,\"key\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"pressed\":%s,\"drawn\":%s}", first ? "" : ",", place, int(m.panel),
                      keys.of(m.panel).key.c_str(), m.x, m.y, m.pressed ? "true" : "false", drawn[place] ? "true" : "false");
        out += text, first = false;
    }
    return out + "]}";
}

}  // namespace

int main(int argc, char **argv) {
    Args args;
    if (!Read(argc, argv, args)) return std::fprintf(stderr, "usage: ft-roompoints [--socket NAME] [--together NAME] [--panels NAME] [--probe]\n"), 2;
    struct sigaction stop = {};
    stop.sa_handler = Stop;   // no SA_RESTART: poll comes back, and the loop ends
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    points::Asker asker;
    std::string err;
    if (!asker.Open(args.socket, args.together, args.panels, err)) return std::fprintf(stderr, "ft-roompoints: %s\n", err.c_str()), 1;
    points::Marks marks;
    points::PanelKeys keys;
    std::unique_ptr<points::Drawer> drawer;
    if (args.probe) drawer = std::make_unique<points::NoDrawer>();
    else drawer = std::make_unique<points::Dots>();

    bool drawn[proto::kPeople] = {}, any = false;
    double drew = -1e9;
    while (!g_stop) {
        const double left = drew + kFrameS - Now();
        pollfd fd = {asker.fd(), POLLIN, 0};
        if (poll(&fd, 1, any ? (left > 0 ? int(left * 1000) + 1 : 0) : kIdleMs) < 0 && errno != EINTR) return std::perror("ft-roompoints: poll"), 1;
        const double now = Now();
        asker.Tick(now, marks, keys);
        if (!any || now - drew >= kFrameS) {
            drew = now, any = false;
            drawer->Tick(now);
            for (int place = 0; place < proto::kPeople; ++place) {
                const points::Mark m = marks.At(place, now);
                const points::PanelKey &panel = keys.of(m.panel);
                drawn[place] = m.shown && panel.known && drawer->Show(place, panel, m, now);
                if (!drawn[place]) drawer->Hide(place);
                any |= m.shown;
            }
        }
        points::Question q;
        while (asker.Next(q)) {
            if (q.text == "state") asker.Reply(q, "ok " + State(marks, keys, drawn, asker.pointsIn(), now));
            else if (q.text == "quit") asker.Reply(q, "ok"), g_stop = 1;
            else asker.Reply(q, "error state|quit");
        }
    }
    return 0;
}
