// What a hand says on its way to ft-together (together/local/handsend.h): what doesn't fit in
// a full queue waits, in order, the oldest of too many are dropped, and nobody is waited for.
// The other end is a datagram socket of this test's that reads only when the test says so.
#include "../local/handsend.h"

#include <cstdio>

using local::HandSender;
using local::PanelHand;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static PanelHand::Out numbered(int from, int to) {
    PanelHand::Out out;
    for (int i = from; i < to; ++i) out.push_back("n" + std::to_string(i));
    return out;
}

// What waits at the other end, read without waiting.
static PanelHand::Out drained(int fd) {
    PanelHand::Out got;
    char buf[64];
    for (ssize_t n; (n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0;) got.emplace_back(buf, size_t(n));
    return got;
}

// Fills the other end's queue from sockets of the test's own, until a new socket's first
// datagram doesn't fit (one socket alone may run out of its own send buffer first).
static bool fill(const std::string &name, std::vector<int> &fillers) {
    for (int tries = 0; tries < 64; ++tries) {
        const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        fillers.push_back(fd);
        int went = 0;
        while (ft_sock_send(fd, name.c_str(), "x", 1, MSG_DONTWAIT) == 0) ++went;
        if (!went) return true;
    }
    return false;
}

// Reads `count` datagrams from the other end; true when that many were there.
static bool take(int fd, int count) {
    char buf[64];
    for (; count > 0 && recv(fd, buf, sizeof buf, MSG_DONTWAIT) > 0; --count) {}
    return count == 0;
}

int main() {
    const std::string name = "ft_handsend_test_" + std::to_string(getpid());
    sockaddr_un addr;
    const socklen_t len = ft_sock_addr(name.c_str(), &addr);
    const int end = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (end < 0 || !len || bind(end, reinterpret_cast<const sockaddr *>(&addr), len)) return std::perror("bind"), 1;
    std::vector<int> fillers;

    {
        HandSender to(name);
        PanelHand::Out out = numbered(0, 60), all;
        const PanelHand::Out sent = out;
        bool ok = fill(name, fillers);
        to.Send(out);
        check("into a full queue nothing goes, the call comes back, and it all waits", ok && out == sent);
        ok = take(end, 5);
        to.Send(out);
        check("when there is room for five, the five oldest go and the rest wait, in order", ok && out == PanelHand::Out(sent.begin() + 5, sent.end()));
        const PanelHand::Out before = out;
        to.Send(out);
        check("while the queue is full nothing more goes and nothing is lost", out == before);
        for (int round = 0; round < 200 && !out.empty(); ++round) {
            for (std::string &one : drained(end)) {
                if (one != "x") all.push_back(std::move(one));
            }
            to.Send(out);
        }
        for (std::string &one : drained(end)) all.push_back(std::move(one));
        check("as the queue empties it all arrives, once each, in the order it was said", out.empty() && all == sent);
    }
    {
        HandSender to(name);
        PanelHand::Out out = numbered(0, 300);
        const bool ok = fill(name, fillers);
        to.Send(out);
        const size_t most = HandSender::kMostWaiting;
        check("of more than may wait the oldest are dropped", ok && out.size() == most && out.front() == "n" + std::to_string(300 - most) && out.back() == "n299");
        PanelHand::Out more = numbered(300, 310);
        out.insert(out.end(), more.begin(), more.end());
        to.Send(out);
        check("...and again when more is said", out.size() == most && out.front() == "n" + std::to_string(310 - most) && out.back() == "n309");
        drained(end);
    }
    {
        HandSender to(name + "_nobody");
        PanelHand::Out out = numbered(0, 5);
        to.Send(out);
        check("said to where nobody listens, it is dropped", out.empty());
    }
    {
        HandSender to(name);
        PanelHand::Out out;
        to.Send(out);
        check("nothing to say sends nothing", out.empty() && drained(end).empty());
    }
    for (int fd : fillers) close(fd);
    close(end);
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
