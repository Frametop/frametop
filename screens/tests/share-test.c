// screens/share.c, offline: a shared screen's pictures go to its ft-sharepanel one at a time, each
// buffer told once and kept until it's read, at most the stream's rate of them and none while
// nobody watches; and a program that ends, or keeps a picture, is started again. The real
// share.c runs with made-up buffers; this program is also the ft-sharepanel it starts
// ($FT_SHAREPANEL): it notes what it's told and answers as the real one does (--fps 7: it never
// says "done"). SIGUSR1 makes it say nobody watches, SIGUSR2 that someone does again.
// Built by `just test-c` (justfile): share.c with -DFT_SHARE_STUCK_MS=500, so a kept picture is
// noticed soon.
#define _GNU_SOURCE
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "share.h"

static char g_log[PATH_MAX];

static long ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void note(const char *what, int pid, long a, long b) {
    FILE *f = fopen(g_log, "a");
    fprintf(f, "%s %d %ld %ld %ld\n", what, pid, a, b, ms());
    fclose(f);
}

// --- the fake ft-sharepanel: --panel K --size WxH --fps F --fd 3 ---
static volatile sig_atomic_t g_term, g_off, g_on;
static void on_signal(int s) {
    if (s == SIGTERM) g_term = 1;
    if (s == SIGUSR1) g_off = 1;
    if (s == SIGUSR2) g_on = 1;
}

static void tell(const char *msg) { send(3, msg, strlen(msg), MSG_NOSIGNAL); }

static int fake_program(char **argv) {
    const int slot = atoi(argv[2]), fps = atoi(argv[6]);
    signal(SIGTERM, on_signal), signal(SIGUSR1, on_signal), signal(SIGUSR2, on_signal);
    note("start", getpid(), slot, fps);
    tell("state idle");
    tell("want 1");
    while (!g_term) {
        if (g_off) g_off = 0, tell("want 0"), note("off", getpid(), 0, 0);
        if (g_on) g_on = 0, tell("want 1"), note("on", getpid(), 0, 0);
        struct pollfd p = {3, POLLIN, 0};
        if (poll(&p, 1, 10) <= 0) continue;
        char m[512], control[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = {m, sizeof m - 1};
        struct msghdr h = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
        const ssize_t n = recvmsg(3, &h, 0);
        if (n <= 0) break;
        m[n] = 0;
        int nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const int got = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int k = 0; k < got; ++k) {
                int f;
                memcpy(&f, CMSG_DATA(c) + k * sizeof(int), sizeof f);
                close(f), ++nfds;
            }
        }
        unsigned id, w, h2;
        if (sscanf(m, "buffer %u %u %u", &id, &w, &h2) == 3) {
            note("buffer", getpid(), id, nfds * 100000 + (long)w);
        } else if (sscanf(m, "frame %u", &id) == 1) {
            note("frame", getpid(), id, 0);
            if (fps == 7) continue;  // the one that keeps its pictures
            usleep(3000);
            char done[32];
            snprintf(done, sizeof done, "done %u", id);
            tell(done);
        } else if (sscanf(m, "forget %u", &id) == 1) {
            note("forget", getpid(), id, 0);
        } else if (!strcmp(m, "quit")) {
            note("quit", getpid(), 0, 0);
            return 0;
        }
    }
    note("term", getpid(), 0, 0);
    return 0;
}

// --- ft-screens' side: buffers that count their holds ---
struct fake {
    int fd, holds;
};
static struct fake g_bufs[12];
static void *g_current[4];

static void *current(void *data, int index) { return data == &g_current && index >= 0 && index < 4 ? g_current[index] : NULL; }
static bool describe(void *buffer, struct ft_dmabuf *out) {
    const struct fake *f = buffer;
    *out = (struct ft_dmabuf){.width = 1920, .height = 1080, .format = 0x34325258, .modifier = 0, .n_planes = 1};
    out->stride[0] = 7680, out->fd[0] = f->fd;
    return true;
}
static void keep(void *buffer) { ++((struct fake *)buffer)->holds; }
static void release(void *buffer) { --((struct fake *)buffer)->holds; }
static const struct ft_share_buffers g_ops = {current, describe, keep, release};

static int holds(void) {
    int n = 0;
    for (int i = 0; i < 12; ++i) n += g_bufs[i].holds;
    return n;
}

static struct wl_event_loop *g_loop;

static void run(int how_long) {
    const long until = ms() + how_long;
    while (ms() < until) {
        wl_event_loop_dispatch(g_loop, 5);
        ft_share_tick();
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            ft_share_child(pid, status);
            note(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL ? "killed" : "reaped", pid, 0, 0);
        }
    }
}

static void present(int index, int buffer) {
    g_current[index] = &g_bufs[buffer];
    ft_share_presented(index);
}

static const char *ask(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static const char *ask(const char *fmt, ...) {
    static char reply[256];
    char c[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c, sizeof c, fmt, ap);
    va_end(ap);
    reply[0] = 0;
    return ft_share_command(c, reply, sizeof reply) ? reply : "not a share command";
}

// What the log says, in order.
struct event {
    char what[16];
    int pid;
    long a, b, t;
};
static struct event g_events[600];
static int g_nevents;

static void read_log(void) {
    g_nevents = 0;
    FILE *f = fopen(g_log, "r");
    while (f && g_nevents < 600) {
        struct event *e = &g_events[g_nevents];
        if (fscanf(f, "%15s %d %ld %ld %ld", e->what, &e->pid, &e->a, &e->b, &e->t) != 5) break;
        ++g_nevents;
    }
    if (f) fclose(f);
}

static int count(const char *what) {
    int n = 0;
    for (int i = 0; i < g_nevents; ++i) n += !strcmp(g_events[i].what, what);
    return n;
}

// The last event of that kind, or NULL.
static const struct event *last(const char *what) {
    for (int i = g_nevents - 1; i >= 0; --i)
        if (!strcmp(g_events[i].what, what)) return &g_events[i];
    return NULL;
}

typedef bool (*scenario)(void);
static bool check(const char *name, scenario s) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        g_loop = wl_event_loop_create();
        for (int i = 0; i < 12; ++i) g_bufs[i].fd = memfd_create("picture", MFD_CLOEXEC);
        ft_share_init(g_loop, &g_ops, &g_current);
        bool ok = s();
        ft_share_shutdown();
        run(300);
        if (holds() != 0) fprintf(stderr, "  %d buffers are still held at the end\n", holds()), ok = false;
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s: %s\n", name, ok ? "ok" : "FAILED");
    return ok;
}

#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) return fprintf(stderr, "  not so: %s\n", #c), false; \
    } while (0)
#define SAYS(reply, text) EXPECT(!strncmp(reply, text, strlen(text)))

static bool commands(void) {
    SAYS(ask("shares"), "ok 0");
    SAYS(ask("share 1 start 0 1280x720 30"), "error screen 1 has no picture");
    g_current[0] = &g_bufs[0], g_current[1] = &g_bufs[1];
    SAYS(ask("share 1 start 3 1280x720 30"), "error slot");
    SAYS(ask("share 1 start 0 1281x720 30"), "error slot");
    SAYS(ask("share 1 start 0 64x64 30"), "error slot");
    SAYS(ask("share 1 start 0 1280x720 0"), "error slot");
    SAYS(ask("share 1 start 0 1280x720"), "error share <N> start");
    SAYS(ask("share 1 dance"), "error share <N> start");
    SAYS(ask("share 1 stop"), "error screen 1 isn't shared");
    SAYS(ask("screens"), "not a share command");
    SAYS(ask("share 1 start 0 1280x720 30"), "ok");
    SAYS(ask("shares"), "ok 1 1:0:starting:1280x720");
    SAYS(ask("share 1 start 0 1280x720 30"), "ok running");
    SAYS(ask("share 2 start 0 1280x720 30"), "error panel 0 is screen 1");
    EXPECT(!ft_share_wanted(0));
    run(300);
    SAYS(ask("shares"), "ok 1 1:0:idle:1280x720");
    EXPECT(ft_share_wanted(0) && !ft_share_wanted(1));
    SAYS(ask("share 2 start 1 640x360 15"), "ok");
    run(300);
    SAYS(ask("shares"), "ok 2 1:0:idle:1280x720 2:1:idle:640x360");
    SAYS(ask("share 1 stop"), "ok");
    EXPECT(!ft_share_wanted(0));
    run(300);
    SAYS(ask("shares"), "ok 1 2:1:idle:640x360");
    read_log();
    EXPECT(count("start") == 2 && count("quit") + count("term") == 1 && count("reaped") == 1 && count("killed") == 0);
    return true;
}

// Each buffer is told once, with its fd, and each picture is kept only while it's read.
static bool pictures(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 60"), "ok");
    run(300);
    read_log();
    EXPECT(count("buffer") == 1 && count("frame") == 1);  // someone watches: the picture as it is, unasked
    EXPECT(last("buffer")->b == 100000 + 1920 && last("frame")->a == last("buffer")->a);
    for (int i = 0; i < 6; ++i) present(0, i % 2), run(40);
    run(100);
    read_log();
    EXPECT(count("buffer") == 2 && count("frame") == 7 && last("frame")->a == last("buffer")->a);
    EXPECT(holds() == 0);
    return true;
}

// At most the stream's rate of pictures goes, and the last one drawn does go.
static bool rate(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 10"), "ok");
    run(300);
    const long began = ms();
    for (int i = 0; i < 100; ++i) present(0, i % 2), run(10);
    const long took = ms() - began;
    present(0, 2);
    run(300);
    read_log();
    const int frames = count("frame") - 2;  // (but for the first and the last)
    EXPECT(frames >= took / 100 - 2 && frames <= took / 100 + 2);
    EXPECT(count("buffer") == 3 && last("frame")->a == last("buffer")->a);
    return true;
}

// A program that keeps a picture gets no other, and is ended and started again.
static bool kept(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 7"), "ok");
    run(200);
    for (int i = 0; i < 10; ++i) present(0, i % 3), run(20);
    read_log();
    EXPECT(count("frame") == 1 && holds() == 1 && g_bufs[0].holds == 1);
    run(700);
    read_log();
    EXPECT(holds() == 0 && count("term") + count("quit") == 1 && count("start") == 1);
    run(2200);
    read_log();
    EXPECT(count("start") == 2 && count("frame") == 2);
    return true;
}

// While nobody watches nothing goes; when someone does again, the buffers are told anew and the
// picture goes as it is.
static bool watched(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 60"), "ok");
    run(200);
    present(0, 1), run(50);
    read_log();
    EXPECT(count("frame") == 2 && count("buffer") == 2);
    const int pid = last("start")->pid;
    kill(pid, SIGUSR1);
    run(100);
    EXPECT(!ft_share_wanted(0));
    for (int i = 0; i < 6; ++i) present(0, i % 2), run(20);
    read_log();
    EXPECT(count("off") == 1 && count("frame") == 2 && holds() == 0);
    kill(pid, SIGUSR2);
    run(100);
    read_log();
    EXPECT(ft_share_wanted(0) && count("frame") == 3 && count("buffer") == 3 && last("buffer")->a == 3 && last("frame")->a == 3);
    kill(pid, SIGUSR2);  // asked once more: the picture once more
    run(100);
    read_log();
    EXPECT(count("frame") == 4 && count("buffer") == 3);
    return true;
}

// A buffer that goes is forgotten, and so is the oldest of more than the program keeps.
static bool forgotten(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 60"), "ok");
    run(200);
    present(0, 1), run(40);
    ft_share_forget(&g_bufs[0]);
    ft_share_forget(&g_bufs[5]);  // (one it never heard of)
    run(40);
    read_log();
    EXPECT(count("forget") == 1 && last("forget")->a == 1);
    present(0, 0), run(40);
    read_log();
    EXPECT(count("buffer") == 3 && last("buffer")->a == 3);
    for (int i = 2; i < 11; ++i) present(0, i), run(40);
    read_log();
    EXPECT(count("buffer") == 12 && count("forget") == 4 && count("frame") == 12 && holds() == 0);
    return true;
}

// A program that dies gives its picture back and is started again.
static bool died(void) {
    g_current[0] = &g_bufs[0];
    SAYS(ask("share 1 start 0 1280x720 7"), "ok");
    run(200);
    read_log();
    EXPECT(count("frame") == 1 && holds() == 1);
    kill(last("start")->pid, SIGKILL);
    run(200);
    EXPECT(holds() == 0 && !ft_share_wanted(0));
    SAYS(ask("shares"), "ok 1 1:0:lost:1280x720");
    run(2200);
    read_log();
    EXPECT(count("start") == 2 && count("killed") == 1 && count("frame") == 2);
    SAYS(ask("share 1 start 0 1280x720 30"), "ok");  // new settings: the old one goes first
    run(1000);
    read_log();
    EXPECT(count("start") == 3 && last("start")->b == 30 && last("start")->t >= last("reaped")->t);
    return true;
}

int main(int argc, char **argv) {
    if (argc > 8 && !strcmp(argv[1], "--panel")) {
        snprintf(g_log, sizeof g_log, "%s/events.log", getenv("XDG_RUNTIME_DIR"));
        return fake_program(argv);
    }
    char dir[] = "/tmp/share-test-XXXXXX", self[PATH_MAX];
    if (!mkdtemp(dir)) return perror("mkdtemp"), 2;
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return perror("readlink"), 2;
    self[n] = 0;
    setenv("FT_SHAREPANEL", self, 1);
    setenv("XDG_RUNTIME_DIR", dir, 1);  // ft-sharepanel's logs
    unsetenv("FT_TOGETHER_SOCKET");
    snprintf(g_log, sizeof g_log, "%s/events.log", dir);
    const struct { const char *name; scenario s; } all[] = {
        {"commands", commands}, {"pictures", pictures},   {"rate", rate}, {"a picture kept", kept},
        {"watched", watched},   {"forgotten", forgotten}, {"died", died},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof all / sizeof *all; ++i) {
        unlink(g_log);
        failed += !check(all[i].name, all[i].s);
    }
    char rm[PATH_MAX + 16];
    snprintf(rm, sizeof rm, "rm -rf %s", dir);
    if (system(rm)) {}
    return failed ? 1 : 0;
}
