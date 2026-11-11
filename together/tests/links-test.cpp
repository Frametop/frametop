// The links for real (together/net/quiclinks.h): two or three ends in this one program, on
// loopback. What a link promises the session: Up with the far end's address, the control
// stream's bytes in order, a close that lets what was sent arrive and then says nothing more,
// datagrams, and limits on what a far end can make this end hold.
#include "../net/quiclinks.h"
#include "../net/tailnet.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
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
    std::vector<uint8_t> Bytes(int link) const {
        std::vector<uint8_t> out;
        for (const net::Event &e : seen)
            if (e.kind == net::Event::kBytes && e.link == link) out.insert(out.end(), e.bytes.begin(), e.bytes.end());
        return out;
    }
};

// A link from `a` to `b`, up at both ends: a's number for it and b's.
static bool Link(End &a, End &b, int &atA, int &atB) {
    const int ups = b.Count(net::Event::kUp);
    atA = a.links->Call("127.0.0.1", b.port);
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
    ending();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
