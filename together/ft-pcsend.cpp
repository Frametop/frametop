// ft-pcsend: a split compute PC's end of Frametop Together (docs/together.md, "A PC's panels";
// proto/sender.h). It calls its person's Frame with the code that Frame made for it, hears what
// each of its panels is to be encoded at, and sends pictures for the ones that are watched.
//
//   ft-pcsend HOST PORT CODE --feed [--loopback]
//   ft-pcsend HOST PORT CODE --loop FILE [--loopback]
//
//   HOST PORT    where the Frame's ft-together takes calls: an address on the tailnet
//   CODE         what `sender` on that ft-together's socket answered; "-" when it is in the
//                environment instead (FT_PCSEND_CODE), where other users of the PC can't read it
//   --feed       the pictures are a streaming host's, which runs this and hands them over on
//                standard input as it encodes them (pcsend/feed.h): the PC's own windows
//   --loop FILE  the pictures are a loop that was encoded earlier (pcsend/loop.h), played again
//                and again from its keyframe: what the tests and the fake peer have for a PC.
//                No encoder is taken, however many panels it sends; so it can't encode at
//                another bitrate, and it plays slower where an encoder would encode less often
//   --loopback   for tests: the Frame is on 127.0.0.1
//
// It says what it is told, a line each ("panel 3 W 1"), and how many pictures it has sent for a
// panel ("panel 3 sent 240") when that one is no longer watched and when it ends. With a feed
// those lines are what the host encodes by, and "panel 3 K 0" is also said when a picture was
// lost on the way and the pictures after it are no use until a keyframe. It ends when the link
// does, or the feed: with 1 when it was refused or the link was lost, 2 when it couldn't start or
// the feed isn't one, and 0 when it was told to stop or the feed was closed.
#include "net/quiclinks.h"
#include "net/tailnet.h"
#include "pcsend/feed.h"
#include "pcsend/loop.h"
#include "proto/framewire.h"
#include "proto/sender.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <poll.h>
#include <string>
#include <time.h>
#include <vector>

namespace {

constexpr int kSendingMs = 5, kRestingMs = 100;   // between looks at the link: while a panel is watched, and while none is
constexpr double kAskS = 0.25;                    // between two asks of the host for a keyframe of one panel

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

// One of the PC's panels: what the Frame last said of it, and what was sent of it.
struct Panel {
    int pictures = proto::kPanelPictures;
    bool watched = false, fromKey = true;   // the next picture sent is to be a keyframe
    uint32_t count = 0;
    size_t next = 0;     // a loop's: where in it the panel is
    double due = 0;      // a loop's: when its next picture is
    double asked = -1;   // a feed's: when its host was last asked for a keyframe
};

// The link to the Frame and the PC's panels on it.
struct Pc {
    net::QuicLinks &links;
    int link;
    bool feed;   // a host encodes the pictures, by what is said here
    Panel panels[proto::kPanelNumbers];

    // One picture on its way. False when the link takes none now: what follows it is no use.
    bool Send(int number, bool key, const std::vector<uint8_t> &unit) {
        proto::FrameHead head;
        head.panel = uint8_t(number), head.number = panels[number].count + 1, head.key = key;
        if (!links.Frame(link, proto::PackFrame(head, unit.data(), unit.size()), uint32_t(number))) return false;
        ++panels[number].count;
        return true;
    }

    // What the Frame says a panel is to be encoded at.
    void Heard(const proto::Encode &encode, double now) {
        Panel &p = panels[encode.number];
        const bool was = p.watched;
        if (encode.word == proto::kEncodePictures) p.pictures = int(encode.value);
        // A loop's keyframe is its first picture. A host makes one when it reads the line, and
        // its pictures until then are still good to whoever lost nothing.
        else if (encode.word == proto::kEncodeKey) p.fromKey = p.fromKey || !feed, p.asked = now;
        // (from the keyframe without waiting for the K that follows a W 1: no picture before it)
        else if (encode.word == proto::kEncodeWatched && bool(encode.value) != was) p.watched = encode.value, p.fromKey = true, p.due = 0;
        std::printf("panel %d %c %u\n", int(encode.number), char(encode.word), unsigned(encode.value));
        if (was && !p.watched) std::printf("panel %d sent %u\n", int(encode.number), unsigned(p.count));
    }

    // The loop's next picture for each watched panel whose time it is.
    void Play(const std::vector<pcsend::Picture> &loop, double now) {
        for (int number = 0; number < proto::kPanelNumbers; ++number) {
            Panel &p = panels[number];
            if (!p.watched || now < p.due) continue;
            if (p.fromKey) p.next = 0, p.fromKey = false;
            const pcsend::Picture &picture = loop[p.next];
            if (Send(number, picture.key, picture.unit)) p.next = (p.next + 1) % loop.size();
            else p.fromKey = true;
            p.due = (p.due > now - 1 ? p.due : now) + 1.0 / p.pictures;   // (in step, unless it fell a second behind)
        }
    }

    // The host's pictures, as it made them. Those of a panel nobody watches go nowhere, and so
    // do those that lean on a picture that was lost: the host is asked for a keyframe then.
    void PassOn(const std::vector<pcsend::Unit> &units, double now) {
        for (const pcsend::Unit &unit : units) {
            Panel &p = panels[unit.number];
            if (!p.watched) continue;
            if (!p.fromKey || unit.key) p.fromKey = !Send(unit.number, unit.key, unit.picture);
            else if (now - p.asked >= kAskS) p.asked = now, std::printf("panel %d K 0\n", unit.number);
        }
    }

    // How many pictures were sent for each panel that had any.
    void SaySent() const {
        for (int number = 0; number < proto::kPanelNumbers; ++number)
            if (panels[number].count) std::printf("panel %d sent %u\n", number, unsigned(panels[number].count));
    }
};

int Usage() {
    std::fprintf(stderr, "usage: ft-pcsend HOST PORT CODE (--feed | --loop FILE) [--loopback]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    const char *loopFile = nullptr;
    bool loopback = false, fed = false;
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--loopback") loopback = true;
        else if (a == "--feed") fed = true;
        else if (a == "--loop" && i + 1 < argc) loopFile = argv[++i];
        else words.push_back(a);
    }
    char *end = nullptr;
    const long port = words.size() == 3 ? std::strtol(words[1].c_str(), &end, 10) : 0;
    if (words.size() != 3 || fed == bool(loopFile) || *end || port < 1 || port > 65535) return Usage();
    if (words[2] == "-") {
        const char *code = std::getenv("FT_PCSEND_CODE");
        if (!code) return Usage();
        words[2] = code;
    }
    std::vector<pcsend::Picture> loop;
    if (loopFile && (loop = pcsend::ReadLoop(loopFile)).empty()) return std::fprintf(stderr, "ft-pcsend: %s isn't a loop of pictures that begins with a keyframe\n", loopFile), 2;

    struct sigaction stop = {};
    stop.sa_handler = Stop;
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    net::QuicLinks::Config config;
    config.mayCall = [loopback](const std::string &address) { return loopback ? net::IsLoopback(address) : net::OnTailnet(address); };
    std::string err;
    const std::unique_ptr<net::QuicLinks> links = net::QuicLinks::Open(config, err);
    if (!links) return std::fprintf(stderr, "ft-pcsend: %s\n", err.c_str()), 1;
    const int link = links->Call(words[0], uint16_t(port));
    if (link < 0) return std::fprintf(stderr, "ft-pcsend: couldn't call %s: it isn't an address on the tailnet\n", words[0].c_str()), 1;

    Pc pc = {*links, link, fed, {}};
    std::optional<pcsend::Feed> feed;
    if (fed) feed.emplace(STDIN_FILENO);
    std::vector<pcsend::Unit> units;
    proto::Parser parser;
    while (!g_stop) {
        bool sending = false;
        for (const Panel &p : pc.panels) sending = sending || p.watched;
        pollfd fds[2] = {{links->fd(), POLLIN, 0}, {fed ? feed->fd() : -1, POLLIN, 0}};
        poll(fds, fed ? 2 : 1, sending ? kSendingMs : kRestingMs);
        const double now = Now();
        net::Event e;
        while (links->Next(e)) {
            if (e.kind == net::Event::kUp) {
                proto::Sender sender;
                sender.code = words[2];
                links->Send(link, proto::Pack(sender));
                std::printf("called %s\n", e.address.c_str());
            } else if (e.kind == net::Event::kDown) {
                return pc.SaySent(), std::printf("the link to the headset ended\n"), 1;
            } else if (e.kind == net::Event::kFrameLost && e.tag < uint32_t(proto::kPanelNumbers)) {
                pc.panels[e.tag].fromKey = true;   // what leans on a picture nobody got is no use
            } else if (e.kind == net::Event::kBytes) {
                if (!parser.Feed(e.bytes.data(), e.bytes.size())) return std::printf("the headset said what isn't a message\n"), 1;
                uint16_t kind = 0;
                std::vector<uint8_t> body;
                while (parser.Next(kind, body)) {
                    proto::Refuse refuse;
                    proto::Encode encode;
                    if (kind == proto::kRefuse && proto::Read(body, refuse)) return std::printf("refused: %s\n", refuse.text.c_str()), 1;
                    if (kind == proto::kEncode && proto::Read(body, encode)) pc.Heard(encode, now);
                }
            }
        }
        links->Tick();
        if (!fed) {
            pc.Play(loop, now);
            continue;
        }
        units.clear();
        feed->Read(units);
        pc.PassOn(units, now);
        if (feed->state() == pcsend::Feed::kBroken) return pc.SaySent(), std::fprintf(stderr, "ft-pcsend: what the host handed over isn't a picture\n"), 2;
        if (feed->state() == pcsend::Feed::kEnded) break;
    }
    pc.SaySent();
    return 0;
}
