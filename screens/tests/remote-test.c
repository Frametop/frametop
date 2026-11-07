// screens/remote.c, offline: a display never has two ft-streams, and a host does one stream
// operation at a time. The real remote.c runs with SteamVR stubbed; this program is also the
// ft-stream it starts ($FT_STREAM): it says it's live, and asked to end it takes 300 ms to go
// (Disconnect Monitor), or with the app "hang" it ignores SIGTERM. It notes the "focus" and
// "blur" it's told (the shared clipboard's moments). With the app window:0x1 the host has no
// such window, with window:0x2 it's full: either way it says why and ends. It connects to nothing.
// Built by `just test-c` (justfile): remote.c with -DFT_REMOTE_QUIT_MS=600, so the SIGKILL comes soon.
#define _GNU_SOURCE
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "remote.h"

int ft_vr_modifiers(uint32_t f, uint64_t *o, int m) { (void)f, (void)o, (void)m; return 0; }
bool ft_vr_screen_present(int i, const void *k, const struct ft_dmabuf *b) { (void)i, (void)k, (void)b; return true; }
void ft_vr_forget(const void *k) { (void)k; }
enum ft_attention ft_vr_screen_attention(int i) { (void)i; return FT_HIDDEN; }
static int g_destroyed;
void ft_vr_screen_destroy(int i) { (void)i, ++g_destroyed; }
bool ft_vr_remote_create(int i, const char *l, double m, bool *r) { (void)i, (void)l, (void)m; *r = false; return true; }

static char g_log[PATH_MAX];

static long ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void note(const char *what, int pid, const char *host, const char *app) {
    FILE *f = fopen(g_log, "a");
    fprintf(f, "%s %d %s %s %ld\n", what, pid, host, app, ms());
    fclose(f);
}

// --- the fake ft-stream: ARGV as start_stream gives it ---
static volatile sig_atomic_t g_term;
static void on_term(int s) { (void)s, g_term = 1; }

static int fake_stream(char **argv) {
    const char *host = argv[2], *app = argv[8];
    note("start", getpid(), host, app);
    if (!strncmp(app, "window:", 7)) {  // as ft-stream does when the host's open fails
        const char *why = !strcmp(app, "window:0x1") ? "state lost 404 no window 0x1" : "state lost 503 at most 4 windows";
        send(3, why, strlen(why), MSG_NOSIGNAL);
        return 1;
    }
    if (!strcmp(app, "hang")) signal(SIGTERM, SIG_IGN), alarm(10);  // gone in the end, even if nothing kills it
    else signal(SIGTERM, on_term);
    send(3, "state live via network", 22, MSG_NOSIGNAL);
    bool open = true;
    while (!g_term) {
        // A short wait, not pause(): a SIGTERM just before it would be missed.
        struct pollfd p = {open ? 3 : -1, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        char m[256];
        const ssize_t n = recv(3, m, sizeof m - 1, 0);
        if (n <= 0) {
            open = false;
            continue;
        }
        m[n] = 0;
        if (!strcmp(m, "focus") || !strcmp(m, "blur")) note(m, getpid(), host, app);
    }
    note("term", getpid(), host, app);
    usleep(300000);  // its Disconnect Monitor
    note("exit", getpid(), host, app);
    return 0;
}

// --- ft-screens' side ---
static struct wl_event_loop *g_loop;

static void run(int how_long) {
    const long until = ms() + how_long;
    while (ms() < until) {
        wl_event_loop_dispatch(g_loop, 10);
        ft_remote_tick();
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            ft_remote_child(pid, status);
            note(WIFSIGNALED(status) ? "killed" : "reaped", pid, "-", "-");
        }
    }
}

static void cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cmd(const char *fmt, ...) {
    char c[256], reply[256] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c, sizeof c, fmt, ap);
    va_end(ap);
    if (!ft_remote_command(c, reply, sizeof reply) || strncmp(reply, "ok", 2)) {
        fprintf(stderr, "%s -> %s\n", c, reply);
        exit(2);
    }
}

static void start(int n, const char *host, const char *app, int fps) {
    cmd("remote %d start frame %s %s 1920x1080 %d 0 1.0 test", n, host, app, fps);
}

// What the log says, in order: each ft-stream (by pid) with its host, app and times.
struct proc {
    int pid;
    char host[64], app[64];
    long start, term, gone;  // 0: not yet
    bool killed;
    char said[32];  // f: focus, b: blur, in order
};
static struct proc g_procs[16];
static int g_nprocs;

static struct proc *proc_of(int pid) {
    for (int i = 0; i < g_nprocs; ++i)
        if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

static void read_log(void) {
    g_nprocs = 0;
    FILE *f = fopen(g_log, "r");
    char what[16], host[64], app[64];
    int pid;
    long t;
    while (f && fscanf(f, "%15s %d %63s %63s %ld", what, &pid, host, app, &t) == 5) {
        struct proc *p = proc_of(pid);
        if (!strcmp(what, "start") && !p && g_nprocs < 16) {
            p = &g_procs[g_nprocs++];
            *p = (struct proc){.pid = pid, .start = t};
            snprintf(p->host, sizeof p->host, "%s", host), snprintf(p->app, sizeof p->app, "%s", app);
        } else if (p && !strcmp(what, "term")) p->term = t;
        else if (p && (!strcmp(what, "reaped") || !strcmp(what, "killed"))) p->gone = t, p->killed = what[0] == 'k';
        else if (p && (!strcmp(what, "focus") || !strcmp(what, "blur")) && strlen(p->said) < sizeof p->said - 1)
            p->said[strlen(p->said)] = what[0];
    }
    if (f) fclose(f);
}

// One scenario in a process of its own, so g_remotes starts empty: 0 when it held.
typedef bool (*scenario)(void);
static bool check(const char *name, scenario s) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        g_loop = wl_event_loop_create();
        ft_remote_init(g_loop, true);
        const bool ok = s();
        ft_remote_shutdown();
        run(1500);
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

// Stopped and started again at once (ft-layout's route change, Connected off and on): the new
// ft-stream waits for the old one's exit.
static bool stop_start(void) {
    start(101, "192.0.2.7", "display:X", 60);
    run(300);
    cmd("remote 101 stop");
    start(101, "192.0.2.7", "display:X", 60);
    run(1200);
    read_log();
    EXPECT(g_nprocs == 2);
    EXPECT(g_procs[0].term && g_procs[0].gone && !g_procs[0].killed);
    EXPECT(g_procs[1].start >= g_procs[0].gone);
    return true;
}

// New settings in one command: the same slot starts over once the old one has gone.
static bool new_settings(void) {
    start(101, "192.0.2.7", "display:X", 60);
    run(300);
    start(101, "192.0.2.7", "display:X", 30);
    run(1200);
    read_log();
    EXPECT(g_nprocs == 2);
    EXPECT(g_procs[1].start >= g_procs[0].gone);
    return true;
}

// Another display of the host waits while one of the host's streams releases its Remote Monitor.
static bool same_host(void) {
    start(101, "192.0.2.7", "display:X", 60);
    start(102, "192.0.2.7", "display:Y", 60);
    run(300);
    cmd("remote 101 stop");
    start(103, "192.0.2.7", "display:Z", 60);
    run(1200);
    read_log();
    EXPECT(g_nprocs == 3);
    EXPECT(!strcmp(g_procs[2].app, "display:Z") && g_procs[2].start >= g_procs[0].gone);
    EXPECT(!g_procs[1].term);  // the other display streams on
    return true;
}

// Another host's display doesn't wait for it.
static bool other_host(void) {
    start(101, "192.0.2.7", "display:X", 60);
    run(300);
    cmd("remote 101 stop");
    start(102, "192.0.2.8", "display:X", 60);
    run(1200);
    read_log();
    EXPECT(g_nprocs == 2);
    EXPECT(g_procs[1].start < g_procs[0].gone);
    return true;
}

// An ft-stream that ignores SIGTERM is killed after FT_REMOTE_QUIT_MS, and then the new one starts.
static bool hung(void) {
    start(101, "192.0.2.7", "hang", 60);
    run(300);
    const long stopped = ms();
    cmd("remote 101 stop");
    start(101, "192.0.2.7", "display:X", 60);
    run(FT_REMOTE_QUIT_MS + 1000);
    read_log();
    EXPECT(g_nprocs == 2);
    EXPECT(g_procs[0].killed && g_procs[0].gone - stopped >= FT_REMOTE_QUIT_MS);
    EXPECT(g_procs[1].start >= g_procs[0].gone);
    return true;
}

// Typing going to a remote screen and leaving it reaches its ft-stream, and only its own.
static bool focus_blur(void) {
    start(101, "192.0.2.7", "display:X", 60);
    start(102, "192.0.2.8", "display:Y", 60);
    run(300);
    EXPECT(ft_remote_is(100) && ft_remote_is(101));
    ft_remote_focus(100);
    ft_remote_focus(100);  // every click says it
    run(100);
    ft_remote_blur(100);
    ft_remote_focus(101);
    ft_remote_focus(7);  // not a remote screen: nothing
    run(300);
    read_log();
    EXPECT(g_nprocs == 2);
    // Either may have logged its start first.
    const struct proc *x = !strcmp(g_procs[0].app, "display:X") ? &g_procs[0] : &g_procs[1];
    const struct proc *y = x == &g_procs[0] ? &g_procs[1] : &g_procs[0];
    EXPECT(!strcmp(x->app, "display:X") && !strcmp(y->app, "display:Y"));
    EXPECT(!strcmp(x->said, "ffb"));
    EXPECT(!strcmp(y->said, "f"));
    return true;
}

// A window closed on the host: its panel goes instead of trying again. A full host is worth
// the usual retry.
static bool window_closed(void) {
    start(101, "192.0.2.7", "window:0x1", 60);
    start(102, "192.0.2.8", "window:0x2", 60);
    run(2600);
    read_log();
    int gone = 0, full = 0;
    for (int i = 0; i < g_nprocs; ++i) gone += !strcmp(g_procs[i].app, "window:0x1"), full += !strcmp(g_procs[i].app, "window:0x2");
    EXPECT(gone == 1 && !ft_remote_is(100) && g_destroyed == 1);
    EXPECT(full == 2 && ft_remote_is(101));
    char reply[256] = "";
    EXPECT(ft_remote_command("remotes", reply, sizeof reply) && !strncmp(reply, "ok 1 102:", 9));
    return true;
}

int main(int argc, char **argv) {
    if (argc > 8 && !strcmp(argv[1], "stream")) {
        snprintf(g_log, sizeof g_log, "%s/events.log", getenv("XDG_RUNTIME_DIR"));
        return fake_stream(argv);
    }
    char dir[] = "/tmp/remote-test-XXXXXX", self[PATH_MAX];
    if (!mkdtemp(dir)) return perror("mkdtemp"), 2;
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return perror("readlink"), 2;
    self[n] = 0;
    setenv("FT_STREAM", self, 1);
    setenv("XDG_RUNTIME_DIR", dir, 1);  // ft-stream's logs
    snprintf(g_log, sizeof g_log, "%s/events.log", dir);
    const struct { const char *name; scenario s; } all[] = {
        {"stop then start", stop_start}, {"new settings", new_settings}, {"same host", same_host},
        {"other host", other_host},      {"hung ft-stream", hung},       {"focus and blur", focus_blur},
        {"window closed", window_closed},
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
