// The OpenVR side of ft-screens (vr.cpp), called from the wlroots compositor (compositor.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ft_dmabuf {
    int width, height;
    uint32_t format;    // DRM_FORMAT_*
    uint64_t modifier;  // DRM_FORMAT_MOD_*
    int n_planes;
    uint32_t offset[4], stride[4];
    int fd[4];
};

// FT_FRONT: a spin (the lazy susan) brought panel `screen` to straight ahead: typing goes there.
enum ft_event_type { FT_MOTION, FT_BUTTON, FT_SCROLL, FT_LEAVE, FT_QUIT, FT_FRONT };

struct ft_event {
    enum ft_event_type type;
    int screen;
    double x, y;      // FT_MOTION: buffer pixels from the top left
    uint32_t button;  // FT_BUTTON: linux BTN_*
    bool pressed;
    bool controller;  // FT_BUTTON: from a hand controller's laser (not the 3D mouse's)
    double dx, dy;    // FT_SCROLL: notches (positive dy: scroll down)
};

bool ft_vr_init(void);
void ft_vr_shutdown(void);
// Modifiers SteamVR can import for a DRM format. Returns the count (at most max).
int ft_vr_modifiers(uint32_t format, uint64_t *out, int max);
// The screens are showing (by the visibility mode; not counting a wrist-pinned screen).
bool ft_vr_screens_shown(void);
// Frametop is paused for a VR game ("pause on"): everything is hidden, and KWin slows down.
bool ft_vr_paused(void);
// Screen (or floating window) `index` shows now. True without SteamVR (--no-vr).
bool ft_vr_screen_visible(int index);
// How much of a screen you see, for its frame rate (compositor.c): hidden (or out of view),
// in view, or focused (you look at it, or a laser or the mouse is on it). Focused without
// SteamVR (--no-vr) or for an unknown screen.
enum ft_attention { FT_HIDDEN, FT_IN_VIEW, FT_FOCUSED };
enum ft_attention ft_vr_screen_attention(int index);
// The display's refresh rate and the time since its last vsync, in seconds. False without
// them (no SteamVR, or the headset isn't reporting).
bool ft_vr_vsync(double *since, double *hz);
// A panel for screen `index`, width in metres, placed in a row in front of the head.
void ft_vr_screen_create(int index, double metres, int count);
void ft_vr_screen_destroy(int index);
// KWin's scale for a screen (its logical pixels: buffer pixels / scale; the lazy susan sizes by them).
void ft_vr_screen_scale(int index, double scale);
// A key from the input relay (evdev code, 1 press, 0 release, 2 repeat): a press counts as use of the
// front slot's screen, and Ctrl held makes a carry move finely.
void ft_vr_key(uint32_t code, int value);
// Ctrl or Alt pressed while the 3D mouse carries a panel (carry_toggle): true for its press, repeats and
// release, which go to the carry, not the desktop.
bool ft_vr_carry_key(uint32_t code, int value);
// A key for the gamescope app clicked last (vr.cpp's "gamescope apps"; value as ft_vr_key's, a repeat
// sent as a press): true if it went there. A key goes up where it went down; a press doesn't go
// while `elsewhere` (frunner's search has the typing) or while that app's panel is hidden.
bool ft_vr_app_key(uint32_t code, int value, bool elsewhere);
// An app has the typing, on a panel that's shown (the relay grabs the keyboards for it, as for the desktop).
bool ft_vr_app_typing(void);
// A click on something that isn't an app (Steam's panels): the typing leaves the apps.
void ft_vr_app_blur(void);
// Where Full Keyboard opens, for the relay's "kbpanel show" (vrkbspot.cpp): "dash" and the bottom
// centre of Steam's page while the dashboard shows it, else "head" and the headset's pose, then
// twelve numbers (a 3x4 pose, rows, the standing universe). False if neither is known.
bool ft_vr_keyboard_spot(char *out, int size);
// The 3D mouse's wheel (notches, + up) during a carry_toggle carry: it pushes the carried panel. False if none.
bool ft_vr_carry_wheel(int notches);
// A remote screen's panel (remote.c): another machine's display, streamed by ft-stream. It
// has a screen's controls and takes the same commands; its frames come through
// ft_vr_screen_present, and its input arrives as events for its index. *restored: it's back
// where it was when its stream last stopped in this run (Remote Displays' Disconnect).
bool ft_vr_remote_create(int index, const char *label, double metres, bool *restored);
// What the remote screen shows (its host and app, each time they're set): its dashboard tile's
// memory key. Another display on the same panel is another key.
void ft_vr_remote_named(int index, const char *host, const char *app);
// A PC app window's scale, as ft-layout says it (remote.c's "remote N scale"); 0: its PC takes none. With
// the size its stream has or will have for it (0 x 0: not said).
void ft_vr_remote_scale(int index, double scale, int width, int height);
// The wheel over a remote screen (notches, as FT_SCROLL's dy): true if Meta is held and it's a PC app
// window's whose PC takes a scale (vrappscale.cpp): it steps that, and the PC doesn't hear the wheel.
bool ft_vr_app_scale_wheel(int index, double dy);
// A remote screen's host ("192.0.2.61" or with ":PORT") and app ("display:DP-2", "window:0x...",
// "Desktop"), as ft-layout started it (remote.c); NULL for a screen that isn't a remote one.
const char *ft_remote_host(int index);
const char *ft_remote_app(int index);
// The headset went off for good, or back on (remoteaway.h): while away, remote.c starts no
// stream but a Together room's (ft-layout starts the others again when it's back on).
void ft_remote_away(bool away);
// Gamepad mode's pad for remote screen `index` (remote.c): "pad B LT RT LX LY RX RY" or "pad off".
void ft_remote_pad(int index, const char *msg);
// A second ft-screens next to the running desktop (--beside): nothing goes to ft-floatd or
// ft-layout.
void ft_vr_beside(void);
// A spare output's panel, for floating windows (slot numbers from 1): hidden until
// ft-floatd floats a window on it and KWin has the output turned on.
void ft_vr_float_create(int index, int slot);
void ft_vr_float_output(int index, bool on);
// Show a client buffer (identified by `key`) on the screen's panel. False if SteamVR
// can't import it.
bool ft_vr_screen_present(int index, const void *key, const struct ft_dmabuf *buf);
// The panel keeps its picture for now ("hold", while ft-floatd changes its window): new buffers wait.
bool ft_vr_screen_held(int index);
// A buffer is going away: drop its import.
void ft_vr_forget(const void *key);
// Poll panel input and SteamVR events.
void ft_vr_poll(void (*handle)(const struct ft_event *, void *), void *data);
// Gamepad mode's pill (vr.cpp): over the panel typing goes to, `screen` (-1: Steam, no pill), from
// compositor.c's keys_update; and the panel SteamVR's Gamepad mode is for while it's ours (-1: none),
// for the input relay's "pad" (a remote display's).
void ft_vr_gamepad_target(int screen);
int ft_vr_gamepad_screen(void);
// The panel the 3D mouse's laser is on, when its wheel can scroll there directly (the input
// relay's "wheel", not SteamVR's joystick scrolling): it shows, nothing's paused, and nothing
// drags it. -1 when not.
int ft_vr_mouse_screen(void);
// The panel the 3D mouse's laser is on, held press or not (keys_update: Meta to a PC); -1 none.
int ft_vr_mouse_panel(void);
// The relay's wheel in the ~100 ms before it hears ft_vr_mouse_screen() went -1 for a held press, a
// torn-off window or a KWin move (keys_update): what SteamVR's scroll would do there (push or pull a
// carried window, tear one off, or scroll under the held button). False if that isn't why it's -1.
bool ft_vr_held_wheel(int notches, void (*handle)(const struct ft_event *, void *), void *data);
// Dictation's notice (vrvoice.cpp): "listen hold|toggle|command LEVEL", "typing" or "off", from ft-voiced.
void ft_vr_voice(const char *words);
// A command from the control socket (@ft_screens); writes the reply (see vr.cpp).
void ft_vr_command(const char *command, char *reply, int reply_size);

#ifdef __cplusplus
}
#endif
