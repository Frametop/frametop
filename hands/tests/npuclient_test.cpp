// track/npuclient.cpp and npusession.cpp against a fake ft-npud (no DSP): the layout both sides pin, the HELLO
// that hands over the sealed memory, BUSY, a silent or old daemon, a closed connection, a signal during a wait,
// and the retry backoff after a failure.
//   build/npuclient-test
#include "../track/npuclient.h"
#include "../track/npusession.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

// npu-hands npud/test_transport.py pins the same numbers on the daemon's side.
static_assert(NpuClient::kOutSlot == 47824, "landmark output slot");
static_assert(NpuClient::kOuts == 204800, "landmark outputs");
static_assert(NpuClient::kDetOuts == 396096, "detector outputs");
static_assert(NpuClient::kShmBytes == 413696, "memory size");

namespace {

using ms = std::chrono::milliseconds;

// A fake daemon: a listening socket in a fresh directory and its memory (a sealed memfd unless told otherwise).
struct Fake {
    std::string dir, path;
    int srv = -1, mem = -1;
    uint8_t *map = nullptr;
    explicit Fake(bool sealed = true, uint32_t version = 2, uint32_t graph = 0x1234) {
        char tmpl[] = "/tmp/npuclient-test-XXXXXX";
        dir = mkdtemp(tmpl);
        path = dir + "/ft-npud.sock";
        mem = memfd_create("fake-npud", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (ftruncate(mem, NpuClient::kShmBytes) != 0) std::abort();
        if (sealed) fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
        map = static_cast<uint8_t *>(mmap(nullptr, NpuClient::kShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0));
        const uint32_t h[8] = {0x4450504E, version, 0, 0, 0, 0, 0, graph};
        std::memcpy(map, h, sizeof h);
        srv = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
        if (bind(srv, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(srv, 4) != 0) std::abort();
    }
    ~Fake() {
        ::close(srv), ::close(mem), munmap(map, NpuClient::kShmBytes);
        unlink(path.c_str()), rmdir(dir.c_str());
    }
    // accepts one client and sends it status, with the memory's fd when with_fd
    int greet(uint8_t status, bool with_fd) {
        const int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
        iovec iov{&status, 1};
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))] = {};
        msghdr mh{};
        mh.msg_iov = &iov, mh.msg_iovlen = 1;
        if (with_fd) {
            mh.msg_control = ctl, mh.msg_controllen = sizeof ctl;
            cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET, cm->cmsg_type = SCM_RIGHTS, cm->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cm), &mem, sizeof mem);
        }
        sendmsg(c, &mh, MSG_NOSIGNAL);
        return c;
    }
};

// open() on the main thread while the fake answers on another
bool open_with(Fake &f, NpuClient &cl, std::string &err, const std::function<void()> &daemon) {
    std::thread t(daemon);
    const bool ok = cl.open(f.dir, 1000, 0x1234, err);
    t.join();
    return ok;
}

void hello_maps_the_memory() {
    Fake f;
    NpuClient cl;
    std::string err;
    int c = -1;
    CHECK(open_with(f, cl, err, [&] { c = f.greet(0, true); }));
    CHECK(cl.connected());
    if (cl.connected()) {
        cl.crop(2)[5] = 77;   // the client's writes land in the daemon's memory
        CHECK(f.map[NpuClient::kCrops + 2 * NpuClient::kCrop * NpuClient::kCrop + 5] == 77);
        f.map[NpuClient::kDetOuts + NpuClient::kDetBytes] = 0x7f;
        CHECK(cl.det_output(1)[0] == 0x7f);
    }
    // pair bits past the request's crops never reach the daemon
    CHECK(!cl.run(1, 500, err, NpuClient::kPair0) && !cl.run(3, 500, err, NpuClient::kPair1));
    CHECK(!cl.run(2, 500, err, NpuClient::kPair1) && !cl.run(3, 500, err, NpuClient::kPair0 | NpuClient::kPair1));
    CHECK(!cl.run(4, 500, err, 0x80) && cl.connected());
    // a paired request and a plain one answered OK, then one the daemon never answers before closing
    std::thread t([&] {
        uint8_t req = 0, ok = 0;
        CHECK(recv(c, &req, 1, 0) == 1 && req == (4 | NpuClient::kPair0 | NpuClient::kPair1));
        send(c, &ok, 1, MSG_NOSIGNAL);
        CHECK(recv(c, &req, 1, 0) == 1 && req == 3);
        send(c, &ok, 1, MSG_NOSIGNAL);
        CHECK(recv(c, &req, 1, 0) == 1 && req == 1);
        ::close(c);
    });
    CHECK(cl.run(4, 500, err, NpuClient::kPair0 | NpuClient::kPair1));
    CHECK(cl.run(3, 500, err));
    CHECK(!cl.run(1, 500, err));
    CHECK(err == "ft-npud closed the connection");
    CHECK(!cl.connected());
    t.join();
}

void busy_old_unsealed_and_closed() {
    std::string err;
    {
        Fake f;
        NpuClient cl;
        int c = -1;
        CHECK(!open_with(f, cl, err, [&] { c = f.greet(2, false); }));
        CHECK(err == "ft-npud serves another client");
        ::close(c);
    }
    {
        Fake f(true, 1);
        NpuClient cl;
        int c = -1;
        CHECK(!open_with(f, cl, err, [&] { c = f.greet(0, true); }));
        CHECK(err == "ft-npud speaks another protocol version");
        ::close(c);
    }
    {
        Fake f(true, 2, 0x9999);
        NpuClient cl;
        int c = -1;
        CHECK(!open_with(f, cl, err, [&] { c = f.greet(0, true); }));
        CHECK(err == "ft-npud serves another graph than the heads file's");
        ::close(c);
    }
    {
        Fake f(false);
        NpuClient cl;
        int c = -1;
        CHECK(!open_with(f, cl, err, [&] { c = f.greet(0, true); }));
        CHECK(err == "ft-npud's memory is too small or not sealed");
        ::close(c);
    }
    {
        Fake f;
        NpuClient cl;
        CHECK(!open_with(f, cl, err, [&] { ::close(accept4(f.srv, nullptr, nullptr, 0)); }));   // another UID
        CHECK(err == "ft-npud closed the connection (another user?)");
    }
    {
        NpuClient cl;
        CHECK(!cl.open("/tmp/npuclient-test-none", 1000, 0x1234, err));
        CHECK(!cl.connected());
    }
}

void silent_daemon_times_out() {
    Fake f;   // listening, never accepting: connect succeeds into the backlog, no HELLO
    NpuClient cl;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!cl.open(f.dir, 1000, 0x1234, err));
    const auto took = std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - t0).count();
    CHECK(err == "ft-npud did not answer the connection");
    CHECK(took >= NpuClient::kHelloMs - 5 && took < NpuClient::kHelloMs + 150);
}

void signal_does_not_cut_a_wait() {
    Fake f;
    NpuClient cl;
    std::string err;
    int c = -1;
    CHECK(open_with(f, cl, err, [&] { c = f.greet(0, true); }));
    struct sigaction sa{};
    sa.sa_handler = [](int) {};   // no SA_RESTART: poll() returns EINTR
    sigaction(SIGALRM, &sa, nullptr);
    itimerval it{{0, 5000}, {0, 5000}};   // every 5 ms
    setitimer(ITIMER_REAL, &it, nullptr);
    std::thread t([&] {
        uint8_t req = 0, ok = 0;
        recv(c, &req, 1, 0);
        std::this_thread::sleep_for(ms(60));
        send(c, &ok, 1, MSG_NOSIGNAL);
    });
    CHECK(cl.run(4, 500, err));
    itimerval off{};
    setitimer(ITIMER_REAL, &off, nullptr);
    t.join();
    // and a timeout still ends on time under the signals
    setitimer(ITIMER_REAL, &it, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!cl.run(4, 40, err));
    const auto took = std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - t0).count();
    setitimer(ITIMER_REAL, &off, nullptr);
    CHECK(err == "ft-npud timed out");
    CHECK(took >= 35 && took < 120);
    ::close(c);
}

void failure_backs_off() {
    // a request failed on a session that last connected long ago: the next step must not try again at once
    NpuSession s("/tmp/npuclient-test-none");
    s.fail("ft-npud timed out");
    CHECK(!s.ensure());
    CHECK(s.last_error() == "ft-npud timed out");   // not "cannot open .../ft-npud.heads": no attempt
    CHECK(s.failures == 1);
}

}  // namespace

int main() {
    hello_maps_the_memory();
    busy_old_unsealed_and_closed();
    silent_daemon_times_out();
    signal_does_not_cut_a_wait();
    failure_backs_off();
    std::printf(failures ? "npuclient: %d failures\n" : "npuclient: ok\n", failures);
    return failures != 0;
}
