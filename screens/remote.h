// Remote screens (remote.c): displays of other machines, streamed by ft-stream
// (stream/ft-stream.cpp) and shown as panels of their own, with every screen's controls.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-server-core.h>

#include "vr.h"

// Remote screens are numbered from here (from 1, like the others), clear of KWin's screens
// and spare outputs, so their numbers don't move when the desktop's screens change.
#define FT_REMOTE_FIRST 101
#define FT_REMOTE_MAX 16

// vr: connected to SteamVR (without it, no stream starts: there'd be no panel).
void ft_remote_init(struct wl_event_loop *loop, bool vr);
void ft_remote_shutdown(void);
// Whether screen `index` (from 0) is a remote screen that's running.
bool ft_remote_is(int index);
// Pointer input on a remote screen's panel.
void ft_remote_event(const struct ft_event *e);
// The pointer is on a screen that isn't a remote one: a room's panel it was on is told it left.
void ft_remote_pointer_away(void);
// A key (linux KEY_* code) for remote screen `index`.
void ft_remote_key(int index, uint32_t code, bool pressed);
// Typing went to remote screen `index` (a click or a spin to it): the shared clipboard sends the
// desktop's text to its host (stream/clipboard.h). Said on every click; ft-stream acts once.
void ft_remote_focus(int index);
// Typing went elsewhere: the keys it holds on the host come up (and gamepad mode's pad goes),
// and the shared clipboard brings the host's text back.
void ft_remote_blur(int index);
// Gamepad mode's pad state for remote screen `index`: "pad B LT RT LX LY RX RY" or "pad off"
// (from the input relay, input/vrpad.py).
void ft_remote_pad(int index, const char *msg);
// Input from someone else of a Together room (peerinput.h), on a window of the wearer's PC that
// they were given. It goes through the display's own stream, apart from the wearer's: nothing
// here moves where the wearer's pointer is said to be, or lets go of what the wearer pressed.
//
// The size in pixels of remote screen `index`, when it is another machine's display whose
// stream runs. False for anything else, a room's panel too.
bool ft_remote_peer_size(int index, int *width, int *height);
// Their motion, button (where it says, as the host's pointer may have been elsewhere since) or scroll.
void ft_remote_peer_event(const struct ft_event *e);
// Their key. It doesn't say that typing came to the display, as the wearer's key does: the
// shared clipboard sends the desktop's text to a host only for the wearer.
void ft_remote_peer_key(int index, uint32_t code, bool pressed);
// Whether the wearer holds a mouse button that went down on a remote screen.
bool ft_remote_held(void);
// Every tick: each stream hears how much of its screen you see, and lost ones restart.
void ft_remote_tick(void);
// A child process exited: true if it was one of ours.
bool ft_remote_child(pid_t pid, int status);
// "remote ..." and "remotes" commands; false for anything else.
bool ft_remote_command(const char *cmd, char *reply, int size);
