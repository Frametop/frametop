// ft-clip: the Frametop desktop's clipboard (its text or PNG image) for ft-stream's shared clipboard
// (docs/shared-clipboard.md). A Wayland client of the nested KWin through
// wlr-data-control (protocols/wlr-data-control-unstable-v1.xml), which KWin gives clients that
// aren't focused, as a clipboard manager needs.
//
//   ft-clip get   the clipboard's text on stdout. Exit 1 when it holds no text (empty, an image
//                 only, nothing copied), more than FT_CLIP_MAX bytes, or a password (the app that copied
//                 it says so with x-kde-passwordManagerHint, as KeePassXC does for clipboard
//                 managers), 2 when there's no desktop.
//   ft-clip set   the text on stdin (at most FT_CLIP_MAX bytes, not empty) becomes the clipboard. It
//                 exits once the desktop has taken it, leaving a child that hands the text to
//                 the apps that paste it until something else is copied, or the desktop ends.
//                 It reads all of stdin before it connects, so a writer never waits on it.
//   ft-clip get-image, ft-clip set-image   the same for a PNG image (image/png, at most
//                 FT_CLIP_MAX_IMAGE bytes): get-image exits 1 when the clipboard holds no PNG, and
//                 set-image takes only a PNG.
//
// WAYLAND_DISPLAY names the desktop (ft-stream gives $XDG_RUNTIME_DIR/frametop/wayland-0).
// Nothing is logged of the content itself.
#define _GNU_SOURCE
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#ifndef FT_CLIP_MAX
#define FT_CLIP_MAX (1 << 20)  // ft-stream's kMaxText (stream/clipboard.h)
#endif
#ifndef FT_CLIP_MAX_IMAGE
#define FT_CLIP_MAX_IMAGE (32 << 20)  // ft-stream's FT_CLIP_MAX_IMAGE (stream/clipboard.h)
#endif
static size_t g_max = FT_CLIP_MAX;  // FT_CLIP_MAX_IMAGE for an image
// For the app that owns the clipboard to hand it over, or an app that pastes to take it.
static int g_timeout_ms = 2000;  // 10 s for an image
static bool g_image;  // get-image or set-image

// The text types offered and taken, best first.
static const char *const kTypes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
enum { kNumTypes = sizeof kTypes / sizeof kTypes[0] };
static const char kPng[] = "image/png";
static bool is_png(const char *data, size_t len) {
    return len > 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
}
static const char kSecretHint[] = "x-kde-passwordManagerHint";  // offered with a password

static struct wl_seat *g_seat;
static struct zwlr_data_control_manager_v1 *g_manager;

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data, (void)version;
    if (!g_seat && strcmp(iface, wl_seat_interface.name) == 0)
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!g_manager && strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0)
        g_manager = wl_registry_bind(reg, name, &zwlr_data_control_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data, (void)reg, (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

// ------------------------------------------------------------------------------------ get

struct offer {
    struct zwlr_data_control_offer_v1 *offer;
    int best;  // index into kTypes, kNumTypes: no text type
    bool png;  // offers kPng
    bool secret;  // offers kSecretHint
};
static struct offer *g_selection;  // the clipboard's offer, NULL while it's empty
static bool g_selection_seen;

static void offer_type(void *data, struct zwlr_data_control_offer_v1 *o, const char *type) {
    (void)o;
    struct offer *of = data;
    if (strcmp(type, kSecretHint) == 0) of->secret = true;
    if (strcmp(type, kPng) == 0) of->png = true;
    for (int i = 0; i < of->best; ++i)
        if (strcmp(type, kTypes[i]) == 0) of->best = i;
}
static const struct zwlr_data_control_offer_v1_listener offer_listener = {offer_type};

static void forget(struct offer *of) {
    if (!of) return;
    zwlr_data_control_offer_v1_destroy(of->offer);
    free(of);
}

static void device_offer(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *o) {
    (void)data, (void)d;
    struct offer *of = calloc(1, sizeof *of);
    if (!of) {
        zwlr_data_control_offer_v1_destroy(o);
        return;
    }
    of->offer = o, of->best = kNumTypes;
    zwlr_data_control_offer_v1_add_listener(o, &offer_listener, of);
}
static void device_selection(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *o) {
    (void)data, (void)d;
    if (g_selection && (!o || g_selection->offer != o)) forget(g_selection);
    g_selection = o ? zwlr_data_control_offer_v1_get_user_data(o) : NULL;
    g_selection_seen = true;
}
static void device_finished(void *data, struct zwlr_data_control_device_v1 *d) { (void)data, (void)d; }
static void device_primary(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *o) {
    (void)data, (void)d;
    if (o) forget(zwlr_data_control_offer_v1_get_user_data(o));  // the primary selection isn't ours to share
}
static const struct zwlr_data_control_device_v1_listener device_listener = {device_offer, device_selection,
                                                                            device_finished, device_primary};

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int get(struct wl_display *dpy) {
    struct zwlr_data_control_device_v1 *dev = zwlr_data_control_manager_v1_get_data_device(g_manager, g_seat);
    zwlr_data_control_device_v1_add_listener(dev, &device_listener, NULL);
    // The device's first events are the clipboard as it stands: its offer, the offer's types,
    // then the selection.
    for (int i = 0; i < 2 && !g_selection_seen; ++i)
        if (wl_display_roundtrip(dpy) < 0) return 2;
    if (!g_selection || (g_image ? !g_selection->png : g_selection->best == kNumTypes)) return 1;
    if (g_selection->secret) return fprintf(stderr, "ft-clip: the clipboard holds a password, which isn't shared\n"), 1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return 2;
    zwlr_data_control_offer_v1_receive(g_selection->offer, g_image ? kPng : kTypes[g_selection->best], fds[1]);
    wl_display_flush(dpy);
    close(fds[1]);
    // The app that owns the clipboard writes it, then closes its end.
    char *text = malloc(g_max + 1);
    if (!text) return 2;
    size_t n = 0;
    const int64_t until = now_ms() + g_timeout_ms;
    for (;;) {
        const int64_t left = until - now_ms();
        struct pollfd p = {fds[0], POLLIN, 0};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) {
            fprintf(stderr, "ft-clip: the app that owns the clipboard didn't hand it over in %d ms\n", g_timeout_ms);
            return 1;
        }
        const ssize_t r = read(fds[0], text + n, g_max + 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return 2;
        if (r == 0) break;
        n += (size_t)r;
        if (n > g_max) return fprintf(stderr, "ft-clip: the clipboard holds more than %zu bytes\n", g_max), 1;
    }
    if (n == 0) return 1;
    if (g_image && !is_png(text, n)) return fprintf(stderr, "ft-clip: the clipboard's image/png isn't a PNG\n"), 1;
    for (size_t at = 0; at < n;) {
        const ssize_t w = write(STDOUT_FILENO, text + at, n - at);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 2;
        at += (size_t)w;
    }
    return 0;
}

// ------------------------------------------------------------------------------------ set

static char *g_text;
static size_t g_len;
static bool g_cancelled;

static void source_send(void *data, struct zwlr_data_control_source_v1 *s, const char *type, int32_t fd) {
    (void)data, (void)s, (void)type;
    // A pasting app that stops reading mustn't hold the clipboard up for the others: the
    // writes never block, and the app gets g_timeout_ms for all of it.
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int64_t until = now_ms() + g_timeout_ms;
    for (size_t at = 0; at < g_len;) {
        const int64_t left = until - now_ms();
        struct pollfd p = {fd, POLLOUT, 0};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        const ssize_t w = write(fd, g_text + at, g_len - at);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (w <= 0) break;
        at += (size_t)w;
    }
    close(fd);
}
static void source_cancelled(void *data, struct zwlr_data_control_source_v1 *s) {
    (void)data, (void)s;
    g_cancelled = true;  // something else was copied
}
static const struct zwlr_data_control_source_v1_listener source_listener = {source_send, source_cancelled};

// set's text, from stdin: all of it, before the desktop is asked anything.
static int read_input(void) {
    g_text = malloc(g_max + 1);
    if (!g_text) return 2;
    for (;;) {
        const ssize_t r = read(STDIN_FILENO, g_text + g_len, g_max + 1 - g_len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return 2;
        if (r == 0) break;
        g_len += (size_t)r;
        if (g_len > g_max) return fprintf(stderr, "ft-clip: more than %zu bytes to copy\n", g_max), 1;
    }
    if (g_len == 0) return fprintf(stderr, "ft-clip: nothing to copy\n"), 1;
    if (g_image && !is_png(g_text, g_len)) return fprintf(stderr, "ft-clip: that isn't a PNG image\n"), 1;
    return 0;
}

static int set(struct wl_display *dpy) {
    struct zwlr_data_control_source_v1 *src = zwlr_data_control_manager_v1_create_data_source(g_manager);
    zwlr_data_control_source_v1_add_listener(src, &source_listener, NULL);
    if (g_image)
        zwlr_data_control_source_v1_offer(src, kPng);
    else
        for (int i = 0; i < kNumTypes; ++i) zwlr_data_control_source_v1_offer(src, kTypes[i]);
    struct zwlr_data_control_device_v1 *dev = zwlr_data_control_manager_v1_get_data_device(g_manager, g_seat);
    zwlr_data_control_device_v1_set_selection(dev, src);
    if (wl_display_roundtrip(dpy) < 0) return 2;
    if (g_cancelled) return 0;  // replaced at once
    // The text stays ours to hand out: a child keeps the connection, and the caller goes on.
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid > 0) _exit(0);
    setsid();
    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0) dup2(null, STDIN_FILENO), dup2(null, STDOUT_FILENO), dup2(null, STDERR_FILENO);
    while (!g_cancelled && wl_display_dispatch(dpy) >= 0) {}
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc == 2 ? argv[1] : "";
    const bool want_get = strcmp(mode, "get") == 0 || strcmp(mode, "get-image") == 0;
    const bool want_set = strcmp(mode, "set") == 0 || strcmp(mode, "set-image") == 0;
    if (!want_get && !want_set) return fprintf(stderr, "usage: ft-clip get|set|get-image|set-image\n"), 2;
    g_image = strstr(mode, "-image") != NULL;
    if (g_image) g_max = FT_CLIP_MAX_IMAGE, g_timeout_ms = 10000;
    signal(SIGPIPE, SIG_IGN);
    // What ft-stream left open (its sockets) mustn't stay open in the child that serves a copy.
    close_range(3, ~0U, 0);
    if (want_set) {
        const int r = read_input();
        if (r != 0) return r;
    }
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return fprintf(stderr, "ft-clip: no desktop at %s\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?"), 2;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    if (wl_display_roundtrip(dpy) < 0) return 2;
    if (!g_seat || !g_manager) return fprintf(stderr, "ft-clip: the desktop has no %s\n", g_seat ? "data control" : "seat"), 2;
    const int r = want_get ? get(dpy) : set(dpy);
    wl_display_disconnect(dpy);
    return r;
}
