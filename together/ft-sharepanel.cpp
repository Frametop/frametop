// ft-sharepanel: encodes one panel this Frame shares with a Together room (docs/together.md,
// together/README.md). Its pictures come from ft-screens, which starts it for a shared screen
// (screens/share.c, media/screenfeed.h), or are a test picture (--pattern). They go through the
// Frame's hardware encoder (media/irisenc.h) and, encoded, to ft-together's media socket
// (local/mediawire.h), which sends them to whoever watches the panel and says back what to
// encode: at how many bits and pictures a second (--fps at the most, fewer while nobody has
// the panel focused), when a keyframe is needed, and whether anyone watches.
// While nobody does, nothing is drawn or encoded, and after a few seconds the encoder is closed.
//
//   ft-sharepanel --panel K --size WxH [--fps F] [--socket NAME] (--fd N | --pattern)
//
// --panel: the panel's number among this Frame's (0 to 2). --size: the stream's pixels, even; a
// screen of another size is scaled to it. --socket: ft-together's (its media socket is
// NAME_media). --fd: the socket to ft-screens. It ends when ft-together or ft-screens goes; the
// one who started it starts it again.
#include "local/mediaclient.h"
#include "media/panelenc.h"
#include "media/patternfeed.h"
#include "media/screenfeed.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <poll.h>
#include <sys/prctl.h>
#include <time.h>

namespace {

constexpr double kLinger = 3;        // seconds the encoder stays open with nobody watching
constexpr double kKeyWait = 0.05;    // a keyframe asked for waits this long for the screen to change, then is made of the last picture
constexpr double kRetry = 2;         // between tries to open an encoder that wouldn't
constexpr double kReport = 10;

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

struct Args {
    int panel = -1, width = 0, height = 0, fps = 30, fd = -1;
    bool pattern = false;
    std::string socket = FT_SOCK_TOGETHER;
};

bool Read(int argc, char **argv, Args &a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (k == "--pattern") a.pattern = true;
        else if (k == "--panel" && v) a.panel = std::atoi(v), ++i;
        else if (k == "--size" && v && std::sscanf(v, "%dx%d", &a.width, &a.height) == 2) ++i;
        else if (k == "--fps" && v) a.fps = std::atoi(v), ++i;
        else if (k == "--fd" && v) a.fd = std::atoi(v), ++i;
        else if (k == "--socket" && v && *v) a.socket = v, ++i;
        else return false;
    }
    return a.panel >= 0 && a.panel <= 2 && a.width >= 128 && a.height >= 128 && a.width <= 4096 && a.height <= 4096 &&
           !((a.width | a.height) & 1) && a.fps >= 1 && a.fps <= 60 && (a.pattern != (a.fd >= 0));
}

// What ft-together last said to encode.
struct Asked {
    int bitrate = 2000000, pictures = 0;   // (pictures: --fps until it says)
    bool watched = false, key = false;
    double keyAt = 0, unwatchedAt = 0;
};

struct Counts {
    int pictures = 0, units = 0, keys = 0, dropped = 0;
    size_t bytes = 0;
};

// The encoded pictures that wait, to ft-together. One it can't take (too long, or too much
// already waits there) is dropped, and nothing follows it but a keyframe.
void Hand(media::PanelEncoder &enc, local::MediaClient &together, Asked &asked, bool &skipToKey, Counts &n) {
    irisenc::Unit u;
    while (enc.Next(u)) {
        if (skipToKey && !u.key) {
            ++n.dropped;
            continue;
        }
        std::vector<uint8_t> bytes;
        if (u.size <= local::kMostMediaUnit) local::PutMediaUnit(bytes, u.key, u.data, u.size);
        if (bytes.empty() || !together.Send(std::move(bytes))) {
            ++n.dropped, skipToKey = true;
            if (!asked.key) asked.key = true, asked.keyAt = Now();
            continue;
        }
        skipToKey = false;
        ++n.units, n.keys += u.key, n.bytes += u.size;
    }
}

int Run(const Args &args, media::Feed &feed, const std::function<void(const std::string &)> &state) {
    local::MediaClient together(64);
    if (!together.Connect(args.socket + "_media", local::kMediaSource, uint8_t(args.panel))) {
        std::printf("no ft-together on @%s_media\n", args.socket.c_str());
        return state("lost no ft-together"), 1;
    }
    std::printf("panel %d, %dx%d at %d pictures a second: waiting for someone to watch\n", args.panel, args.width, args.height, args.fps);
    state("idle");

    media::PanelEncoder enc;
    Asked asked;
    asked.pictures = args.fps;
    Counts n;
    bool skipToKey = false;
    double retryAt = 0, reportAt = Now() + kReport;
    int code = 0;
    while (!g_stop) {
        double now = Now();
        double wait = 1;
        if (enc.open() && asked.watched) wait = enc.Free() >= 0 ? feed.Wait(now) : 0.002;
        if (enc.open() && asked.key) wait = std::min(wait, std::max(0.0, asked.keyAt + kKeyWait - now));
        if (enc.open() && !asked.watched) wait = std::min(wait, std::max(0.0, asked.unwatchedAt + kLinger - now));
        if (!enc.open() && asked.watched) wait = std::min(wait, std::max(0.0, retryAt - now));
        pollfd p[3] = {together.Poll(), {feed.fd(), POLLIN, 0}, {enc.open() ? enc.fd() : -1, POLLIN, 0}};
        poll(p, 3, int(wait * 1000 + 0.999));
        now = Now();

        if (!together.Read()) {
            std::printf("ft-together went\n");
            code = 1;
            break;
        }
        uint8_t kind;
        std::vector<uint8_t> body;
        while (together.Next(kind, body)) {
            if (kind == local::kMediaRate && body.size() == 4) {
                asked.bitrate = std::clamp(int(local::MediaU32(body.data())), 100000, 50000000);
                enc.Bitrate(asked.bitrate);
            } else if (kind == local::kMediaPictures && body.size() == 1) {
                const int pictures = std::clamp(int(body[0]), 1, args.fps);
                if (pictures != asked.pictures) {
                    feed.AtMost(asked.pictures = pictures);
                    std::printf("at most %d pictures a second\n", pictures);
                }
            } else if (kind == local::kMediaKey) {
                if (!asked.key) asked.key = true, asked.keyAt = now;
            } else if (kind == local::kMediaWatched && body.size() == 1) {
                const bool watched = body[0] != 0;
                if (watched && !asked.watched && enc.open()) feed.Fresh();
                if (!watched && asked.watched) asked.unwatchedAt = now;
                asked.watched = watched;
            }
        }
        if (!feed.Serve()) break;   // ft-screens went, or said to end

        if (asked.watched && !enc.open() && now >= retryAt) {
            std::string err;
            if (enc.Open(args.width, args.height, args.fps, asked.bitrate, err) && feed.Begin(enc.pictures(), err)) {
                std::printf("watched: encoding at %d kbit/s\n", asked.bitrate / 1000);
                state("live");
                skipToKey = false;
            } else {
                std::printf("can't encode: %s\n", err.c_str());
                state("lost " + err);
                enc.Close();
                retryAt = now + kRetry;
            }
        }
        if (enc.open() && !asked.watched && now - asked.unwatchedAt >= kLinger) {
            Hand(enc, together, asked, skipToKey, n);
            feed.End();
            enc.Close();
            asked.key = false;
            std::printf("nobody watches: the encoder is closed\n");
            state("idle");
        }
        if (enc.open()) {
            Hand(enc, together, asked, skipToKey, n);
            if (!asked.watched) {
                feed.Skip(now);
            } else if (const int into = enc.Free(); into >= 0 && feed.Draw(enc.pictures(), into, now)) {
                if (enc.Queue(into, asked.key)) asked.key = false, ++n.pictures;
                else std::printf("the encoder took no picture: %s\n", enc.lastError().c_str());
            } else if (asked.key && now - asked.keyAt >= kKeyWait && enc.Again(true)) {
                asked.key = false, ++n.pictures;
            }
        }
        if (!together.Flush()) {
            std::printf("ft-together went\n");
            code = 1;
            break;
        }
        if (now >= reportAt) {
            if (n.pictures || n.units || n.dropped)
                std::printf("%s: %d pictures in, %d out (%d keyframes, %d dropped), %.0f kbit/s of %d asked, at most %d pictures a second\n",
                            asked.watched ? "watched" : "idle", n.pictures, n.units, n.keys, n.dropped, double(n.bytes) * 8 / 1000 / kReport,
                            asked.bitrate / 1000, asked.pictures);
            n = Counts{}, reportAt = now + kReport;
        }
    }
    if (enc.open()) feed.End();
    state(code ? "lost ft-together went" : "lost");
    return code;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Args args;
    if (!Read(argc, argv, args)) {
        std::fprintf(stderr, "usage: ft-sharepanel --panel 0..2 --size WxH [--fps F] [--socket NAME] (--fd N | --pattern)\n");
        return 2;
    }
    // ft-screens blocks SIGTERM for its own loop, and a child inherits that.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);
    if (args.pattern) {
        media::PatternFeed feed(args.fps);
        return Run(args, feed, [](const std::string &) {});
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);   // ft-screens went: so does this
    media::ScreenFeed feed(args.fd);
    return Run(args, feed, [&feed](const std::string &words) { feed.Say("state " + words); });
}
