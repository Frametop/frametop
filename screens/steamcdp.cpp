// Steam's pages' mouse over DevTools (steamcdp.h has why; cdpwire.h the wire).
#include "steamcdp.h"

#include "cdpwire.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace steamcdp {
namespace {
constexpr int kPort = 8080;
constexpr int kWaitMs = 1000;           // a reply, a connect: given up on after this
constexpr int64_t kRetryMs = 1000;      // a page that couldn't be opened: tried again after this

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int Dial() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval tv{kWaitMs / 1000, (kWaitMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(kPort), a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) return ::close(fd), -1;
    return fd;
}
bool SendAll(int fd, const std::string &s) {
    for (size_t at = 0; at < s.size();) {
        const ssize_t n = ::send(fd, s.data() + at, s.size() - at, MSG_NOSIGNAL);
        if (n <= 0) return false;
        at += size_t(n);
    }
    return true;
}
// What's there to read, waiting up to `ms` for some. False: closed or failed.
bool ReadSome(int fd, std::string *buf, int ms) {
    pollfd p{fd, POLLIN, 0};
    const int r = ::poll(&p, 1, ms);
    if (r <= 0) return r == 0;
    char b[16384];
    const ssize_t n = ::recv(fd, b, sizeof b, MSG_DONTWAIT);
    if (n <= 0) return false;
    buf->append(b, size_t(n));
    return true;
}

// GET /json's body ("" when Steam isn't there).
std::string TargetList() {
    const int fd = Dial();
    if (fd < 0) return "";
    std::string got;
    if (SendAll(fd, "GET /json HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nConnection: close\r\n\r\n")) {
        const int64_t until = Now() + kWaitMs;
        for (char b[16384]; Now() < until;) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, int(until - Now())) <= 0) break;
            const ssize_t n = ::recv(fd, b, sizeof b, 0);
            if (n <= 0) break;
            got.append(b, size_t(n));
        }
    }
    ::close(fd);
    const size_t body = got.find("\r\n\r\n");
    return body == std::string::npos ? "" : got.substr(body + 4);
}

std::string Base64(const uint8_t *p, size_t n) {
    static const char *const k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += k[v >> 18 & 63], out += k[v >> 12 & 63];
        out += i + 1 < n ? k[v >> 6 & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

const char *Name(Button b) { return b == Button::Left ? "left" : b == Button::Right ? "right" : "middle"; }
int Bit(Button b) { return b == Button::Left ? 1 : b == Button::Right ? 2 : 4; }
const char *Held(int buttons) { return buttons & 1 ? "left" : buttons & 2 ? "right" : buttons & 4 ? "middle" : "none"; }

struct Send {
    enum Type { Move, Press, Wheel, Leave, LetGo } type = Move;
    std::string title;
    int x = 0, y = 0, n = 0;   // n: Wheel's notches
    Button b = Button::Left;
    bool down = false;
};

// One page's socket.
struct Page {
    int fd = -1;
    double dpr = 1;
    int nextId = 1;
    int buttons = 0;           // held (cdpwire's bits)
    int x = 0, y = 0;          // where it was last, window pixels
    int64_t retryAt = 0;
    std::string buf;
};
}  // namespace

struct Pages::D {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Send> q;
    bool stop = false;
    std::thread t;
    std::map<std::string, Page> pages;   // (the thread's alone)
    std::mt19937 rng{std::random_device{}()};

    void Push(Send s) {
        {
            std::lock_guard<std::mutex> l(m);
            if (s.type == Send::Move && !q.empty() && q.back().type == Send::Move && q.back().title == s.title) q.back() = s;
            else q.push_back(std::move(s));
        }
        cv.notify_one();
    }

    void Close(Page &p) {
        if (p.fd >= 0) ::close(p.fd);
        p.fd = -1, p.buf.clear();
    }

    // Its frames read and dropped (replies we don't wait for); false when Steam closed it.
    bool Drain(Page &p) {
        while (true) {
            pollfd f{p.fd, POLLIN, 0};
            if (::poll(&f, 1, 0) <= 0) break;
            if (!ReadSome(p.fd, &p.buf, 0)) return false;
        }
        int op;
        std::string payload;
        while (cdpwire::TakeFrame(p.buf, &op, &payload))
            if (op == 8) return false;
        return true;
    }

    bool Open(const std::string &title, Page &p) {
        std::string ws;
        for (const cdpwire::Target &t : cdpwire::Targets(TargetList()))
            if (t.title == title) ws = t.ws;
        std::string hostPort, path;
        if (!cdpwire::SplitWsUrl(ws, &hostPort, &path)) return false;
        p.fd = Dial();
        if (p.fd < 0) return false;
        uint8_t key[16];
        for (uint8_t &k : key) k = uint8_t(rng());
        if (!SendAll(p.fd, cdpwire::UpgradeRequest(hostPort, path, Base64(key, sizeof key)))) return Close(p), false;
        std::string head;
        const int64_t until = Now() + kWaitMs;
        while (head.find("\r\n\r\n") == std::string::npos)
            if (Now() >= until || !ReadSome(p.fd, &head, int(until - Now()))) return Close(p), false;
        if (!cdpwire::Upgraded(head)) return Close(p), false;
        p.buf = head.substr(head.find("\r\n\r\n") + 4);
        // its pixel ratio: CSS pixels are window pixels / it
        const int id = p.nextId++;
        if (!Write(p, cdpwire::Evaluate(id, "devicePixelRatio"))) return false;
        for (int op; Now() < until;) {
            std::string payload;
            if (cdpwire::TakeFrame(p.buf, &op, &payload)) {
                double v;
                if (op == 1 && cdpwire::IsReply(payload, id) && cdpwire::NumberAfter(payload, "value", &v) && v > 0) {
                    p.dpr = v;
                    break;
                }
                continue;
            }
            if (!ReadSome(p.fd, &p.buf, int(until - Now()))) return Close(p), false;
        }
        std::printf("Steam's page \"%s\": its mouse ours (pixel ratio %.2f)\n", title.c_str(), p.dpr);
        return true;
    }

    bool Write(Page &p, const std::string &json) {
        uint8_t mask[4];
        for (uint8_t &k : mask) k = uint8_t(rng());
        if (!SendAll(p.fd, cdpwire::TextFrame(json, mask))) return Close(p), false;
        return true;
    }

    // The page `title`, opened if it isn't; null when it can't be (now).
    Page *Ready(const std::string &title) {
        Page &p = pages[title];
        if (p.fd >= 0 && !Drain(p)) Close(p);
        if (p.fd >= 0) return &p;
        if (Now() < p.retryAt) return nullptr;
        if (!Open(title, p)) {
            p.retryAt = Now() + kRetryMs;
            return nullptr;
        }
        return &p;
    }

    void Mouse(Page &p, cdpwire::Mouse type, const char *button, int dy = 0) {
        Write(p, cdpwire::MouseEvent(p.nextId++, type, p.x / p.dpr, p.y / p.dpr, button, p.buttons, dy));
    }

    // Its buttons held up, where it was last.
    void Release(Page &p) {
        for (Button b : {Button::Left, Button::Right, Button::Middle})
            if (p.buttons & Bit(b) && p.fd >= 0) p.buttons &= ~Bit(b), Mouse(p, cdpwire::Mouse::Released, Name(b));
        p.buttons = 0;
    }

    void Do(const Send &s) {
        if (s.type == Send::LetGo) {
            for (auto &[title, p] : pages) Release(p);
            return;
        }
        if (s.type == Send::Leave) {   // (a page not open has nothing to leave: none opened for it)
            const auto it = pages.find(s.title);
            if (it == pages.end() || it->second.fd < 0) return;
            Release(it->second);
            it->second.x = it->second.y = -1;   // off the page: what it hovered hears the mouse leave
            Mouse(it->second, cdpwire::Mouse::Moved, "none");
            return;
        }
        Page *p = Ready(s.title);
        if (!p) {
            if (s.type == Send::Press) pages[s.title].buttons = 0;   // (Steam gets no half of it)
            return;
        }
        if (s.type != Send::Wheel) p->x = s.x, p->y = s.y;
        switch (s.type) {
            case Send::Move: Mouse(*p, cdpwire::Mouse::Moved, Held(p->buttons)); break;
            case Send::Press:
                p->buttons = s.down ? p->buttons | Bit(s.b) : p->buttons & ~Bit(s.b);
                Mouse(*p, s.down ? cdpwire::Mouse::Pressed : cdpwire::Mouse::Released, Name(s.b));
                break;
            case Send::Wheel: Mouse(*p, cdpwire::Mouse::Wheel, "none", -s.n * 100); break;
            case Send::Leave:
            case Send::LetGo: break;
        }
    }

    void Run() {
        std::unique_lock<std::mutex> l(m);
        while (true) {
            cv.wait(l, [&] { return stop || !q.empty(); });
            if (q.empty()) break;   // (stopping: what's queued goes first)
            std::deque<Send> now;
            now.swap(q);
            l.unlock();
            for (const Send &s : now) Do(s);
            l.lock();
        }
        for (auto &[title, p] : pages) Close(p);
    }
};

Pages::Pages() : d_(new D) { d_->t = std::thread([d = d_.get()] { d->Run(); }); }
Pages::~Pages() {
    {
        std::lock_guard<std::mutex> l(d_->m);
        d_->stop = true;
    }
    d_->cv.notify_one();
    d_->t.join();
}

void Pages::Move(const std::string &title, int x, int y) { d_->Push({Send::Move, title, x, y}); }
void Pages::Press(const std::string &title, int x, int y, Button b, bool down) {
    d_->Push({Send::Press, title, x, y, 0, b, down});
}
void Pages::Wheel(const std::string &title, int notches) { d_->Push({Send::Wheel, title, 0, 0, notches}); }
void Pages::Leave(const std::string &title) { d_->Push({Send::Leave, title}); }
void Pages::LetGo() { d_->Push({Send::LetGo, {}}); }

}  // namespace steamcdp
