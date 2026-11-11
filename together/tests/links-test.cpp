// The links for real (together/net/quiclinks.h): two or three ends in this one program, on
// loopback. What a link promises the session: Up with the far end's address, the control
// stream's bytes in order, a close that lets what was sent arrive and then says nothing more,
// datagrams, a picture whole on a stream of its own or given up in time, a file that was asked
// for and no other, and limits on what a far end can make this end hold.
#include "../net/quiclinks.h"
#include "../net/tailnet.h"
#include "../proto/framewire.h"
#include "../proto/messages.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
}

static uint64_t g_seed = 23;
static uint64_t roll() { return g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull, g_seed >> 11; }

static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static uint16_t FreePort() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    close(fd);
    return ntohs(a.sin_port);
}

static std::vector<uint8_t> Noise(size_t n) {
    std::vector<uint8_t> out(n);
    for (uint8_t &b : out) b = uint8_t(roll());
    return out;
}

// A picture's stream starts with the byte that says it is one (proto/framewire.h).
static std::vector<uint8_t> Picture(size_t n) {
    std::vector<uint8_t> out = Noise(n);
    if (n) out[0] = proto::kFrameStream;
    return out;
}

// One end, and everything its links have told it.
struct End {
    std::unique_ptr<net::QuicLinks> links;
    uint16_t port = 0;
    std::vector<net::Event> seen;

    explicit End(std::function<bool(const std::string &)> mayCall = net::IsLoopback) {
        std::string err;
        net::QuicLinks::Config config;
        config.listen = "127.0.0.1", config.port = port = FreePort(), config.mayCall = std::move(mayCall);
        links = net::QuicLinks::Open(config, err);
        if (!links) std::printf("open: %s\n", err.c_str());
    }
    void Take() {
        net::Event e;
        while (links->Next(e)) seen.push_back(std::move(e));
    }
    // Whether `want` comes true in time, taking what the links tell meanwhile.
    bool Until(const std::function<bool()> &want, double seconds = 5) {
        for (const double until = Now() + seconds;;) {
            links->Tick();
            Take();
            if (want()) return true;
            if (Now() >= until) return false;
            pollfd p = {links->fd(), POLLIN, 0};
            poll(&p, 1, 20);
        }
    }
    int Count(net::Event::Kind kind, int link = -1) const {
        int n = 0;
        for (const net::Event &e : seen) n += e.kind == kind && (link < 0 || e.link == link);
        return n;
    }
    // The newest link that came up, or -1.
    int Newest() const {
        for (size_t i = seen.size(); i-- > 0;)
            if (seen[i].kind == net::Event::kUp) return seen[i].link;
        return -1;
    }
    // The pictures that came on a link, as they came.
    std::vector<std::vector<uint8_t>> Frames(int link) const {
        std::vector<std::vector<uint8_t>> out;
        for (const net::Event &e : seen)
            if (e.kind == net::Event::kFrame && e.link == link) out.push_back(e.bytes);
        return out;
    }
    // The files that came on a link.
    std::vector<std::vector<uint8_t>> Files(int link) const {
        std::vector<std::vector<uint8_t>> out;
        for (const net::Event &e : seen)
            if (e.kind == net::Event::kFile && e.link == link) out.push_back(e.bytes);
        return out;
    }
    // The tags of the pictures a link gave up, in the order it told of them.
    std::vector<uint32_t> Lost(int link) const {
        std::vector<uint32_t> out;
        for (const net::Event &e : seen)
            if (e.kind == net::Event::kFrameLost && e.link == link) out.push_back(e.tag);
        return out;
    }
    std::vector<uint8_t> Bytes(int link) const {
        std::vector<uint8_t> out;
        for (const net::Event &e : seen)
            if (e.kind == net::Event::kBytes && e.link == link) out.insert(out.end(), e.bytes.begin(), e.bytes.end());
        return out;
    }
};

// A link from `a` to `b`, up at both ends: a's number for it and b's. `via` is another port to
// call, whose packets reach b.
static bool Link(End &a, End &b, int &atA, int &atB, uint16_t via = 0) {
    const int ups = b.Count(net::Event::kUp);
    atA = a.links->Call("127.0.0.1", via ? via : b.port);
    if (atA < 0 || !a.Until([&] { return a.Count(net::Event::kUp, atA) == 1; }) || !b.Until([&] { return b.Count(net::Event::kUp) == ups + 1; })) return false;
    atB = b.Newest();
    return true;
}

static void opening() {
    End a;
    std::string err;
    net::QuicLinks::Config config;
    config.listen = "127.0.0.1", config.port = a.port;
    check("an end listens, and a second one on its port doesn't", a.links && !net::QuicLinks::Open(config, err) && err.find("taken") != std::string::npos);
    config.listen = "the frame", config.port = FreePort();
    check("...nor one on something that isn't an address", !net::QuicLinks::Open(config, err) && !err.empty());
    check("an end with no links is quiet, and has nothing to tell", a.links->Quiet() && !a.Until([&] { return !a.seen.empty(); }, 0.2));
}

static void talking() {
    End a, b;
    int la = -1, lb = -1;
    bool ok = Link(a, b, la, lb);
    check("a call comes up at both ends, each knowing the other's address", ok && a.seen[0].address == "127.0.0.1" && b.seen[0].address == "127.0.0.1" && !a.links->Quiet());

    // the callee first: the caller has said nothing yet
    std::vector<uint8_t> toA, toB;
    for (int i = 0; i < 300; ++i) {
        const std::vector<uint8_t> m = Noise(1 + roll() % (i % 50 ? 300 : 60000)), n = Noise(1 + roll() % 2000);
        b.links->Send(lb, m), toA.insert(toA.end(), m.begin(), m.end());
        a.links->Send(la, n), toB.insert(toB.end(), n.begin(), n.end());
    }
    ok = a.Until([&] { return a.Bytes(la).size() >= toA.size(); }, 20) && b.Until([&] { return b.Bytes(lb).size() >= toB.size(); }, 20);
    check("what each end sends arrives at the other, whole and in order", ok && a.Bytes(la) == toA && b.Bytes(lb) == toB);

    // datagrams
    const std::vector<uint8_t> most = Noise(net::kMostDatagram), small = Noise(40);
    ok = a.Until([&] { return a.links->Datagram(la, small); }) && b.Until([&] { return b.Count(net::Event::kDatagram, lb) == 1; });
    ok = ok && a.links->Datagram(la, most) && b.links->Datagram(lb, most) && a.Until([&] { return a.Count(net::Event::kDatagram, la) == 1; }) &&
         b.Until([&] { return b.Count(net::Event::kDatagram, lb) == 2; });
    bool same = ok;
    for (const net::Event &e : b.seen)
        if (e.kind == net::Event::kDatagram) same = same && (e.bytes == small || e.bytes == most);
    check("a datagram arrives as it was sent, the largest a link carries too", same);
    ok = !a.links->Datagram(la, Noise(net::kMostDatagram + 1)) && !a.links->Datagram(la, {}) && !a.links->Datagram(la + 1000, small) && !a.links->Datagram(-1, small);
    check("...and one too long, an empty one, or one for no link isn't sent", ok);

    // closing: what was sent first still arrives, and the closer hears nothing more
    const std::vector<uint8_t> last = Noise(200000);
    const size_t before = b.Bytes(lb).size(), told = a.seen.size();
    b.links->Send(lb, small);   // this waits at a, untaken, when a closes the link
    usleep(300000);
    a.links->Send(la, last);
    a.links->Close(la);
    a.links->Send(la, small);   // into nothing
    ok = b.Until([&] { return b.Count(net::Event::kDown, lb) == 1; }, 10);
    const std::vector<uint8_t> got = b.Bytes(lb);
    check("a closed link still brings what was sent before, then goes down at the far end", ok && got.size() == before + last.size() &&
          std::equal(last.begin(), last.end(), got.begin() + long(before)) && b.seen.back().kind == net::Event::kDown);
    ok = a.Until([&] { return a.links->Quiet(); }, 10) && b.Until([&] { return b.links->Quiet(); }, 10);
    bool silent = true;
    for (size_t i = told; i < a.seen.size(); ++i) silent = silent && a.seen[i].link != la;
    check("...while the end that closed it hears nothing more of it, not even what waited, and both are quiet again", ok && silent && !a.links->Datagram(la, small));
}

static void refusing() {
    End a, b([](const std::string &address) { return address == "127.0.0.9"; });
    check("a call isn't made to an address that may not be called, or to a name that leads nowhere",
          b.links->Call("127.0.0.1", a.port) == -1 && a.links->Call("192.168.1.5", a.port) == -1 && a.links->Call("example.invalid", a.port) == -1 && a.links->Call("", a.port) == -1 &&
              !a.Until([&] { return !a.seen.empty(); }, 0.3) && a.links->Quiet() && b.links->Quiet());
    const int nobody = a.links->Call("localhost", FreePort());
    check("a call nobody answers goes down, and that is all that is told of it",
          nobody >= 0 && a.Until([&] { return a.Count(net::Event::kDown, nobody) == 1; }, 20) && a.seen.size() == 1 && a.Until([&] { return a.links->Quiet(); }));

    // the port is the listener's on its own address only
    const size_t heard = a.seen.size();
    const int beside = b.links->Call("127.0.0.9", a.port);
    check("a call to another address of this machine doesn't reach an end that listens on one",
          beside >= 0 && b.Until([&] { return b.Count(net::Event::kDown, beside) == 1; }, 25) && b.Count(net::Event::kUp) == 0 && (a.Take(), a.seen.size() == heard));

    // closed before it came up: nothing is told, at the end that called
    End c;
    const int early = a.links->Call("127.0.0.1", c.port);
    a.links->Close(early);
    const size_t told = a.seen.size();
    check("a call closed before it came up tells nothing", early >= 0 && a.Until([&] { return a.links->Quiet(); }, 10) && a.seen.size() == told && c.Until([&] { return c.links->Quiet(); }, 15));
}

static void limits() {
    // a far end that sends and sends to an end that doesn't take it
    {
        End a, b;
        int la = -1, lb = -1;
        bool ok = Link(a, b, la, lb);
        const std::vector<uint8_t> chunk = Noise(64000);
        for (int i = 0; i < 64; ++i) a.links->Send(la, chunk);   // 4 MB
        ok = ok && a.Until([&] { return a.Count(net::Event::kDown, la) == 1; }, 20);
        b.Take();
        const size_t held = b.Bytes(lb).size();
        check("a link whose bytes pile up untaken is cut, with at most a little over the limit held", ok && held > (1u << 20) && held < (3u << 19) && b.Count(net::Event::kDown, lb) == 1);
    }
    // datagrams nobody takes are dropped, and the link stays
    {
        End a, b;
        int la = -1, lb = -1;
        bool ok = Link(a, b, la, lb);
        const std::vector<uint8_t> d = Noise(1000);
        int sent = 0;
        for (const double until = Now() + 3; Now() < until && sent < 2000;) {
            if (a.links->Datagram(la, d)) ++sent;
            else usleep(1000);
            a.Take();
        }
        usleep(300000);
        b.Take();
        const int got = b.Count(net::Event::kDatagram, lb);
        check("datagrams that pile up untaken are dropped, and the link goes on", ok && sent >= 600 && got >= 200 && got <= 300 && b.Count(net::Event::kDown) == 0);
        b.links->Send(lb, d);
        check("...carrying the control stream as before", a.Until([&] { return a.Bytes(la) == d; }));
    }
    // more links than one end holds
    {
        End a, b;
        std::vector<int> made;
        int refused = 0;
        for (int i = 0; i < 40; ++i) {
            const int link = a.links->Call("127.0.0.1", b.port);
            if (link >= 0) made.push_back(link);
            else ++refused;
        }
        bool ok = made.size() == 32 && refused == 8 && a.Until([&] { return a.Count(net::Event::kUp) == 32; }, 20) && b.Until([&] { return b.Count(net::Event::kUp) == 32; }, 20);
        // the callee is full now: another end's call is turned away
        End c;
        const int extra = c.links->Call("127.0.0.1", b.port);
        ok = ok && extra >= 0 && c.Until([&] { return c.Count(net::Event::kDown, extra) == 1; }, 20) && c.Count(net::Event::kUp) == 0;
        check("an end holds so many links and no more: its own calls stop, and a call to it is turned away", ok);
        for (int link : made) a.links->Close(link);
        ok = a.Until([&] { return a.links->Quiet(); }, 15) && b.Until([&] { return b.links->Quiet() && b.Count(net::Event::kDown) == 32; }, 15);
        int la = -1, lb = -1;
        check("...and with those closed there is room again", ok && Link(c, b, la, lb));
    }
}

// The network between a caller and the end at port `to`, which can stop carrying anything.
struct Wire {
    int fd = -1;
    uint16_t port = 0, to = 0;
    std::atomic<bool> dead{false}, stop{false};
    std::thread thread;

    explicit Wire(uint16_t toPort) : to(toPort) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
        getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
        port = ntohs(a.sin_port);
        const int room = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &room, sizeof room), setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &room, sizeof room);
        const timeval wait = {0, 20000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        thread = std::thread([this] { Carry(); });
    }
    ~Wire() {
        stop = true;
        thread.join();
        close(fd);
    }
    void Carry() {
        sockaddr_in caller = {}, callee = {};
        callee.sin_family = AF_INET, callee.sin_addr.s_addr = htonl(INADDR_LOOPBACK), callee.sin_port = htons(to);
        bool known = false;
        std::vector<uint8_t> packet(65536);
        while (!stop) {
            sockaddr_in from = {};
            socklen_t len = sizeof from;
            const ssize_t n = recvfrom(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n <= 0 || dead) continue;
            if (ntohs(from.sin_port) == to) {
                if (known) sendto(fd, packet.data(), size_t(n), 0, reinterpret_cast<sockaddr *>(&caller), sizeof caller);
            } else {
                caller = from, known = true;
                sendto(fd, packet.data(), size_t(n), 0, reinterpret_cast<sockaddr *>(&callee), sizeof callee);
            }
        }
    }
};

// One picture sent until it arrives: on a slow machine one may be given up, as it should be.
static bool Arrives(End &from, int link, End &to, int farLink, const std::vector<uint8_t> &picture) {
    for (int tries = 0; tries < 20; ++tries) {
        const size_t had = to.Frames(farLink).size(), lost = from.Lost(link).size();
        if (!from.links->Frame(link, picture, 1)) return false;
        from.Until([&] { return to.Take(), to.Frames(farLink).size() > had || from.Lost(link).size() > lost; }, 10);
        if (to.Frames(farLink).size() > had) return to.Frames(farLink).back() == picture;
    }
    return false;
}

static void pictures() {
    const size_t most = proto::kFrameHead + proto::kMostFrame;
    {
        End a, b;
        int la = -1, lb = -1;
        bool ok = Link(a, b, la, lb);
        ok = ok && Arrives(a, la, b, lb, Picture(40000)) && Arrives(b, lb, a, la, Picture(900)) && a.Until([&] { return !a.links->Sending() && !b.links->Sending(); });
        check("a picture arrives whole on a stream of its own, either way", ok && b.Frames(lb).size() >= 1 && a.Frames(la).size() >= 1);
        check("...the longest one a panel may send too", Arrives(a, la, b, lb, Picture(most)));
        ok = !a.links->Frame(la, Picture(most + 1), 1) && !a.links->Frame(la, {}, 1) && !a.links->Frame(la + 1000, Picture(10), 1) && !a.links->Frame(-1, Picture(10), 1);
        check("...and one too long, an empty one, or one for no link isn't sent", ok && !a.links->Sending());

        // many at once: each arrives or is told lost, none twice, none silently
        const size_t had = b.Frames(lb).size(), lost = a.Lost(la).size();
        std::vector<std::vector<uint8_t>> sent;
        size_t refused = 0;
        for (int i = 0; i < 80; ++i) {
            std::vector<uint8_t> picture = Picture(2000 + roll() % 20000);
            picture[1] = uint8_t(i);   // which one it is, after the byte that says it is a picture
            if (a.links->Frame(la, picture, uint32_t(i))) sent.push_back(picture);
            else ++refused;
        }
        ok = a.Until([&] { return b.Take(), b.Frames(lb).size() - had + (a.Lost(la).size() - lost) >= sent.size() && !a.links->Sending(); }, 20);
        usleep(200000), a.Take(), b.Take();
        const std::vector<std::vector<uint8_t>> got = b.Frames(lb);
        const std::vector<uint32_t> gone = a.Lost(la);
        bool whole = true;
        for (size_t i = had; i < got.size(); ++i) {
            size_t matches = 0;
            for (const std::vector<uint8_t> &picture : sent) matches += picture == got[i];
            whole = whole && matches == 1;
            for (size_t k = lost; k < gone.size(); ++k) whole = whole && gone[k] != got[i][1];   // not both
        }
        std::printf("    of 80 at once: %zu taken, %zu arrived, %zu given up\n", sent.size(), got.size() - had, gone.size() - lost);
        ok = ok && whole && sent.size() + refused == 80 && sent.size() >= 64 && got.size() - had + (gone.size() - lost) == sent.size() && got.size() > had;
        check("a burst of pictures: each one taken arrives whole or is told lost, never both", ok);
        const std::vector<uint8_t> word = Noise(3000);
        a.links->Send(la, word);
        check("...with the control stream going on beside them", b.Until([&] { return b.Bytes(lb) == word; }) && a.Count(net::Event::kDown) == 0 && b.Count(net::Event::kDown) == 0);

        // a far end that doesn't take what comes: it holds so much and no more, and the link stays
        for (int i = 0; i < 8; ++i) {
            a.links->Frame(la, Picture(most), 100);
            a.Until([&] { return !a.links->Sending(); }, 10);
        }
        usleep(300000);
        const size_t before = b.Frames(lb).size();
        b.Take();
        const size_t held = b.Frames(lb).size() - before;
        std::printf("    of 8 MB untaken: %zu MB held\n", held);
        ok = held >= 1 && held <= 4 && b.Count(net::Event::kDown) == 0 && Arrives(a, la, b, lb, Picture(5000));
        check("pictures that pile up untaken are dropped past a few, and the link goes on", ok);

        // closing with pictures on their way
        const size_t told = a.seen.size();
        a.links->Frame(la, Picture(most), 5), a.links->Frame(la, Picture(most), 6);
        a.links->Close(la);
        ok = !a.links->Frame(la, Picture(10), 7) && b.Until([&] { return b.Count(net::Event::kDown, lb) == 1; }, 10) && a.Until([&] { return a.links->Quiet() && !a.links->Sending(); }, 10);
        bool silent = true;
        for (size_t i = told; i < a.seen.size(); ++i) silent = silent && a.seen[i].link != la;
        check("a link closed with pictures on their way goes down, and its closer hears nothing more of them", ok && silent);
    }
    {
        // the network stops carrying: what was sent is given up in time, and told
        End a, b;
        Wire wire(b.port);
        int la = -1, lb = -1;
        bool ok = Link(a, b, la, lb, wire.port) && Arrives(a, la, b, lb, Picture(30000));
        const size_t had = b.Frames(lb).size(), lost = a.Lost(la).size();
        wire.dead = true;
        const double from = Now();
        for (uint32_t tag = 11; tag <= 13; ++tag) ok = ok && a.links->Frame(la, Picture(30000), tag);
        ok = ok && a.links->Sending() && a.Until([&] { return a.Lost(la).size() == lost + 3; }, 5);
        const double took = Now() - from;
        std::vector<uint32_t> gone = a.Lost(la);
        gone.erase(gone.begin(), gone.begin() + long(lost));
        std::sort(gone.begin(), gone.end());
        std::printf("    given up after %.0f ms\n", took * 1000);
        check("pictures the network doesn't carry are given up a moment later, each told with its tag", ok && gone == std::vector<uint32_t>{11, 12, 13} && took > 0.1 && took < 1.5 && !a.links->Sending());
        // their streams wait for the far end's word that it knows: with those, a link has so many on their way
        size_t taken = 0;
        for (uint32_t i = 0; i < 80; ++i) taken += a.links->Frame(la, Picture(3000), 100 + i);
        ok = taken == 61 && a.Until([&] { return a.Lost(la).size() == lost + 3 + taken; }, 5) && !a.links->Sending();
        check("...and a link holds so many on their way and no more: the rest aren't sent", ok);
        wire.dead = false;
        const std::vector<uint8_t> word = Noise(500);
        a.links->Send(la, word);
        ok = b.Until([&] { return b.Bytes(lb) == word; }, 8) && b.Frames(lb).size() == had && a.Count(net::Event::kDown) == 0;
        check("...they never arrive, late or in part, and the link is still a link", ok);
        check("...which carries the next picture whole", Arrives(a, la, b, lb, Picture(30000)));
    }
    {
        // an end taken down with pictures on their way, in and out
        End b;
        int la = -1, lb = -1;
        bool ok = false;
        {
            End a;
            ok = Link(a, b, la, lb);
            for (int i = 0; i < 6; ++i) a.links->Frame(la, Picture(most), 1), b.links->Frame(lb, Picture(200000), 2);
            a.links.reset();
        }
        check("an end taken down with pictures on their way lets go of them", ok && b.Until([&] { return b.Count(net::Event::kDown, lb) == 1 && !b.links->Sending(); }, 6) && b.links->Quiet());
    }
}

using Shared = std::shared_ptr<const std::vector<uint8_t>>;

// A file is sent once the last one's stream is done with, which the sender only sees in time.
static bool Sends(End &from, int link, const Shared &file) {
    return from.Until([&] { return from.links->File(link, file); }, 10);
}

static void files() {
    {
        End a, b;
        int la = -1, lb = -1;
        bool ok = Link(a, b, la, lb);
        std::vector<uint8_t> noise = Noise(3u << 20);
        noise[0] = proto::kFrameStream;   // what a file starts with says nothing about its stream
        const Shared file = std::make_shared<const std::vector<uint8_t>>(noise);
        ok = ok && Sends(a, la, file) && !b.Until([&] { return !b.Files(lb).empty() || !b.Frames(lb).empty(); }, 1);
        check("a file nobody awaits isn't taken: nothing is told of it", ok);
        b.links->AwaitFile(lb, file->size());
        ok = Sends(a, la, file) && b.Until([&] { return b.Files(lb).size() == 1; }, 30) && b.Files(lb)[0] == *file;
        check("an awaited one arrives whole", ok);
        ok = Sends(a, la, file) && !b.Until([&] { return b.Files(lb).size() > 1; }, 1);
        check("...and that was the one: the next isn't awaited any more", ok);
        b.links->AwaitFile(lb, file->size() - 1);
        ok = Sends(a, la, file) && !b.Until([&] { return b.Files(lb).size() > 1; }, 1.5);
        b.links->AwaitFile(lb, file->size()), b.links->AwaitFile(lb, 0);
        ok = ok && Sends(a, la, file) && !b.Until([&] { return b.Files(lb).size() > 1; }, 1);
        check("a file longer than the one awaited is refused, and so is one after the wait was called off", ok);
        const Shared none, empty = std::make_shared<const std::vector<uint8_t>>(), huge = std::make_shared<const std::vector<uint8_t>>(proto::kMostLook + 1);
        ok = !a.links->File(la, none) && !a.links->File(la, empty) && !a.links->File(la, huge) && !a.links->File(la + 1000, file) && !a.links->File(-1, file);
        check("no file, an empty one, one too long, or one for no link isn't sent", ok);

        // the largest, with the rest of the link going on beside it
        const Shared most = std::make_shared<const std::vector<uint8_t>>(Noise(proto::kMostLook));
        a.links->AwaitFile(la, most->size()), b.links->AwaitFile(lb, most->size());
        ok = Sends(a, la, most) && !a.links->File(la, file) && Sends(b, lb, most) && !a.links->Sending();
        check("one file at a time goes out on a link, and a file isn't a picture on its way", ok);
        const std::vector<uint8_t> word = Noise(700), small(40, 7);
        const size_t had = b.Bytes(lb).size();
        a.links->Send(la, word);
        ok = Arrives(a, la, b, lb, Picture(30000)) && a.links->Datagram(la, small) && b.Until([&] { return b.Bytes(lb).size() == had + word.size() && b.Count(net::Event::kDatagram, lb) >= 1; });
        check("the largest file goes both ways at once, with pictures, datagrams and the control stream beside it", ok);
        ok = b.Until([&] { return b.Files(lb).size() == 2; }, 60) && b.Files(lb)[1] == *most && a.Until([&] { return a.Files(la).size() == 1; }, 60) && a.Files(la)[0] == *most;
        check("...and arrives whole", ok && a.Count(net::Event::kDown) == 0 && b.Count(net::Event::kDown) == 0);

        // a file that waits untaken doesn't count against the link
        b.links->AwaitFile(lb, most->size());
        ok = Sends(a, la, most);
        const size_t before = b.Bytes(lb).size();
        a.Until([] { return false; }, 4);   // b takes nothing meanwhile
        a.links->Send(la, word);
        ok = ok && b.Until([&] { return b.Files(lb).size() == 3 && b.Bytes(lb).size() == before + word.size(); }, 30);
        check("a file that waits for the loop doesn't get the link cut", ok && b.Count(net::Event::kDown) == 0);

        // a picture's stream that says it is a file is one
        std::vector<uint8_t> sly = Noise(5000);
        sly[0] = proto::kLookStream;
        const size_t frames = b.Frames(lb).size();
        ok = a.links->Frame(la, sly, 5) && !b.Until([&] { return b.Frames(lb).size() > frames || b.Files(lb).size() > 3; }, 1);
        check("a stream that starts as a file is held to a file's rules: unawaited, it isn't taken", ok);

        b.links->AwaitFile(lb, most->size());
        ok = Sends(a, la, most);
        b.links->Close(lb);
        const size_t told = b.seen.size();
        ok = ok && a.Until([&] { return a.Count(net::Event::kDown, la) == 1; }, 10) && b.Until([&] { return b.links->Quiet(); }, 10) && b.seen.size() == told;
        check("a link closed with a file on its way goes down, and its closer hears nothing more of it", ok);
    }
    {
        End b;
        int la = -1, lb = -1;
        bool ok = false;
        {
            End a;
            ok = Link(a, b, la, lb);
            const Shared most = std::make_shared<const std::vector<uint8_t>>(Noise(proto::kMostLook));
            a.links->AwaitFile(la, most->size()), b.links->AwaitFile(lb, most->size());
            ok = ok && a.links->File(la, most) && b.links->File(lb, most);
            a.links.reset();
        }
        check("an end taken down with files on their way, in and out, lets go of them", ok && b.Until([&] { return b.Count(net::Event::kDown, lb) == 1; }, 6) && b.links->Quiet() && b.Files(lb).empty());
    }
}

static void ending() {
    End b;
    int la = -1, lb = -1;
    double took = 0;
    bool ok = false;
    {
        End a, idle;
        ok = Link(a, b, la, lb);
        a.links->Send(la, Noise(5000));
        const double from = Now();
        a.links.reset();      // with a link up, and bytes on their way
        idle.links.reset();   // and one that never had any
        took = Now() - from;
    }
    check("an end with links up can be taken down, quickly", ok && took < 3);
    check("...and the far end's links go down without waiting for the silence to be noticed", b.Until([&] { return b.Count(net::Event::kDown, lb) == 1; }, 4) && b.links->Quiet());
}

int main() {
    opening();
    talking();
    refusing();
    limits();
    pictures();
    files();
    ending();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
