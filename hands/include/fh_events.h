/*
 * fh_events - what Frametop did with the tracked hands, one JSON line per decision, for a
 * testing session: the Hand Recorder copies them beside the recording (hands/rec/eventlog.py),
 * so a mark or a spoken note can be lined up with what the panels and the pointer did just
 * before it, and a session can say how many pokes, pinches and strokes worked.
 *
 * /run/user/UID/frametop-hands/events.jsonl, appended only while events-want exists next to
 * it (the recorder makes it while a testing session records and removes it after), so normal
 * use writes nothing. A writer looks for events-want at most once a second, when it has
 * something to write, and opens the file again if it was removed.
 *
 * A line:
 *   {"t": CLOCK_MONOTONIC ns, "src": "screens" | "pointer", "kind": "...", "outcome": "...",
 *    "hand": "left" | "right" | "", "panel": the screen's number as the logs give it (0: none),
 *    "text": the log line}
 * kind: what was tried. ft-screens: poke, pinch (a click by pinching), grab (a panel taken by
 *   a pinch or fist), throw, push, flick, card, scroll, tap, lazy-susan, stretch, grab-that,
 *   palm-menu, far-stretch, hold (whatever a hand held, when it was lost). The pointer
 *   helper: ray (the hand's laser), click, drag, right-click, scroll, handoff.
 * outcome: done (it happened), refused (a rule held it back; text says which), missed
 *   (nothing there to act on), end (a hold ended), lost (it ended because the hand was lost),
 *   passed (handed to the other program: the pointer leaves a pinch at a panel to ft-screens),
 *   info (a note: a hold going on, a stroke seen before the panels turned).
 * Each line is one write() to an O_APPEND file, so the two writers don't interleave.
 */
#ifndef FH_EVENTS_H
#define FH_EVENTS_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FH_EVENTS_FILE "events.jsonl"
#define FH_EVENTS_WANT "events-want"

static inline long long fh_events_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* The open file while a session wants events, else -1 (looked at again after a second). */
static inline int fh_events_fd(void) {
    static int fd = -1;
    static long long checked = 0;
    static ino_t ino = 0;
    long long now = fh_events_now_ns();
    if (now - checked < 1000000000LL && checked) return fd;
    checked = now;
    char dir[64], path[96];
    snprintf(dir, sizeof dir, "/run/user/%u/frametop-hands", (unsigned)getuid());
    snprintf(path, sizeof path, "%s/" FH_EVENTS_WANT, dir);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (fd >= 0) close(fd);
        fd = -1;
        return fd;
    }
    snprintf(path, sizeof path, "%s/" FH_EVENTS_FILE, dir);
    if (fd >= 0 && (stat(path, &st) != 0 || st.st_ino != ino)) {   /* removed since: a new one */
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0 && fstat(fd, &st) == 0) ino = st.st_ino;
    }
    return fd;
}

static inline size_t fh_events_quote(char *out, size_t room, const char *s) {
    size_t n = 0;
    for (; *s && n + 7 < room; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, room - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* One decision. side: 0 left, 1 right, -1 neither; panel: the logs' screen number, 0 none. */
static inline void fh_event_text(const char *src, const char *kind, const char *outcome, int side, int panel,
                                 const char *text) {
    int fd = fh_events_fd();
    if (fd < 0) return;
    char q[1024], line[1400];
    fh_events_quote(q, sizeof q, text ? text : "");
    int n = snprintf(line, sizeof line,
                     "{\"t\": %lld, \"src\": \"%s\", \"kind\": \"%s\", \"outcome\": \"%s\", \"hand\": \"%s\", "
                     "\"panel\": %d, \"text\": \"%s\"}\n",
                     fh_events_now_ns(), src, kind, outcome, side == 0 ? "left" : side == 1 ? "right" : "", panel, q);
    if (n > 0 && n < (int)sizeof line) {
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
    }
}

static inline void fh_event(const char *src, const char *kind, const char *outcome, int side, int panel,
                            const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static inline void fh_event(const char *src, const char *kind, const char *outcome, int side, int panel,
                            const char *fmt, ...) {
    if (fh_events_fd() < 0) return;
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    fh_event_text(src, kind, outcome, side, panel, text);
}

#endif
