// ft-clip (stream/ft-clip.c) against a compositor of its own: a wlroots seat with
// wlr-data-control, as KWin gives it, in this process. Runs build/tests/ft-clip (built with
// FT_CLIP_MAX=65536) for each case while dispatching the compositor's clients.
//   ft-clip-test PATH-TO-FT-CLIP
#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>

static struct wl_display *g_display;
static struct wl_event_loop *g_loop;
static struct wlr_seat *g_seat;
static const char *g_clip;
static int g_failed;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            ++g_failed;                                        \
        }                                                      \
    } while (0)

static void request_set_selection(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_request_set_selection_event *e = data;
    wlr_seat_set_selection(g_seat, e->source, e->serial);
}
static struct wl_listener g_set_selection = {.notify = request_set_selection};

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pump(int ms) {
    const int64_t until = now_ms() + ms;
    while (now_ms() < until) {
        wl_display_flush_clients(g_display);
        wl_event_loop_dispatch(g_loop, 5);
    }
}

static int clients(void) {
    int n = 0;
    struct wl_client *c;
    wl_client_for_each(c, wl_display_get_client_list(g_display)) ++n;
    return n;
}

// Runs `ft-clip WHAT` with `in` (len bytes) on its stdin, keeping the compositor going; its
// stdout in *out (malloc'd, *out_len bytes). The exit status, or -1.
static int run(const char *what, const char *in, size_t len, char **out, size_t *out_len) {
    int to[2], from[2];
    if (pipe2(to, O_CLOEXEC) || pipe2(from, O_CLOEXEC)) return -1;
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(to[0], STDIN_FILENO), dup2(from[1], STDOUT_FILENO);
        execl(g_clip, g_clip, what, (char *)NULL);
        _exit(127);
    }
    close(to[0]), close(from[1]);
    fcntl(to[1], F_SETFL, O_NONBLOCK);
    fcntl(from[0], F_SETFL, O_NONBLOCK);
    size_t sent = 0, got = 0, cap = 1 << 16;
    char *buf = malloc(cap);
    int status = -1;
    bool reading = true;
    const int64_t until = now_ms() + 10000;
    while (now_ms() < until) {
        if (to[1] >= 0) {
            const ssize_t w = sent < len ? write(to[1], in + sent, len - sent) : 0;
            if (w > 0) sent += (size_t)w;
            if (sent >= len || (w < 0 && errno != EAGAIN)) close(to[1]), to[1] = -1;
        }
        while (reading) {
            if (got == cap) buf = realloc(buf, cap *= 2);
            const ssize_t r = read(from[0], buf + got, cap - got);
            if (r > 0) got += (size_t)r;
            else {
                if (r == 0) reading = false;  // EOF: every holder of its write end has gone
                break;
            }
        }
        wl_display_flush_clients(g_display);
        wl_event_loop_dispatch(g_loop, 2);
        int st;
        if (status < 0 && waitpid(pid, &st, WNOHANG) == pid) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        // A `set` leaves its background child holding the pipe (until it moves to /dev/null).
        if (status >= 0 && !reading) break;
        if (status >= 0 && strcmp(what, "set") == 0) break;
    }
    if (to[1] >= 0) close(to[1]);
    close(from[0]);
    if (status < 0) kill(pid, SIGKILL), waitpid(pid, NULL, 0);
    if (out) *out = buf, *out_len = got;
    else free(buf);
    return status;
}

static void expect_get(const char *want, size_t want_len, int want_status, const char *what) {
    char *out = NULL;
    size_t n = 0;
    const int st = run("get", "", 0, &out, &n);
    CHECK(st == want_status, "%s: get exited %d, not %d", what, st, want_status);
    if (want) CHECK(n == want_len && memcmp(out, want, n) == 0, "%s: got %zu bytes, not the %zu copied", what, n, want_len);
    free(out);
}

// A selection the compositor sets itself with no text type: an image someone copied.
static void image_send(struct wlr_data_source *s, const char *type, int32_t fd) { (void)s, (void)type, close(fd); }
static void image_destroy(struct wlr_data_source *s) { (void)s; }
static const struct wlr_data_source_impl image_impl = {.send = image_send, .destroy = image_destroy};

int main(int argc, char **argv) {
    if (argc != 2) return fprintf(stderr, "usage: ft-clip-test FT-CLIP\n"), 2;
    g_clip = argv[1];
    signal(SIGPIPE, SIG_IGN);
    char dir[] = "/tmp/ft-clip-test-XXXXXX";
    if (!mkdtemp(dir)) return perror("mkdtemp"), 2;
    setenv("XDG_RUNTIME_DIR", dir, 1);
    g_display = wl_display_create();
    g_loop = wl_display_get_event_loop(g_display);
    const char *socket = wl_display_add_socket_auto(g_display);
    if (!socket) return fprintf(stderr, "no socket\n"), 2;
    setenv("WAYLAND_DISPLAY", socket, 1);
    g_seat = wlr_seat_create(g_display, "seat0");
    wlr_data_control_manager_v1_create(g_display);
    wl_signal_add(&g_seat->events.request_set_selection, &g_set_selection);

    expect_get(NULL, 0, 1, "nothing copied yet");

    const char hello[] = "hello\nw\xc3\xb6rld\r\n";
    CHECK(run("set", hello, sizeof hello - 1, NULL, NULL) == 0, "set hello");
    pump(50);
    expect_get(hello, sizeof hello - 1, 0, "after set");
    expect_get(hello, sizeof hello - 1, 0, "again: the background child still serves it");
    CHECK(clients() == 1, "one child serving the clipboard, %d clients", clients());

    const char second[] = "second";
    CHECK(run("set", second, sizeof second - 1, NULL, NULL) == 0, "set second");
    for (int i = 0; i < 100 && clients() > 1; ++i) pump(20);
    CHECK(clients() == 1, "the first child ended when its text was replaced, %d clients", clients());
    expect_get(second, sizeof second - 1, 0, "after the second set");

    CHECK(run("set", "", 0, NULL, NULL) == 1, "nothing to copy");
    expect_get(second, sizeof second - 1, 0, "an empty set changes nothing");

    // Up to FT_CLIP_MAX (65536 in the test build), and not more.
    const size_t big = 60000;
    char *text = malloc(70000);
    for (size_t i = 0; i < 70000; ++i) text[i] = (char)('a' + i % 26);
    CHECK(run("set", text, big, NULL, NULL) == 0, "set 60000 bytes");
    pump(50);
    expect_get(text, big, 0, "60000 bytes");
    CHECK(run("set", text, 70000, NULL, NULL) == 1, "70000 bytes is too much to copy");
    expect_get(text, big, 0, "a refused set changes nothing");

    struct wlr_data_source image;
    wlr_data_source_init(&image, &image_impl);
    char **type = wl_array_add(&image.mime_types, sizeof *type);
    *type = strdup("image/png");
    wlr_seat_set_selection(g_seat, &image, wl_display_next_serial(g_display));
    for (int i = 0; i < 100 && clients() > 0; ++i) pump(20);
    CHECK(clients() == 0, "the text's child ended when an image was copied, %d clients", clients());
    expect_get(NULL, 0, 1, "an image only");
    wlr_seat_set_selection(g_seat, NULL, wl_display_next_serial(g_display));
    expect_get(NULL, 0, 1, "cleared");

    setenv("WAYLAND_DISPLAY", "ft-clip-test-no-such-display", 1);
    expect_get(NULL, 0, 2, "no desktop");

    free(text);
    wl_display_destroy_clients(g_display);
    wl_list_remove(&g_set_selection.link);
    wl_display_destroy(g_display);
    char path[256];
    snprintf(path, sizeof path, "%s/%s.lock", dir, socket);
    unlink(path);
    rmdir(dir);
    if (g_failed) return fprintf(stderr, "%d failed\n", g_failed), 1;
    printf("ft-clip: all cases passed\n");
    return 0;
}
