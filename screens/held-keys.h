#ifndef FT_HELD_KEYS_H
#define FT_HELD_KEYS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/input-event-codes.h>
// Key releases KWin hasn't had (DEE-222). wlroots sends a key only to the surface with keyboard
// focus, and drops it when none has it: after the focused screen closes, until a click or a
// spin gives another one focus. A modifier held across that (Meta) came up in ft-screens' state
// but stayed down in KWin, so every click was Meta+click, a window move. ft-screens keeps
// those releases here (ft_lost_keys_add) and sends them with the next keyboard focus
// (ft_lost_keys_take). A release of a key KWin doesn't hold changes nothing there, so one sent
// twice is harmless; a lost one isn't.
#define FT_LOST_KEYS_MAX 40  // every key wlroots can hold (32) and every modifier

struct ft_lost_keys {
    uint32_t code[FT_LOST_KEYS_MAX];
    unsigned n;
};

// A release that had no focus to go to. When full (it can't be: see FT_LOST_KEYS_MAX), the
// oldest goes.
static inline void ft_lost_keys_add(struct ft_lost_keys *l, uint32_t code) {
    for (unsigned i = 0; i < l->n; ++i)
        if (l->code[i] == code) return;
    if (l->n == FT_LOST_KEYS_MAX) {
        for (unsigned i = 1; i < l->n; ++i) l->code[i - 1] = l->code[i];
        --l->n;
    }
    l->code[l->n++] = code;
}

// The next release owed, no longer owed (0: none), oldest first.
static inline uint32_t ft_lost_keys_take(struct ft_lost_keys *l) {
    if (!l->n) return 0;
    const uint32_t code = l->code[0];
    for (unsigned i = 1; i < l->n; ++i) l->code[i - 1] = l->code[i];
    --l->n;
    return code;
}

// The modifiers a "release all keys" (the relay's release_keys) lets go of in KWin even when
// ft-screens doesn't hold them: KWin may still hold one ft-screens already let go.
static const uint32_t ft_modifier_keys[] = {KEY_LEFTCTRL,  KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                                            KEY_LEFTALT,   KEY_RIGHTALT,  KEY_LEFTMETA,  KEY_RIGHTMETA};

// The keys "keys" names (keys_state): the modifiers, Esc and Tab, as scripts/keys-report.py
// does. The others it only counts: any program on the machine can ask, and a list of the letters
// held, asked often enough, would spell out what's typed.
static inline bool ft_key_reportable(uint32_t code) {
    for (size_t i = 0; i < sizeof ft_modifier_keys / sizeof *ft_modifier_keys; ++i)
        if (ft_modifier_keys[i] == code) return true;
    return code == KEY_ESC || code == KEY_TAB;
}

// How many times "keys release" lets go of each modifier, in ft-screens' state and in KWin. Both
// count presses through xkbcommon, which counts a second press of a key that's already down:
// one release then leaves it down for good, and further presses and releases come in pairs, so
// it stays down until the desktop restarts (DEE-222: a Meta pressed twice like that would make
// every click Meta+click, a window move, until a reboot, as reported). ft_keyboard_key keeps that from
// happening now; these releases undo it if it happened anyway. A release of a key that isn't
// down changes nothing.
#define FT_MODIFIER_RELEASES 4

// Where keyboard focus goes when the screen that has it closes (index `closing`): the screen
// the pointer is on (`pointer`, -1 none), else the first desktop screen still open (below
// n_config), else the first spare output (a floating window's), -1 if none is left. alive[i]:
// screen i is open.
static inline int ft_focus_survivor(const bool *alive, int count, int closing, int pointer, int n_config) {
    if (pointer >= 0 && pointer < count && pointer != closing && alive[pointer]) return pointer;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < count; ++i)
            if (i != closing && alive[i] && (pass == 1 || i < n_config)) return i;
    return -1;
}

#ifdef WLR_USE_UNSTABLE
#include <wlr/types/wlr_keyboard.h>
// A key into ft-screens' keyboard (its xkb state and held keys). False for a press of a key
// that's already down: wlroots would count it twice in xkb (see FT_MODIFIER_RELEASES), so it
// changes nothing, and doesn't go on to KWin either.
static inline bool ft_keyboard_key(struct wlr_keyboard *kb, uint32_t code, bool pressed, uint32_t t) {
    if (pressed)
        for (size_t i = 0; i < kb->num_keycodes; ++i)
            if (kb->keycodes[i] == code) return false;
    struct wlr_keyboard_key_event ev = {
        .time_msec = t, .keycode = code, .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
    wlr_keyboard_notify_key(kb, &ev);
    return true;
}

// Every modifier comes up in ft-screens' keyboard, however many presses xkb counted.
static inline void ft_keyboard_release_modifiers(struct wlr_keyboard *kb, uint32_t t) {
    for (size_t i = 0; i < sizeof ft_modifier_keys / sizeof *ft_modifier_keys; ++i)
        for (int n = 0; n < FT_MODIFIER_RELEASES; ++n) ft_keyboard_key(kb, ft_modifier_keys[i], false, t);
}
#endif
#endif
