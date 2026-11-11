// Shared screens: a screen's pictures, as KWin draws them, also go to an ft-sharepanel
// (together/ft-sharepanel.cpp), which encodes them for the people of a Together room
// (docs/together.md). It is a program of its own, started here with one end of a SOCK_SEQPACKET
// socket pair as its fd 3, so a stalled encoder holds up nothing here. It hears of each of the
// screen's buffers once, as DMA-BUFs, and then "frame ID" when one holds a new picture; that
// buffer is kept as it is until it says "done". One picture is out at a time, and at most the
// stream's rate of them: what KWin draws meanwhile waits, and only the newest goes. While
// nobody watches the panel (it says "want 0") nothing goes at all. The words are in
// together/media/screenfeed.h.
//
// Commands (on @ft_screens, from ft-roompanels):
//   share <N> start <slot> <W>x<H> <fps>
//       screen N's pictures go to the room as this Frame's panel <slot> (0 to FT_SHARE_MAX - 1),
//       scaled to WxH. Again with the same settings: nothing changes; with others, it starts over.
//   share <N> stop
//   shares  -> "ok <count> <N>:<slot>:<state>:<W>x<H> ..."   (state: starting, idle, live, lost)
// An ft-sharepanel that ends starts again after 2 s, then longer after quick failures (up to
// 30 s). One that keeps a picture for 2 s is ended, and so started again. It logs to
// $XDG_RUNTIME_DIR/frametop-share-<slot>.log.
#define _GNU_SOURCE
#include "share.h"

#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WLR_USE_UNSTABLE
#include <wlr/util/log.h>

#define MOST_TOLD 8        // buffers of the screen's the program knows (KWin cycles through a few)
#ifndef FT_SHARE_STUCK_MS
#define FT_SHARE_STUCK_MS 2000  // a picture kept this long: the program is ended
#endif
#ifndef FT_SHARE_QUIT_MS
#define FT_SHARE_QUIT_MS 3000   // asked to end, it gets this long before SIGKILL
#endif
#define WAIT_MAX_MS 30000

struct told {
    void *buffer;  // NULL: free
    unsigned id;
};

struct share {
    bool used;
    int index, slot, width, height, fps;
    pid_t pid;  // ft-sharepanel, 0 when not running
    int fd;     // our end of its socket, -1 when not connected
    struct wl_event_source *source;
    char state[96];  // as it last said
    bool wanted;     // someone watches the panel
    bool fresh;      // the screen has a picture the program hasn't had
    struct told told[MOST_TOLD];
    unsigned next_id;
    int next_told;   // where the next buffer is noted once all are taken
    void *out;       // the buffer it reads now (kept), NULL none
    unsigned out_id;
    uint32_t out_ms, sent_ms;
    uint32_t restart_at, started_ms, quit_at;  // ms, 0: not waiting
    int failures;
};

static struct share g_shares[FT_SHARE_MAX];
static struct wl_event_loop *g_loop;
static const struct ft_share_buffers *g_buffers;
static void *g_buffers_data;
static char g_program[PATH_MAX];  // ft-sharepanel (found at start: a rebuild replaces our own file)

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static struct share *find(int index) {
    for (int i = 0; i < FT_SHARE_MAX; ++i)
        if (g_shares[i].used && g_shares[i].index == index) return &g_shares[i];
    return NULL;
}

static bool say(struct share *s, const int *fds, int nfds, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static bool say(struct share *s, const int *fds, int nfds, const char *fmt, ...) {
    if (s->fd < 0) return false;
    char msg[400];
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    if (n <= 0 || n >= (int)sizeof msg) return false;
    struct iovec iov = {msg, (size_t)n};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    char control[CMSG_SPACE(sizeof(int) * 4)] = {0};
    if (nfds > 0) {
        m.msg_control = control, m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    return sendmsg(s->fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) == n;
}

// The picture it reads is ours again.
static void take_back(struct share *s) {
    if (s->out) g_buffers->release(s->out);
    s->out = NULL;
}

// It knows none of the screen's buffers any more (it said so, or it went).
static void forget_all(struct share *s) {
    take_back(s);
    memset(s->told, 0, sizeof s->told);
    s->next_told = 0;
}

static void disconnect(struct share *s) {
    if (s->source) wl_event_source_remove(s->source);
    s->source = NULL;
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->wanted = false;
    forget_all(s);
}

// The id it knows `buffer` by, telling it of the buffer first when it doesn't; 0 when it can't be told.
static unsigned id_of(struct share *s, void *buffer) {
    struct told *at = NULL;
    for (int i = 0; i < MOST_TOLD; ++i) {
        if (s->told[i].buffer == buffer) return s->told[i].id;
        if (!s->told[i].buffer && !at) at = &s->told[i];
    }
    struct ft_dmabuf b;
    if (!g_buffers->describe(buffer, &b) || b.n_planes < 1 || b.n_planes > 4) return 0;
    if (!at) {  // more buffers than it keeps: the one told longest ago makes room
        at = &s->told[s->next_told];
        s->next_told = (s->next_told + 1) % MOST_TOLD;
        say(s, NULL, 0, "forget %u", at->id);
        at->buffer = NULL;
    }
    char planes[160];
    int n = 0;
    for (int i = 0; i < b.n_planes; ++i) n += snprintf(planes + n, sizeof planes - (size_t)n, " %u %u", b.offset[i], b.stride[i]);
    const unsigned id = ++s->next_id ? s->next_id : ++s->next_id;
    if (!say(s, b.fd, b.n_planes, "buffer %u %d %d %x %llx %d%s", id, b.width, b.height, b.format, (unsigned long long)b.modifier,
             b.n_planes, planes))
        return 0;
    at->buffer = buffer, at->id = id;
    return id;
}

// Sends the screen's picture, if it has one the program hasn't had and its turn has come.
static void pump(struct share *s) {
    if (!s->wanted || !s->fresh || s->out || s->fd < 0) return;
    const uint32_t t = now_ms();
    if (s->sent_ms && t - s->sent_ms + 2 < 1000u / (unsigned)s->fps) return;  // (the tick sends it)
    void *buffer = g_buffers->current(g_buffers_data, s->index);
    if (!buffer) return;
    const unsigned id = id_of(s, buffer);
    if (!id) return;
    g_buffers->keep(buffer);
    if (!say(s, NULL, 0, "frame %u", id)) {
        g_buffers->release(buffer);
        return;
    }
    s->out = buffer, s->out_id = id, s->out_ms = s->sent_ms = t ? t : 1;
    s->fresh = false;
}

static void message(struct share *s, const char *msg) {
    unsigned id;
    if (strcmp(msg, "want 1") == 0) {
        s->wanted = s->fresh = true;  // the picture as it is now, whether or not it has had it
        s->sent_ms = 0;
        pump(s);
    } else if (strcmp(msg, "want 0") == 0) {
        s->wanted = false;
        forget_all(s);
    } else if (sscanf(msg, "done %u", &id) == 1) {
        if (s->out && s->out_id == id) take_back(s), pump(s);
    } else if (strncmp(msg, "state ", 6) == 0) {
        snprintf(s->state, sizeof s->state, "%.95s", msg + 6);
        wlr_log(WLR_INFO, "share %d (panel %d): %s", s->index + 1, s->slot, s->state);
        if (strncmp(s->state, "live", 4) == 0) s->failures = 0;
    }
}

static int readable(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct share *s = data;
    for (;;) {
        char buf[256];
        const ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) {  // it is gone: its exit (SIGCHLD) sets up the restart
            wlr_log(WLR_INFO, "share %d: ft-sharepanel closed its socket", s->index + 1);
            disconnect(s);
            return 0;
        }
        buf[n] = 0;
        message(s, buf);
    }
    return 0;
}

static void log_path(const struct share *s, char *out, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(out, size, "%s/frametop-share-%d.log", runtime && *runtime ? runtime : "/tmp", s->slot);
}

static bool start_program(struct share *s) {
    s->restart_at = s->quit_at = 0;
    s->started_ms = now_ms();
    snprintf(s->state, sizeof s->state, "lost");
    if (!*g_program) return false;
    char log[PATH_MAX], slot[8], size[32], fps[8];
    log_path(s, log, sizeof log);
    snprintf(slot, sizeof slot, "%d", s->slot);
    snprintf(size, sizeof size, "%dx%d", s->width, s->height);
    snprintf(fps, sizeof fps, "%d", s->fps);
    const char *socket = getenv("FT_TOGETHER_SOCKET");  // ft-together's, when it isn't the usual one
    char *argv[] = {g_program, "--panel", slot, "--size", size, "--fps", fps, "--fd", "3", NULL, NULL, NULL};
    if (socket && *socket) argv[9] = "--socket", argv[10] = (char *)socket;
    const int fd = ft_child_spawn(argv, log, &s->pid);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "share %d: can't run %s: %s", s->index + 1, g_program, strerror(errno));
        s->pid = 0;
        return false;
    }
    s->fd = fd;
    s->source = wl_event_loop_add_fd(g_loop, fd, WL_EVENT_READABLE, readable, s);
    snprintf(s->state, sizeof s->state, "starting");
    wlr_log(WLR_INFO, "share %d: started ft-sharepanel (pid %d) for panel %d, %s at %d fps (log %s)", s->index + 1, s->pid, s->slot,
            size, s->fps, log);
    return true;
}

static void stop_program(struct share *s) {
    say(s, NULL, 0, "quit");
    disconnect(s);
    if (s->pid <= 0) return;
    kill(s->pid, SIGTERM);
    if (!s->quit_at) s->quit_at = now_ms() | 1;
}

static void schedule_restart(struct share *s) {
    const uint32_t ran = now_ms() - s->started_ms;
    s->failures = ran < 20000 ? s->failures + 1 : 0;
    uint32_t wait = 2000;
    for (int k = 1; k < s->failures && wait < WAIT_MAX_MS; ++k) wait *= 2;
    if (wait > WAIT_MAX_MS) wait = WAIT_MAX_MS;
    s->restart_at = (now_ms() + wait) | 1;
    wlr_log(WLR_INFO, "share %d: starting it again in %.1f s", s->index + 1, wait / 1000.0);
}

void ft_share_init(struct wl_event_loop *loop, const struct ft_share_buffers *buffers, void *data) {
    g_loop = loop;
    g_buffers = buffers;
    g_buffers_data = data;
    if (!ft_child_path("FT_SHAREPANEL", "together", "ft-sharepanel", g_program, sizeof g_program)) g_program[0] = 0;
    for (int i = 0; i < FT_SHARE_MAX; ++i) g_shares[i].fd = -1;
}

void ft_share_shutdown(void) {
    for (int i = 0; i < FT_SHARE_MAX; ++i) stop_program(&g_shares[i]), g_shares[i].used = false;
}

void ft_share_presented(int index) {
    struct share *s = find(index);
    if (!s) return;
    s->fresh = true;
    pump(s);
}

bool ft_share_wanted(int index) {
    const struct share *s = find(index);
    return s && s->wanted;
}

void ft_share_forget(void *buffer) {
    for (int i = 0; i < FT_SHARE_MAX; ++i) {
        struct share *s = &g_shares[i];
        if (s->out == buffer) s->out = NULL;  // (it can't go while it's kept; if it did, the hold went with it)
        for (int k = 0; k < MOST_TOLD; ++k) {
            if (s->told[k].buffer != buffer) continue;
            say(s, NULL, 0, "forget %u", s->told[k].id);
            s->told[k].buffer = NULL;
        }
    }
}

void ft_share_tick(void) {
    const uint32_t t = now_ms();
    for (int i = 0; i < FT_SHARE_MAX; ++i) {
        struct share *s = &g_shares[i];
        if (s->pid > 0 && s->quit_at && (int32_t)(t - s->quit_at) >= FT_SHARE_QUIT_MS) {
            wlr_log(WLR_ERROR, "share %d: ft-sharepanel (pid %d) didn't end, killing it", s->index + 1, s->pid);
            kill(s->pid, SIGKILL);
            s->quit_at = 0;
        }
        if (s->used && s->out && t - s->out_ms > FT_SHARE_STUCK_MS) {
            wlr_log(WLR_ERROR, "share %d: ft-sharepanel kept a picture for %d ms, ending it", s->index + 1, FT_SHARE_STUCK_MS);
            stop_program(s);  // its exit sets up the restart
        }
        if (s->used && s->restart_at && (int32_t)(t - s->restart_at) >= 0 && s->pid == 0 && s->fd < 0 && !start_program(s))
            schedule_restart(s);
        if (s->used) pump(s);
    }
}

bool ft_share_child(pid_t pid, int status) {
    for (int i = 0; i < FT_SHARE_MAX; ++i) {
        struct share *s = &g_shares[i];
        if (s->pid != pid) continue;
        s->pid = 0;
        s->quit_at = 0;
        if (WIFEXITED(status)) wlr_log(WLR_INFO, "share %d: ft-sharepanel exited (%d)", s->index + 1, WEXITSTATUS(status));
        else wlr_log(WLR_INFO, "share %d: ft-sharepanel killed (signal %d)", s->index + 1, WTERMSIG(status));
        disconnect(s);
        if (s->used) {
            if (strncmp(s->state, "lost", 4) != 0) snprintf(s->state, sizeof s->state, "lost");  // keeps the why
            if (!s->restart_at) schedule_restart(s);
        }
        return true;
    }
    return false;
}

bool ft_share_command(const char *cmd, char *reply, int size) {
    if (strcmp(cmd, "shares") == 0) {
        int n = 0;
        for (int i = 0; i < FT_SHARE_MAX; ++i) n += g_shares[i].used;
        int at = snprintf(reply, size, "ok %d", n);
        for (int i = 0; i < FT_SHARE_MAX && at < size; ++i) {
            const struct share *s = &g_shares[i];
            if (!s->used) continue;
            char state[32];
            sscanf(s->state, "%31s", state);
            at += snprintf(reply + at, size - at, " %d:%d:%s:%dx%d", s->index + 1, s->slot, state, s->width, s->height);
        }
        return true;
    }
    int number;
    char word[16];
    if (sscanf(cmd, "share %d %15s", &number, word) != 2) return false;
    const int index = number - 1;
    struct share *s = find(index);
    if (strcmp(word, "stop") == 0) {
        if (!s) return snprintf(reply, size, "error screen %d isn't shared", number), true;
        stop_program(s);
        s->used = false;
        s->restart_at = 0;
        wlr_log(WLR_INFO, "share %d: stopped", number);
        return snprintf(reply, size, "ok"), true;
    }
    if (strcmp(word, "start") != 0) return snprintf(reply, size, "error share <N> start <slot> <W>x<H> <fps> | share <N> stop"), true;
    int slot, width, height, fps;
    if (sscanf(cmd, "share %*d start %d %dx%d %d", &slot, &width, &height, &fps) != 4)
        return snprintf(reply, size, "error share <N> start <slot> <W>x<H> <fps>"), true;
    if (slot < 0 || slot >= FT_SHARE_MAX || width < 128 || height < 128 || width > 4096 || height > 4096 || ((width | height) & 1) ||
        fps < 1 || fps > 60)
        return snprintf(reply, size, "error slot, size or fps out of range"), true;
    if (!g_buffers->current(g_buffers_data, index)) return snprintf(reply, size, "error screen %d has no picture", number), true;
    if (s && s->slot == slot && s->width == width && s->height == height && s->fps == fps) return snprintf(reply, size, "ok running"), true;
    for (int i = 0; i < FT_SHARE_MAX; ++i)
        if (&g_shares[i] != s && g_shares[i].used && g_shares[i].slot == slot)
            return snprintf(reply, size, "error panel %d is screen %d", slot, g_shares[i].index + 1), true;
    if (!*g_program) return snprintf(reply, size, "error no ft-sharepanel (together/build, or $FT_SHAREPANEL)"), true;
    if (s) {
        stop_program(s);  // new settings: it starts over once the old one has gone
    } else {
        for (int i = 0; i < FT_SHARE_MAX && !s; ++i)
            if (!g_shares[i].used && g_shares[i].pid == 0) s = &g_shares[i];
        if (!s) return snprintf(reply, size, "error an old ft-sharepanel is still ending: ask again"), true;
        *s = (struct share){.used = true, .index = index, .fd = -1};
    }
    s->slot = slot, s->width = width, s->height = height, s->fps = fps;
    char log[PATH_MAX];
    log_path(s, log, sizeof log);
    const int f = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);  // a fresh log
    if (f >= 0) close(f);
    s->restart_at = now_ms() | 1;  // ft_share_tick starts it, once an old one's exit is reaped
    snprintf(s->state, sizeof s->state, "starting");
    return snprintf(reply, size, "ok"), true;
}
