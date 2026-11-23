// Finding ft-npud and telling it from an impostor. ft-hands runs in the release's rootless container, where the
// host's /run is at /run/host/run and the host's root has no uid: its files and its socket's peer credentials show
// the kernel's overflow uid (usually 65534), as do every other unmapped host uid (the system users below 1000).
// A peer with the overflow uid is taken for ft-npud only when no one but root, this user or an unmapped uid could
// have put a socket at that path: every directory from the socket's up to / is one of theirs and not writable by
// its group or others. On SteamOS that chain is the host's / and /run (root's, 755) and the unit's runtime
// directory, so a system user can't plant a socket there either.
#pragma once

#include <string>
#include <sys/types.h>

// DIR as given when DIR/ft-npud.heads exists, else /run/host + DIR when that one does (inside the container), else
// DIR (and connecting fails with DIR's error). The heads file, not the socket: a socket link alone isn't a daemon.
std::string npu_server_dir(const std::string &dir);

// The kernel's overflow uid (/proc/sys/kernel/overflowuid), 65534 when it can't be read.
uid_t npu_overflow_uid();

// Whether a server with uid PEER at a socket in (resolved, symlink-free) directory DIR is ft-npud for a client
// running as SELF: PEER is root or SELF, or PEER is OVERFLOW and DIR's chain is safe (above). WHY says why not.
bool npu_server_trusted(uid_t peer, uid_t self, uid_t overflow, const std::string &dir, std::string &why);
