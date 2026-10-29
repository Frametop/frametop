#ifndef FT_HELD_KEYS_H
#define FT_HELD_KEYS_H
#include <stdbool.h>
#include <stdint.h>
#include <linux/input-event-codes.h>
// Key releases KWin hasn't had (DEE-222). wlroots sends a key only to the surface with keyboard
// focus, and drops it when none has it: after the focused screen closes, until a click or a
// spin gives another one focus. A modifier held across that (Meta) came up in ft-screens' state
// but stayed down in KWin, so every click was Meta+click, a window move. ft-screens keeps
// those releases here (ft_lost_keys_add) and sends them with the next keyboard focus
// (ft_lost_keys_take). A release of a key KWin doesn't hold changes nothing there, so one sent
// twice is harmless; a lost one isn't.
#define FT_LOST_KEYS_MAX 16

struct ft_lost_keys {
    uint32_t code[FT_LOST_KEYS_MAX];
    unsigned n;
};

// A release that had no focus to go to. When full, the oldest goes: modifiers come up last.
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
#endif
