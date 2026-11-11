// ft-peerpanel: shows one panel someone else shares with a Together room (docs/together.md,
// together/README.md). ft-together's media socket hands it the panel's encoded pictures
// (local/mediawire.h), in order and none missing since the last keyframe. They go through the
// Frame's hardware decoder and a GPU pass (stream/iris.h, as a remote display's do) into a ring
// of three RGBA buffers, which ft-screens shows as a panel: it starts this program as it starts
// ft-stream (screens/remote.c), with a SOCK_SEQPACKET socket as --fd, and speaks the same words:
//
//   from ft-screens: "modifiers M ..." first (what SteamVR imports), then "release I",
//                    "attention hidden|view|focused", "quit"; a remote display's input words
//                    (move, button, key ...) are taken and dropped: nothing goes to a peer's
//                    panel yet
//   to ft-screens:   "state connecting|live|lost ...", "buffers W H FORMAT MODIFIER OFFSET
//                    STRIDE" with the ring's three fds, "frame I"
//
// How much of the panel its viewer sees goes on to ft-together, so that a panel nobody looks at
// isn't sent, and so does word that the decoder lost its place and needs a keyframe.
//
//   ft-peerpanel --panel ID [--socket NAME] (--fd N | --probe [--frames N] [--seconds S])
//
// --panel: the panel's id in the room. --probe: no ft-screens; every picture decoded is read
// back as ft-sharepanel --pattern's test picture (media/testpattern.h) and a line printed:
// "picture <count> <W>x<H> number <n> light <v> dark <v> grey <v>". It ends when ft-together or
// ft-screens goes; ft-screens starts it again.
#include "local/mediaclient.h"
#include "media/testpattern.h"
#include "media/words.h"

#include "../stream/iris.h"

#include <sys/prctl.h>

namespace {

constexpr size_t kMostWaiting = 30;       // pictures waiting for the decoder; past that it has lost its place
constexpr int64_t kStuckNs = 1'000'000'000;   // the decoder took nothing for this long: the same

struct Args {
    int panel = -1, fd = -1, frames = 0, seconds = 0;
    bool probe = false;
    std::string socket = FT_SOCK_TOGETHER;
};

bool ReadArgs(int argc, char **argv, Args &a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (k == "--probe") a.probe = true;
        else if (k == "--panel" && v) a.panel = std::atoi(v), ++i;
        else if (k == "--fd" && v) a.fd = std::atoi(v), ++i;
        else if (k == "--frames" && v) a.frames = std::atoi(v), ++i;
        else if (k == "--seconds" && v) a.seconds = std::atoi(v), ++i;
        else if (k == "--socket" && v && *v) a.socket = v, ++i;
        else return false;
    }
    return a.panel >= 0 && a.panel <= 255 && (a.probe != (a.fd >= 0));
}

// ft-screens' end, or nobody's (--probe).
struct Screens {
    int fd = -1;

    bool Say(const std::string &msg, const int *fds = nullptr, int nfds = 0) const { return fd < 0 || media::SendWords(fd, msg, fds, nfds); }
};

// The panel's pictures on their way to the decoder, and what ft-together hears of them.
struct Incoming {
    local::MediaClient &together;
    std::deque<std::vector<uint8_t>> units;
    int level = 0;   // of its viewer: 0 hidden, 1 in view, 2 focused (mediawire.h)
    bool needKey = false;
    int64_t stuckSince = -1;

    void Tell(bool fresh) {
        std::vector<uint8_t> out;
        local::PutMedia(out, local::kMediaAttention, uint8_t(level), uint8_t(fresh ? 1 : 0));
        together.Send(std::move(out));
    }
    // The decoder can't go on from what it has: everything waiting goes, and a keyframe is asked for.
    void Lost(const char *why) {
        std::printf("the decoder lost its place (%s): asking for a keyframe\n", why);
        units.clear(), needKey = true, stuckSince = -1;
        Tell(true);
    }
    void Take(uint8_t kind, std::vector<uint8_t> &body) {
        if (kind != local::kMediaUnit || body.size() < 2) return;
        const bool key = body[0] & 1;
        if (needKey && !key) return;
        needKey = false;
        body.erase(body.begin());
        units.push_back(std::move(body));
        if (units.size() > kMostWaiting) Lost("too many pictures wait");
    }
};

// Reads the test picture's squares back from the ring buffer just drawn (still bound).
void Probe(const Converter &conv, int count) {
    const int width = int(conv.width), side = media::testpattern::Square(width);
    std::vector<uint8_t> rows(size_t(width) * size_t(side) * 4);
    glReadPixels(0, 0, width, side, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    const auto at = [&](int square) { return int(rows[(size_t(side / 2) * size_t(width) + size_t(square * side + side / 2)) * 4]); };
    std::printf("picture %d %ux%u number %d light %d dark %d grey %d\n", count, conv.width, conv.height,
                media::testpattern::Read(rows.data(), width * 4, width), at(0), at(1), at(media::testpattern::kSquares + 1));
}

int Run(const Args &args) {
    Screens screens{args.fd};
    local::MediaClient together(local::kMostMedia);
    if (!together.Connect(args.socket + "_media", local::kMediaSink, uint8_t(args.panel))) {
        std::printf("no ft-together on @%s_media\n", args.socket.c_str());
        return screens.Say("state lost no ft-together"), 1;
    }

    Converter conv;
    conv.toSteamVR = false;
    conv.range = EGL_YUV_FULL_RANGE_EXT;   // what the Frame's encoder makes of RGBA (docs/together.md, T2)
    Incoming in{together};
    if (args.probe) {
        in.level = 2;
    } else {
        // ft-screens' first word: what SteamVR imports.
        pollfd p = {screens.fd, POLLIN, 0};
        std::string msg;
        int fds[media::kMostWordFds], nfds = 0;
        if (poll(&p, 1, 5000) != 1 || media::TakeWords(screens.fd, msg, fds, nfds) != 1 || msg.rfind("modifiers", 0) != 0)
            return std::printf("ft-screens didn't say what SteamVR imports\n"), 1;
        char *at = &msg[9];
        for (char *end = nullptr;; at = end) {
            const uint64_t m = std::strtoull(at, &end, 16);
            if (end == at) break;
            conv.modifiers.push_back(m);
        }
    }
    Decoder dec;
    dec.toSteamVR = false;
    if (!dec.Open("/dev/video-dec0", 6) || !conv.Init()) return screens.Say("state lost no decoder"), 1;
    screens.Say("state connecting");
    in.Tell(false);
    std::printf("panel %d: waiting for its pictures\n", args.panel);

    bool owned[3] = {};   // ring buffers ft-screens shows or is about to
    bool live = false;
    uint64_t seq = 0;
    int shown = 0, dropped = 0, lastSent = -1, total = 0;
    int64_t lastReport = MonoNs(), allHeldSince = -1;
    const int64_t until = args.seconds > 0 ? MonoNs() + int64_t(args.seconds) * 1'000'000'000 : 0;
    int code = 0;
    while (!g_stop) {
        pollfd p[3] = {{dec.fd, short(POLLIN | POLLPRI | (in.units.empty() ? 0 : POLLOUT)), 0}, together.Poll(), {screens.fd, POLLIN, 0}};
        if (poll(p, 3, 100) > 0 && (p[0].revents & POLLERR)) usleep(1000);

        if (!together.Read()) {
            std::printf("ft-together went\n");
            code = 1;
            break;
        }
        uint8_t kind;
        std::vector<uint8_t> body;
        while (together.Next(kind, body)) in.Take(kind, body);

        bool open = true;
        while (screens.fd >= 0) {
            std::string msg;
            int fds[media::kMostWordFds], nfds = 0, i = 0;
            const int got = media::TakeWords(screens.fd, msg, fds, nfds);
            for (int k = 0; k < nfds; ++k) close(fds[k]);
            if (got < 0) open = false;
            if (got <= 0) break;
            if (std::sscanf(msg.c_str(), "release %d", &i) == 1) {
                if (i >= 0 && i < 3) owned[i] = false;
            } else if (msg.rfind("attention ", 0) == 0) {
                const int level = msg == "attention hidden" ? 0 : msg == "attention focused" ? 2 : 1;
                if (level != in.level) in.level = level, in.Tell(false);
            } else if (msg == "quit") {
                g_stop = 1;
            }
        }
        if (!open) break;   // ft-screens closed its end

        if (dec.Events()) {
            if (!conv.Setup(dec)) break;
            const int fds[3] = {conv.out[0].fd, conv.out[1].fd, conv.out[2].fd};
            char b[160];
            std::snprintf(b, sizeof b, "buffers %u %u %x %llx %u %u", conv.width, conv.height, DRM_FORMAT_ABGR8888,
                          (unsigned long long)conv.out[0].modifier, conv.out[0].offset, conv.out[0].stride);
            if (!screens.Say(b, fds, 3)) break;
            for (bool &o : owned) o = false;
        }
        dec.Reclaim();
        while (!in.units.empty() && dec.Feed(in.units.front().data(), in.units.front().size(), seq)) in.units.pop_front(), ++seq, in.stuckSince = -1;
        if (!in.units.empty()) {
            if (in.stuckSince < 0) in.stuckSince = MonoNs();
            else if (MonoNs() - in.stuckSince > kStuckNs) in.Lost("it takes no more");
        }

        uint64_t frame = 0;
        bool last = false;
        int i;
        while ((i = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t at0 = MonoNs();
            int at = -1;
            for (int k = 0; k < 3 && at < 0; ++k)
                if (!owned[(conv.next + k) % 3]) at = (conv.next + k) % 3;
            if (at < 0) {   // ft-screens still has all three: this picture is skipped
                dec.Requeue(i);
                ++dropped;
                // ft-screens hands one back with every new picture it shows. If none comes back for
                // this long, they were lost: all but the one it shows are ours again.
                if (allHeldSince < 0) allHeldSince = at0;
                else if (at0 - allHeldSince > 2'000'000'000) {
                    std::printf("ring: no buffer back from ft-screens for 2 s, taking them back\n");
                    for (int k = 0; k < 3; ++k) owned[k] = k == lastSent;
                    allHeldSince = -1;
                }
                continue;
            }
            allHeldSince = -1;
            conv.ConvertInto(i, at);
            conv.next = (at + 1) % 3;
            dec.Requeue(i);
            ++shown, ++total;
            if (args.probe) {
                Probe(conv, total);
            } else {
                owned[at] = true, lastSent = at;
                if (!screens.Say("frame " + std::to_string(at))) g_stop = 1;
            }
            if (!live) live = true, screens.Say("state live");
        }
        if (!together.Flush()) {
            std::printf("ft-together went\n");
            code = 1;
            break;
        }
        const int64_t now = MonoNs();
        if (now - lastReport >= 10'000'000'000) {
            if (shown || dropped)
                std::printf("shown %.1f pictures a second (%d dropped), convert %.1f ms\n", shown / ((now - lastReport) / 1e9), dropped,
                            shown ? conv.msSum / shown : 0.0);
            conv.msSum = 0, shown = dropped = 0, lastReport = now;
        }
        if ((args.frames > 0 && total >= args.frames) || (until && now >= until)) break;
    }
    screens.Say(code ? "state lost ft-together went" : "state lost");
    dec.Close();
    return code;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Args args;
    if (!ReadArgs(argc, argv, args)) {
        std::fprintf(stderr, "usage: ft-peerpanel --panel ID [--socket NAME] (--fd N | --probe [--frames N] [--seconds S])\n");
        return 2;
    }
    // ft-screens blocks SIGTERM for its own loop, and a child inherits that.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    if (!args.probe) prctl(PR_SET_PDEATHSIG, SIGTERM);   // ft-screens went: so does this
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);
    return Run(args);
}
