// Offline replay of the RemoteStreams model's path against the real screens/remote.c, with the
// SteamVR side stubbed and a fake ft-stream (FT_STREAM) that takes 1 s to go after SIGTERM.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "remote.h"

int ft_vr_modifiers(uint32_t f, uint64_t *o, int m) { (void)f, (void)o, (void)m; return 0; }
bool ft_vr_screen_present(int i, const void *k, const struct ft_dmabuf *b) { (void)i, (void)k, (void)b; return true; }
void ft_vr_forget(const void *k) { (void)k; }
enum ft_attention ft_vr_screen_attention(int i) { (void)i; return FT_HIDDEN; }
void ft_vr_screen_destroy(int i) { (void)i; }
bool ft_vr_remote_create(int i, const char *l, double m, bool *r) { (void)i, (void)l, (void)m; *r = false; return true; }

static struct wl_event_loop *loop;
static void run(int ms) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        wl_event_loop_dispatch(loop, 10);
        ft_remote_tick();
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) ft_remote_child(pid, status);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}
static void cmd(const char *c) {
    char reply[512] = "";
    ft_remote_command(c, reply, sizeof reply);
    FILE *f = fopen("@WORK@/rs-events.log", "a");
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    fprintf(f, "%ld.%09ld ft-layout: %s -> %s\n", (long)t.tv_sec, t.tv_nsec, c, reply);
    fclose(f);
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "stop-start";
    loop = wl_event_loop_create();
    ft_remote_init(loop, true);
    const char *A = "remote 101 start frame 192.0.2.7 display:X 1920x1080 60 0 1.0 test";
    const char *B = "remote 101 start frame 192.0.2.7 display:X 1920x1080 30 0 1.0 test";
    cmd(A);
    run(600);
    if (!strcmp(mode, "stop-start")) cmd("remote 101 stop"), cmd(A);  // ft-layout remote host NAME route=...
    else cmd(B);                                                        // new settings, one command
    run(2500);
    cmd("remotes");
    ft_remote_shutdown();
    run(1500);
    return 0;
}
