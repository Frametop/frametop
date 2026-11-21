// screens/remote.c, offline: a display never has two ft-streams, and a host does one stream
// operation at a time. The real remote.c runs with SteamVR stubbed; this program is also the
// ft-stream it starts ($FT_STREAM): it says it's live, and asked to end it takes 300 ms to go
// (Disconnect Monitor), or with the app "hang" it ignores SIGTERM. It notes the "focus" and
// "blur" it's told (the shared clipboard's moments), and each "move" and "leave" (the pointer
// coming onto its panel and going). With the app window:0x1 the host has no
// such window, with window:0x2 it's full: either way it says why and ends. window:0x3 goes live
// and ends 100 ms later (its window closed on the host). It connects to nothing.
// And it is the ft-peerpanel remote.c starts for a panel of a Together room ($FT_PEERPANEL):
// the same, with "room" (or the room's socket it was given) for its host.
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
static char g_named[256];   // the last ft_vr_remote_named: "INDEX HOST APP" (the index is the number - 1)
void ft_vr_remote_named(int i, const char *h, const char *a) { snprintf(g_named, sizeof g_named, "%d %s %s", i, h, a); }

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

static int fake_stream(const char *host, const char *app) {
    note("start", getpid(), host, app);
    if (!strncmp(app, "window:", 7)) {  // as ft-stream does when the host's open fails
        const char *why = !strcmp(app, "window:0x1") ? "state lost 404 no window 0x1" : "state lost 503 at most 4 windows";
        send(3, why, strlen(why), MSG_NOSIGNAL);
        return 1;
    }
    if (!strcmp(app, "window:0x3")) {
        send(3, "state live via network", 22, MSG_NOSIGNAL);
        usleep(100000);
        note("exit", getpid(), host, app);
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
        if (!strcmp(m, "focus") || !strcmp(m, "blur") || !strcmp(m, "leave")) note(m, getpid(), host, app);
        else if (!strncmp(m, "move ", 5)) note("move", getpid(), host, app);
        else if (!strncmp(m, "button ", 7)) note(m[n - 1] == '1' ? "press" : "raise", getpid(), host, app);
        else if (!strncmp(m, "scroll ", 7)) note("scroll", getpid(), host, app);
        else if (!strncmp(m, "key ", 4)) note("key", getpid(), host, app);
        else if (!strncmp(m, "peerkey ", 8)) note("Key", getpid(), host, app);
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
    // f: focus, b: blur, m: move, l: leave, p and r: a button pressed and raised, s: scroll,
    // k: key, K: peerkey, in order
    char said[32];
};
static struct proc g_procs[16];
static int g_nprocs;

static struct proc *proc_of(int pid) {
    for (int i = 0; i < g_nprocs; ++i)
        if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

static const char *spaced(const char *word) {
    static char s[20];
    snprintf(s, sizeof s, " %s ", word);
    return s;
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
        else if (p && strstr(" focus blur move leave press raise scroll key Key ", spaced(what)) && strlen(p->said) < sizeof p->said - 1)
            p->said[strlen(p->said)] = what[0];
    }
    if (f) fclose(f);
}

// One scenario in a process of its own, so g_remotes starts empty: 0 when it held.
typedef bool (*scenario)(void);
// Whether an ft-stream started after "down" in the log (ft_remote_shutdown).
static bool started_after_shutdown(void) {
    FILE *f = fopen(g_log, "r");
    char what[16], host[64], app[64];
    int pid;
    long t;
    bool down = false, started = false;
    while (f && fscanf(f, "%15s %d %63s %63s %ld", what, &pid, host, app, &t) == 5) {
        if (!strcmp(what, "down")) down = true;
        else if (down && !strcmp(what, "start")) started = true;
    }
    if (f) fclose(f);
    return started;
}

static bool check(const char *name, scenario s) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        g_loop = wl_event_loop_create();
        ft_remote_init(g_loop, true);
        bool ok = s();
        ft_remote_shutdown();
        note("down", 0, "-", "-");
        run(1500);
        if (started_after_shutdown()) {  // it would outlive ft-screens
            fprintf(stderr, "  an ft-stream started after ft_remote_shutdown\n");
            ok = false;
        }
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

// Its dashboard tile's key is what it shows: named once the slot is filled in (it was "?/?" when
// read before, 2026-11-16), and again when new settings point it at another display.
static bool named(void) {
    start(101, "192.0.2.7", "display:X", 60);
    EXPECT(!strcmp(g_named, "100 192.0.2.7 display:X"));
    start(101, "192.0.2.7", "display:Y", 60);
    EXPECT(!strcmp(g_named, "100 192.0.2.7 display:Y"));
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

// A panel of a Together room: ft-peerpanel is told its panel, and the room's socket when that
// isn't the usual one. It waits for an old program of its own screen, and for nothing else:
// not another room panel's, not a host's queue.
static bool room_panels(void) {
    char reply[256] = "";
    cmd("remote 116 peer 9 1280x720 1.5 Screen 2");
    start(101, "192.0.2.7", "hang", 60);
    run(300);
    EXPECT(ft_remote_is(115) && ft_remote_command("remotes", reply, sizeof reply) &&
           !strcmp(reply, "ok 2 116:peer-9:live:1280x720 101:frame:live:1920x1080"));
    cmd("remote 116 peer 9 1280x720 1.5 Screen 2");   // the same again: nothing starts over
    cmd("remote 101 stop");                           // it hangs on its way out, for FT_REMOTE_QUIT_MS
    cmd("remote 116 stop");
    setenv("FT_TOGETHER_SOCKET", "ft_together_test", 1);
    cmd("remote 115 peer 10 1920x804 2.0");
    cmd("remote 116 peer 9 1920x1080 1.5 Screen 2");
    run(1200);
    read_log();
    EXPECT(g_nprocs == 4);
    const struct proc *first = NULL, *other = NULL, *again = NULL;
    for (int i = 0; i < g_nprocs; ++i) {
        const struct proc *q = &g_procs[i];
        if (!strcmp(q->app, "peer:9") && !strcmp(q->host, "room")) first = q;
        if (!strcmp(q->app, "peer:10") && !strcmp(q->host, "ft_together_test")) other = q;
        if (!strcmp(q->app, "peer:9") && !strcmp(q->host, "ft_together_test")) again = q;
    }
    EXPECT(first && other && again);
    EXPECT(first->term && first->gone && !first->killed);
    EXPECT(other->start < first->gone);    // another panel doesn't wait for it
    EXPECT(again->start >= first->gone);   // its own screen's next program does
    EXPECT(ft_remote_command("remotes", reply, sizeof reply) && !strcmp(reply, "ok 2 115:peer-10:live:1920x804 116:peer-9:live:1920x1080"));
    return true;
}

// What isn't a room's panel is refused, and an app can't be said to be one.
static bool room_panel_refused(void) {
    const char *bad[] = {"remote 116 peer x 1280x720 1.5", "remote 116 peer 9 1280x720", "remote 116 peer -1 1280x720 1.5",
                         "remote 116 peer 256 1280x720 1.5", "remote 116 peer 9 32x720 1.5", "remote 116 peer 9 1280x720 0.01",
                         "remote 117 peer 9 1280x720 1.5", "remote 116 start frame 192.0.2.7 peer:9 1920x1080 60 0 1.0"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        char reply[256] = "";
        EXPECT(ft_remote_command(bad[i], reply, sizeof reply) && !strncmp(reply, "error", 5));
    }
    run(200);
    read_log();
    EXPECT(g_nprocs == 0 && !ft_remote_is(115));
    return true;
}

// The pointer leaving a room's panel is told to its program, once: by the leave itself, by the
// pointer turning up on another remote screen, and by its turning up on a screen of the
// desktop's. A remote display's stream is told of no leave.
static bool pointer_leaves(void) {
    cmd("remote 116 peer 9 1280x720 1.5");
    cmd("remote 115 peer 10 1280x720 1.5");
    start(101, "192.0.2.7", "display:X", 60);
    run(300);
    struct ft_event e = {.type = FT_MOTION, .screen = 115, .x = 5, .y = 5};
    ft_remote_event(&e);  // onto panel 9
    e.type = FT_LEAVE;
    ft_remote_event(&e);  // and off it
    ft_remote_event(&e);  // (said twice: told once)
    e.type = FT_MOTION;
    ft_remote_event(&e);  // onto it again
    e.screen = 114;
    ft_remote_event(&e);  // onto panel 10: 9 hears it left
    e.type = FT_LEAVE, e.screen = 115;
    ft_remote_event(&e);  // a late leave from 9: nothing, the pointer is on 10
    e.type = FT_MOTION, e.screen = 114;
    ft_remote_event(&e);  // ...where it still is
    e.type = FT_SCROLL, e.screen = 100, e.dy = 1;
    ft_remote_event(&e);  // onto the remote display: 10 hears it left
    e.type = FT_LEAVE;
    ft_remote_event(&e);  // off the remote display: its stream isn't told
    e.type = FT_MOTION, e.screen = 114;
    ft_remote_event(&e);  // back onto panel 10
    e.type = FT_BUTTON, e.screen = 115, e.button = 0x110, e.pressed = true;
    ft_remote_event(&e);  // a click on 9 (a move, then the button): 10 hears it left
    ft_remote_pointer_away();  // onto a screen of the desktop's
    ft_remote_pointer_away();
    run(300);
    read_log();
    EXPECT(g_nprocs == 3);
    const struct proc *nine = NULL, *ten = NULL, *display = NULL;
    for (int i = 0; i < g_nprocs; ++i) {
        const struct proc *q = &g_procs[i];
        if (!strcmp(q->app, "peer:9")) nine = q;
        if (!strcmp(q->app, "peer:10")) ten = q;
        if (!strcmp(q->app, "display:X")) display = q;
    }
    EXPECT(nine && ten && display);
    EXPECT(!strcmp(nine->said, "mlmlmpl"));
    EXPECT(!strcmp(ten->said, "mmlml"));
    EXPECT(!strcmp(display->said, "s"));
    return true;
}

// Someone else of a Together room on a window of the wearer's PC (peerinput.h): their pointer
// and keys go to its stream, a key in a word of its own, and only to a display whose stream
// runs: not to a room's panel or a number that is none. Where the wearer's pointer is and what
// the wearer holds stay as they were.
static bool someone_else(void) {
    start(101, "192.0.2.7", "display:X", 60);
    cmd("remote 116 peer 9 1280x720 1.5");
    run(300);
    int w = 0, h = 0;
    EXPECT(ft_remote_peer_size(100, &w, &h) && w == 1920 && h == 1080);
    EXPECT(!ft_remote_peer_size(115, &w, &h) && !ft_remote_peer_size(101, &w, &h) && !ft_remote_peer_size(0, &w, &h));
    struct ft_event e = {.type = FT_MOTION, .screen = 115, .x = 5, .y = 5};
    ft_remote_event(&e);  // the wearer's pointer is on the room's panel
    EXPECT(!ft_remote_held());
    struct ft_event theirs = {.type = FT_MOTION, .screen = 100, .x = 9, .y = 9};
    ft_remote_peer_event(&theirs);
    theirs.type = FT_BUTTON, theirs.button = 0x110, theirs.pressed = true;
    ft_remote_peer_event(&theirs);  // (a move first: the host's pointer may have been elsewhere)
    EXPECT(!ft_remote_held());      // theirs isn't the wearer's
    e.type = FT_BUTTON, e.screen = 100, e.button = 0x110, e.pressed = true;
    ft_remote_event(&e);  // the wearer presses there: the room's panel hears the pointer left
    theirs.pressed = false;
    ft_remote_peer_event(&theirs);
    EXPECT(ft_remote_held());  // ...and the wearer's isn't theirs
    theirs.type = FT_SCROLL, theirs.dy = 1;
    ft_remote_peer_event(&theirs);
    ft_remote_peer_key(100, 30, true), ft_remote_peer_key(100, 30, false);
    ft_remote_key(100, 30, true);
    theirs.screen = 115, ft_remote_peer_event(&theirs), ft_remote_peer_key(115, 30, true);  // a room's panel
    theirs.screen = 7, ft_remote_peer_event(&theirs), ft_remote_peer_key(7, 30, true);      // no remote screen
    e.pressed = false;
    ft_remote_event(&e);
    EXPECT(!ft_remote_held());
    start(102, "192.0.2.8", "display:Y", 60);
    EXPECT(!ft_remote_peer_size(101, &w, &h));  // its stream hasn't started yet
    run(300);
    EXPECT(ft_remote_peer_size(101, &w, &h));
    read_log();
    const struct proc *nine = NULL, *display = NULL;
    for (int i = 0; i < g_nprocs; ++i) {
        if (!strcmp(g_procs[i].app, "peer:9")) nine = &g_procs[i];
        if (!strcmp(g_procs[i].app, "display:X")) display = &g_procs[i];
    }
    EXPECT(nine && display);
    EXPECT(!strcmp(display->said, "mmpmpmrsKKkmr"));
    EXPECT(!strcmp(nine->said, "ml"));
    return true;
}

// A window panel's stream that ended: tried again after 0.2 s, not 2 s (the host's 404 then
// takes the panel). A display's waits the usual 2 s.
static bool window_retry(void) {
    start(101, "192.0.2.7", "window:0x3", 60);
    run(900);
    read_log();
    EXPECT(g_nprocs >= 2 && g_procs[1].start - g_procs[0].gone < 600);
    return true;
}

// Apple High Performance Screen Sharing apps ("apple:virtual@2") accept '@' and run ft-appless
// ($FT_APPLESS), following the same one-stream-per-display and host_busy rules.
static bool apple_backend(void) {
    start(101, "192.0.2.7", "apple:virtual@2", 60);
    run(300);
    cmd("remote 101 stop");
    start(101, "192.0.2.7", "apple:physical", 60);
    run(1200);
    read_log();
    EXPECT(g_nprocs == 2);
    EXPECT(!strcmp(g_procs[0].app, "apple:virtual@2") && !strcmp(g_procs[1].app, "apple:physical"));
    EXPECT(g_procs[1].start >= g_procs[0].gone);
    return true;
}

int main(int argc, char **argv) {
    if (argc > 8 && !strcmp(argv[1], "stream")) {
        snprintf(g_log, sizeof g_log, "%s/events.log", getenv("XDG_RUNTIME_DIR"));
        return fake_stream(argv[2], argv[8]);
    }
    if (argc > 4 && !strcmp(argv[1], "--panel") && !strcmp(argv[3], "--fd") && !strcmp(argv[4], "3")) {
        char app[64];
        snprintf(app, sizeof app, "peer:%s", argv[2]);
        snprintf(g_log, sizeof g_log, "%s/events.log", getenv("XDG_RUNTIME_DIR"));
        return fake_stream(argc == 7 && !strcmp(argv[5], "--socket") ? argv[6] : argc == 5 ? "room" : "odd", app);
    }
    char dir[] = "/tmp/remote-test-XXXXXX", self[PATH_MAX];
    if (!mkdtemp(dir)) return perror("mkdtemp"), 2;
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return perror("readlink"), 2;
    self[n] = 0;
    setenv("FT_STREAM", self, 1);
    setenv("FT_APPLESS", self, 1);
    setenv("FT_PEERPANEL", self, 1);
    unsetenv("FT_TOGETHER_SOCKET");
    setenv("XDG_RUNTIME_DIR", dir, 1);  // ft-stream's logs
    snprintf(g_log, sizeof g_log, "%s/events.log", dir);
    const struct { const char *name; scenario s; } all[] = {
        {"stop then start", stop_start}, {"new settings", new_settings}, {"same host", same_host},
        {"other host", other_host},      {"hung ft-stream", hung},       {"focus and blur", focus_blur},
        {"window closed", window_closed}, {"window retry", window_retry}, {"room panels", room_panels},
        {"room panel refused", room_panel_refused}, {"pointer leaves", pointer_leaves}, {"someone else", someone_else},
        {"named for its tile", named},
        {"apple backend", apple_backend},
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
