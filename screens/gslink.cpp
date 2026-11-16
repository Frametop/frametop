// gslink.h's thread.
#include "gslink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace gsforward {

namespace {
struct Send {
    enum Type { Tag, Untag, Pointer, Button, Scroll, Key, Raise, Resize, Close } type = Close;
    uint32_t win = 0;
    uint64_t overlay = 0;
    int a = 0, b = 0;
    bool down = false;
};
}  // namespace

struct AsyncLink::Shared {
    std::string display;
    bool needForwarding = true;
    std::mutex m;
    std::condition_variable done;
    std::deque<Send> sends;
    std::vector<Update> updates;
    bool on = false, stop = false, finished = false;
    bool tryNow = false;   // turned on: open now, not at the next retry
    std::atomic<bool> open{false};
    int wake = -1;   // eventfd: a send or a change waits

    ~Shared() {
        if (wake >= 0) close(wake);
    }

    void Wake() {
        const uint64_t one = 1;
        if (wake >= 0) (void)!write(wake, &one, sizeof one);
    }
    void Push(const Send &s) {
        {
            std::lock_guard<std::mutex> l(m);
            if (s.type == Send::Pointer && !sends.empty() && sends.back().type == Send::Pointer) sends.back() = s;
            else if (sends.size() < kMaxQueued) sends.push_back(s);
        }
        Wake();
    }
    void Say(Update u) {
        std::lock_guard<std::mutex> l(m);
        updates.push_back(std::move(u));
    }

    static void Do(Link &link, const Send &s) {
        switch (s.type) {
            case Send::Tag: link.Tag(s.win, s.overlay); break;
            case Send::Untag: link.Untag(s.win); break;
            case Send::Pointer: link.Pointer(s.a, s.b); break;
            case Send::Button: link.Button(uint8_t(s.a), s.down); break;
            case Send::Scroll: link.Scroll(s.a); break;
            case Send::Key: link.Key(s.win, uint8_t(s.a), uint16_t(s.b), s.down); break;
            case Send::Raise: link.Raise(s.win); break;
            case Send::Resize: link.Resize(s.win, s.a, s.b); break;
            case Send::Close: break;
        }
    }

    void Run() {
        Link link;
        std::string lastWhy;
        auto nextTry = std::chrono::steady_clock::time_point{};
        bool wasOpen = false;
        for (;;) {
            bool wantOn, stopping, fresh;
            std::deque<Send> todo;
            {
                std::lock_guard<std::mutex> l(m);
                wantOn = on, stopping = stop, fresh = tryNow;
                tryNow = false;
                todo.swap(sends);
            }
            if (fresh) nextTry = {}, lastWhy.clear();
            for (const Send &s : todo) {
                if (s.type != Send::Close) {
                    Do(link, s);
                } else if (link.IsOpen()) {
                    // (said now, before an on right after it opens again: each Opened has its Closed)
                    link.Close();
                    wasOpen = false;
                    Update u;
                    u.type = Update::Closed, u.why = "off";
                    Say(u);
                }
            }
            if (stopping) break;
            const auto now = std::chrono::steady_clock::now();
            if (wantOn && !link.IsOpen() && now >= nextTry) {
                nextTry = now + std::chrono::milliseconds(kRetryMs);
                std::string why;
                if (link.Open(display.c_str(), needForwarding, &why)) {
                    lastWhy.clear();
                    Update u;
                    u.type = Update::Opened, u.pid = link.GamescopePid();
                    Say(u);
                } else if (why != lastWhy) {
                    lastWhy = why;
                    Update u;
                    u.type = Update::Closed, u.why = why;
                    Say(u);
                }
            }
            // (until none: Describe's replies can bring events in that the socket won't say are there)
            for (bool more = link.IsOpen(); more;) {
                std::vector<Event> events = link.Poll();
                more = !events.empty() && link.IsOpen();
                for (Event &e : events) {
                    if (e.type == Event::Mapped) link.Describe(&e.win);
                    Update u;
                    u.event = std::move(e);
                    Say(u);
                }
            }
            const bool isOpen = link.IsOpen();
            if (wasOpen && !isOpen) {
                Update u;
                u.type = Update::Closed, u.why = wantOn ? "the app display went" : "off";
                Say(u);
                nextTry = now + std::chrono::milliseconds(kRetryMs);
            }
            wasOpen = isOpen;
            open = isOpen;
            // until the display has events, a send waits, or (closed and on) it's time to try again
            pollfd fds[2] = {{wake, POLLIN, 0}, {link.Fd(), POLLIN, 0}};
            const int n = link.Fd() >= 0 ? 2 : 1;
            int timeout = -1;
            if (wantOn && !isOpen)
                timeout = int(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(nextTry - now).count()));
            if (poll(fds, nfds_t(n), timeout) > 0 && (fds[0].revents & POLLIN)) {
                uint64_t v;
                (void)!read(wake, &v, sizeof v);
            }
        }
        link.Close();
        open = false;
        std::lock_guard<std::mutex> l(m);
        finished = true;
        done.notify_all();
    }
};

AsyncLink::AsyncLink(std::string display, bool needForwarding) : d_(std::make_shared<Shared>()) {
    d_->display = std::move(display);
    d_->needForwarding = needForwarding;
    d_->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    std::thread([d = d_] { d->Run(); }).detach();
}

AsyncLink::~AsyncLink() { Stop(0); }

void AsyncLink::SetOn(bool on) {
    {
        std::lock_guard<std::mutex> l(d_->m);
        if (d_->on == on) return;
        d_->on = on;
        if (!on) d_->sends.push_back({});   // (a Close, after what's been sent)
        else d_->tryNow = true;
    }
    d_->Wake();
}

std::vector<Update> AsyncLink::Take() {
    std::vector<Update> out;
    std::lock_guard<std::mutex> l(d_->m);
    out.swap(d_->updates);
    return out;
}

bool AsyncLink::IsOpen() const { return d_->open; }

void AsyncLink::Stop(int waitMs) {
    std::unique_lock<std::mutex> l(d_->m);
    if (!d_->stop) {
        d_->stop = true;
        l.unlock();
        d_->Wake();
        l.lock();
    }
    d_->done.wait_for(l, std::chrono::milliseconds(std::max(0, waitMs)), [&] { return d_->finished; });
}

void AsyncLink::Tag(uint32_t win, uint64_t overlay) { d_->Push({Send::Tag, win, overlay}); }
void AsyncLink::Untag(uint32_t win) { d_->Push({Send::Untag, win}); }
void AsyncLink::Pointer(int x, int y) { d_->Push({Send::Pointer, 0, 0, x, y}); }
void AsyncLink::Button(uint8_t button, bool down) { d_->Push({Send::Button, 0, 0, button, 0, down}); }
void AsyncLink::Scroll(int notches) { d_->Push({Send::Scroll, 0, 0, notches}); }
void AsyncLink::Key(uint32_t win, uint8_t keycode, uint16_t state, bool down) {
    d_->Push({Send::Key, win, 0, keycode, state, down});
}
void AsyncLink::Raise(uint32_t win) { d_->Push({Send::Raise, win}); }
void AsyncLink::Resize(uint32_t win, int w, int h) { d_->Push({Send::Resize, win, 0, w, h}); }

}  // namespace gsforward
