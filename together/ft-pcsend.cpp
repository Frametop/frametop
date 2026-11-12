// ft-pcsend: a split compute PC's end of Frametop Together, as far as the Frame can tell
// (docs/together.md, "A PC's panels"; proto/sender.h). It calls its person's Frame with the code
// that Frame made for it, hears what each of its panels is to be encoded at, and sends pictures
// for the ones that are watched. It is what the tests and the fake peer have for a PC, and the
// pattern for the sender in the PC's own streaming host.
//
// Its pictures aren't a window's. They are a loop that was encoded earlier, played again and
// again from its keyframe: no encoder is taken, however many panels it sends. So it can't
// encode at another bitrate, and it plays slower where an encoder would encode less often.
//
//   ft-pcsend HOST PORT CODE --loop FILE [--loopback]
//
//   HOST PORT    where the Frame's ft-together takes calls: an address on the tailnet
//   CODE         what `sender` on that ft-together's socket answered
//   --loop FILE  the pictures, as a Frame's encoding program hands them over (local/mediawire.h:
//                'U' messages one after another), the first a keyframe
//   --loopback   for tests: the Frame is on 127.0.0.1
//
// It says what it is told, a line each ("panel 3 W 1"), and how many pictures it has made for a
// panel ("panel 3 sent 240") when that one is no longer watched and when it ends. It ends when
// the link does: with 1 when it was refused or the link was lost, and 0 when it was told to stop.
#include "local/mediawire.h"
#include "net/quiclinks.h"
#include "net/tailnet.h"
#include "proto/framewire.h"
#include "proto/sender.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <string>
#include <time.h>
#include <vector>

namespace {

constexpr int kSendingMs = 5, kRestingMs = 100;   // between looks at the link: while a panel is watched, and while none is
constexpr size_t kMostLoop = 64u << 20;           // a loop's file

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

struct Picture {
    bool key = false;
    std::vector<uint8_t> unit;
};

// The loop's pictures out of its file. None when it isn't one.
std::vector<Picture> Loop(const char *path) {
    std::vector<Picture> loop;
    FILE *f = std::fopen(path, "rb");
    if (!f) return loop;
    local::MediaReader reader;
    uint8_t buf[1 << 16], kind = 0;
    std::vector<uint8_t> body;
    size_t total = 0;
    bool ok = true;
    for (size_t n; ok && (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        ok = (total += n) <= kMostLoop && reader.Feed(buf, n);
        while (ok && reader.Next(kind, body)) {
            ok = kind == local::kMediaUnit && body.size() > 1 && !(body[0] & ~1);
            if (ok) loop.push_back({bool(body[0] & 1), {body.begin() + 1, body.end()}});
        }
    }
    std::fclose(f);
    if (!ok || reader.held() || loop.empty() || !loop[0].key) loop.clear();
    return loop;
}

// One of the PC's panels: what the Frame last said of it, and where in the loop it is.
struct Panel {
    int pictures = proto::kPanelPictures;
    bool watched = false, fromKey = true;   // the next picture is to be the loop's keyframe
    size_t next = 0;
    uint32_t count = 0;
    double due = 0;
};

// How many pictures it made for each panel that had any.
void SaySent(const Panel *panels) {
    for (int number = 0; number < proto::kPanelNumbers; ++number)
        if (panels[number].count) std::printf("panel %d sent %u\n", number, unsigned(panels[number].count));
}

}  // namespace

int main(int argc, char **argv) {
    const char *loopFile = nullptr;
    bool loopback = false;
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--loopback") loopback = true;
        else if (a == "--loop" && i + 1 < argc) loopFile = argv[++i];
        else words.push_back(a);
    }
    char *end = nullptr;
    const long port = words.size() == 3 ? std::strtol(words[1].c_str(), &end, 10) : 0;
    if (words.size() != 3 || !loopFile || *end || port < 1 || port > 65535) return std::fprintf(stderr, "usage: ft-pcsend HOST PORT CODE --loop FILE [--loopback]\n"), 2;
    const std::vector<Picture> loop = Loop(loopFile);
    if (loop.empty()) return std::fprintf(stderr, "ft-pcsend: %s isn't a loop of pictures that begins with a keyframe\n", loopFile), 2;

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

    Panel panels[proto::kPanelNumbers];
    proto::Parser parser;
    while (!g_stop) {
        bool sending = false;
        for (const Panel &p : panels) sending = sending || p.watched;
        pollfd fd = {links->fd(), POLLIN, 0};
        poll(&fd, 1, sending ? kSendingMs : kRestingMs);
        net::Event e;
        while (links->Next(e)) {
            if (e.kind == net::Event::kUp) {
                proto::Sender sender;
                sender.code = words[2];
                links->Send(link, proto::Pack(sender));
                std::printf("called %s\n", e.address.c_str());
            } else if (e.kind == net::Event::kDown) {
                return SaySent(panels), std::printf("the link to the headset ended\n"), 1;
            } else if (e.kind == net::Event::kFrameLost && e.tag < uint32_t(proto::kPanelNumbers)) {
                panels[e.tag].fromKey = true;   // what leans on a picture nobody got is no use
            } else if (e.kind == net::Event::kBytes) {
                if (!parser.Feed(e.bytes.data(), e.bytes.size())) return std::printf("the headset said what isn't a message\n"), 1;
                uint16_t kind = 0;
                std::vector<uint8_t> body;
                while (parser.Next(kind, body)) {
                    proto::Refuse refuse;
                    proto::Encode encode;
                    if (kind == proto::kRefuse && proto::Read(body, refuse)) return std::printf("refused: %s\n", refuse.text.c_str()), 1;
                    if (kind != proto::kEncode || !proto::Read(body, encode)) continue;
                    Panel &p = panels[encode.number];
                    const bool was = p.watched;
                    if (encode.word == proto::kEncodePictures) p.pictures = int(encode.value);
                    else if (encode.word == proto::kEncodeKey) p.fromKey = true;
                    // (from the keyframe without waiting for the K that follows a W 1: no picture before it)
                    else if (encode.word == proto::kEncodeWatched && bool(encode.value) != was) p.watched = encode.value, p.fromKey = true, p.due = 0;
                    std::printf("panel %d %c %u\n", int(encode.number), char(encode.word), unsigned(encode.value));
                    if (was && !p.watched) std::printf("panel %d sent %u\n", int(encode.number), unsigned(p.count));
                }
            }
        }
        links->Tick();
        const double now = Now();
        for (int number = 0; number < proto::kPanelNumbers; ++number) {
            Panel &p = panels[number];
            if (!p.watched || now < p.due) continue;
            if (p.fromKey) p.next = 0, p.fromKey = false;
            const Picture &picture = loop[p.next];
            proto::FrameHead head;
            head.panel = uint8_t(number), head.number = ++p.count, head.key = picture.key;
            if (links->Frame(link, proto::PackFrame(head, picture.unit.data(), picture.unit.size()), uint32_t(number))) p.next = (p.next + 1) % loop.size();
            else p.fromKey = true;
            p.due = (p.due > now - 1 ? p.due : now) + 1.0 / p.pictures;   // (in step, unless it fell a second behind)
        }
    }
    SaySent(panels);
    return 0;
}
