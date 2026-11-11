// Shared screens (share.c): a screen's pictures, as KWin draws them, also go to an ft-sharepanel
// (together/ft-sharepanel.cpp), which encodes them for the people of a Together room
// (docs/together.md).
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include <wayland-server-core.h>

#include "vr.h"

// A Frame shares at most this many panels (its encoder sessions, docs/together.md).
#define FT_SHARE_MAX 3

// The compositor's buffers, as the tap needs them: it knows nothing else of them.
struct ft_share_buffers {
    // The buffer on screen `index`'s panel now; NULL when it has none, or isn't a screen whose
    // whole buffer is its picture (a floating window's is a part of its buffer). `data` is
    // ft_share_init's.
    void *(*current)(void *data, int index);
    bool (*describe)(void *buffer, struct ft_dmabuf *out);
    // One more hold on a buffer, so that its client leaves it as it is; and that hold let go.
    void (*keep)(void *buffer);
    void (*release)(void *buffer);
};

void ft_share_init(struct wl_event_loop *loop, const struct ft_share_buffers *buffers, void *data);
void ft_share_shutdown(void);
// Screen `index` (from 0) shows a new picture.
void ft_share_presented(int index);
// Whether screen `index` is shared and someone in the room watches it: it is drawn for them,
// whether or not its owner looks at it.
bool ft_share_wanted(int index);
// A buffer is going away.
void ft_share_forget(void *buffer);
// Every tick: pictures that waited for their turn go, and programs that ended start again.
void ft_share_tick(void);
// A child process exited: true if it was one of ours.
bool ft_share_child(pid_t pid, int status);
// "share ..." and "shares" commands; false for anything else.
bool ft_share_command(const char *cmd, char *reply, int size);
