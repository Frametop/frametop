// The shared clipboard's syncs (stream/sharedclip.h).
#include "sharedclip.h"

#include "hostinput.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

using namespace ftstream;

namespace {

int64_t Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

const char *Kind(const clipboard::Content &c) { return c.image ? "image" : "text"; }

}  // namespace

void SharedClipboard::Open(Client *c, const std::string &address, const std::string &ownPin) {
    client = c, host = address;
    const char *run = std::getenv("XDG_RUNTIME_DIR");
    state = clipboard::StateFile(run, hostinput::RelayId(address));
    if (state.empty()) {
        std::printf("clipboard: no XDG_RUNTIME_DIR, so it isn't shared\n");
        gone = true;
        return;
    }
    wayland = std::string(run) + "/frametop/wayland-0";  // session/frametop-session.sh
    std::ifstream p(KeyDir("hosts") + "/" + address + ".pin");
    if (!ownPin.empty()) pin = ownPin;
    else if (!(p >> pin)) pin.clear();
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    clip = n > 0 ? std::string(exe, size_t(n)) : "ft-stream";
    clip = clip.substr(0, clip.rfind('/') + 1) + "ft-clip";
    thread = std::thread(&SharedClipboard::Loop, this);
}
void SharedClipboard::Close() {
    if (!thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mu);
        quit = true;
    }
    cv.notify_one();
    thread.join();  // a sync under way stops within 0.1 s (quit)
}
void SharedClipboard::Tick(int64_t now, const std::function<bool()> &shared) {
    if (now < checkAt) return;
    checkAt = now + 3'000'000'000;
    const bool want = shared();
    if (want != on.exchange(want)) std::printf("clipboard: %s with %s\n", want ? "shared" : "not shared", host.c_str());
}
void SharedClipboard::Focus() {
    if (focused) return;
    focused = true;
    Queue('s');
}
void SharedClipboard::Blur() {
    if (!focused) return;
    focused = false;
    Queue('b');
}
void SharedClipboard::Queue(char op) {
    if (!on || gone) return;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (ops.empty() || ops.back() != op) ops.push_back(op);
    }
    cv.notify_one();
}
void SharedClipboard::Loop() {
    for (;;) {
        char op;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] { return quit || !ops.empty(); });
            if (quit) return;
            op = ops.front();
            ops.pop_front();
        }
        if (op == 's') Send();
        else Bring();
        std::fflush(stdout);
    }
}
void SharedClipboard::Say(const std::string &why) {
    if (said.insert(why).second) std::printf("clipboard: %s\n", why.c_str());
}
bool SharedClipboard::Answered(long code, bool set, bool image) {
    if (quit) return false;  // the stream ends: nothing to say about a request it cut short
    std::string why;
    const clipboard::Answer a = clipboard::Classify(code, set, image, &why);
    if (a == clipboard::Answer::Ok) return true;
    Say(why);
    if (a == clipboard::Answer::Gone) gone = true;  // not again from this stream
    if (a == clipboard::Answer::TextOnly) images = false;  // nor images
    return false;
}
bool SharedClipboard::Desktop(clipboard::Content *c) {
    *c = {};
    for (const bool image : {false, true}) {
        if (image && !images) break;
        const char *what = image ? "get-image" : "get";
        const int st = Run(what, nullptr, &c->data);
        if (st == 2 || st < 0) return Say(std::string("no Frametop desktop to share the clipboard with (ft-clip ") +
                                          what + ": " + std::to_string(st) + ")"), false;
        if (st == 0) return c->image = image, true;
        c->data.clear();
    }
    return true;
}
void SharedClipboard::Send() {
    if (!on) return;  // turned off while it waited
    clipboard::Content c;
    if (!Desktop(&c) || !clipboard::ShouldPush(c, clipboard::ReadLast(state))) return;
    std::string body = c.data;
    if (!Answered(client->Clipboard(true, c.image, &body, pin, &quit), true, c.image)) return;
    clipboard::WriteLast(state, clipboard::Hash(c));
    std::printf("clipboard: sent %zu bytes of %s to %s\n", c.data.size(), Kind(c), host.c_str());
}
void SharedClipboard::Bring() {
    if (!on) return;
    // Its text, else (a host that shares images) its image.
    clipboard::Content h, desktop;
    if (!Answered(client->Clipboard(false, false, &h.data, pin, &quit), false, false)) return;
    if (h.data.empty() && images) {
        h.image = true;
        if (!Answered(client->Clipboard(false, true, &h.data, pin, &quit), false, true)) return;
    }
    if (!Desktop(&desktop)) return;
    if (!clipboard::ShouldPull(h, desktop, clipboard::ReadLast(state))) {
        if (clipboard::Fits(h)) clipboard::WriteLast(state, clipboard::Hash(h));  // the same
        return;
    }
    if (!h.image) h.data = clipboard::Normalize(h.data);
    const char *what = h.image ? "set-image" : "set";
    const int st = Run(what, &h.data, nullptr);
    if (st != 0) return Say(std::string("ft-clip ") + what + ": " + std::to_string(st));
    clipboard::WriteLast(state, clipboard::Hash(h));
    std::printf("clipboard: brought %zu bytes of %s from %s\n", h.data.size(), Kind(h), host.c_str());
}
int SharedClipboard::Run(const char *what, const std::string *in, std::string *out) {
    const bool image = std::strstr(what, "image") != nullptr;
    const size_t max = image ? clipboard::kMaxImage : clipboard::kMaxText;
    int to[2] = {-1, -1}, from[2] = {-1, -1};
    if (pipe2(to, O_CLOEXEC) != 0) return -1;
    if (pipe2(from, O_CLOEXEC) != 0) return close(to[0]), close(to[1]), -1;
    std::vector<std::string> env{"WAYLAND_DISPLAY=" + wayland};
    for (char **e = environ; *e; ++e)
        if (std::strncmp(*e, "WAYLAND_DISPLAY=", 16) != 0) env.push_back(*e);
    std::vector<char *> envp;
    for (auto &e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    char *argv[] = {clip.data(), const_cast<char *>(what), nullptr};
    pid_t pid = -1;
    const int r = posix_spawn(&pid, clip.c_str(), &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(to[0]), close(from[1]);
    if (r != 0) return close(to[1]), close(from[0]), Say("can't run " + clip + ": " + std::strerror(r)), -1;
    // ft-clip reads all of its input before it does anything, so this can't wait on it.
    for (size_t at = 0; in && at < in->size();) {
        const ssize_t w = write(to[1], in->data() + at, in->size() - at);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        at += size_t(w);
    }
    close(to[1]);
    const int64_t until = Now() + (image ? 12'000'000'000 : 3'000'000'000);  // its own wait: 10 s, 2 s
    bool late = false, big = false;
    if (out) {
        char buf[65536];
        for (;;) {
            const int64_t left = (until - Now()) / 1'000'000;
            if (left <= 0 || quit) {
                late = true;
                break;
            }
            pollfd p{from[0], POLLIN, 0};
            const int polled = poll(&p, 1, int(std::min<int64_t>(left, 100)));  // quit within 0.1 s
            if (polled == 0 || (polled < 0 && errno == EINTR)) continue;
            if (polled < 0) {
                late = true;
                break;
            }
            const ssize_t n = read(from[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (out->size() + size_t(n) > max) {  // ft-clip says no first; never more
                big = true;
                break;
            }
            out->append(buf, size_t(n));
        }
    }
    close(from[0]);
    int st = 0;
    while (!late && !big) {
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (w < 0 || Now() >= until || quit) break;
        usleep(10'000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    if (!quit) Say(std::string("ft-clip ") + what + (big ? " gave more than it may" : " didn't finish in time"));
    return -1;
}
