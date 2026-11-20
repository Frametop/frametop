// A helper program of ft-screens' with a socket to it: ft-stream, ft-appless and ft-peerpanel,
// which bring a panel its pictures (remote.c), and ft-sharepanel, which takes a screen's (share.c).
#pragma once

#include <stdbool.h>
#include <sys/types.h>

// Runs argv[0] with one end of a new SOCK_SEQPACKET socket pair as its fd 3, /dev/null as its
// input, and its output added to the file `log`. It gets none of ft-screens' blocked signals
// (the event loop takes SIGTERM, SIGINT and SIGCHLD through a signalfd): with SIGTERM blocked a
// child heard neither kill nor its death signal. Returns our end, which doesn't block, and the
// child's pid; or -1 with errno set.
int ft_child_spawn(char *const argv[], const char *log, pid_t *pid);

// <repo>/<folder>/build/<name> for an ft-screens at <repo>/screens/build/ft-screens, or what the
// environment variable `env` names. False when neither can be said (ft-screens was rebuilt and
// its own file is gone: so it is asked once, at start).
bool ft_child_path(const char *env, const char *folder, const char *name, char *out, size_t size);
