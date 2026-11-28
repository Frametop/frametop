#ifndef FT_WAITING_KEYS_H
#define FT_WAITING_KEYS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
// Modifiers a keyboard holds that nothing has been told went down. While a remote screen has the
// typing, its computer gets a Meta combination only when the input rules give it one
// ("remote_key_bindings", conf/ft_remote_keys.py), so the input relay keeps Meta, and a modifier
// pressed after it, from everything until the key they are for says where they go:
// "key <code> 1 held". ft-screens only notes them (ft_vr_key: Super+drag, a carry), and keeps
// them here, because a laser's click or a wheel never passes the relay: one that goes to a panel
// takes the waiting modifiers there first (ft_waiting_keys_take), so Super+drag on a computer's
// screen is still its own. "key <code> 0 held" is the release: of one still waiting it is
// nothing more (ft_waiting_keys_drop), of one a click took it is a key's release like any other.
#define FT_WAITING_KEYS_MAX 8  // every modifier (held-keys.h, ft_modifier_keys)

// Where a key from the relay goes, by the word after its value: none, where typing goes;
// "here", to the Frame's own desktop though a remote screen has the typing; "held", nowhere yet.
enum ft_key_to { FT_KEY_TYPING, FT_KEY_HERE, FT_KEY_HELD };

static inline enum ft_key_to ft_key_to_word(const char *word) {
    if (!strcmp(word, "here")) return FT_KEY_HERE;
    if (!strcmp(word, "held")) return FT_KEY_HELD;
    return FT_KEY_TYPING;
}

struct ft_waiting_keys {
    uint32_t code[FT_WAITING_KEYS_MAX];
    unsigned n;
};

// A key waits, once. False when there is no room (more keys than there are modifiers).
static inline bool ft_waiting_keys_add(struct ft_waiting_keys *w, uint32_t code) {
    for (unsigned i = 0; i < w->n; ++i)
        if (w->code[i] == code) return true;
    if (w->n == FT_WAITING_KEYS_MAX) return false;
    w->code[w->n++] = code;
    return true;
}

// The next key waiting, no longer waiting (0: none), in the order they went down.
static inline uint32_t ft_waiting_keys_take(struct ft_waiting_keys *w) {
    if (!w->n) return 0;
    const uint32_t code = w->code[0];
    for (unsigned i = 1; i < w->n; ++i) w->code[i - 1] = w->code[i];
    --w->n;
    return code;
}

// A key no longer waits. False if it wasn't waiting (a click took it, or it never was).
static inline bool ft_waiting_keys_drop(struct ft_waiting_keys *w, uint32_t code) {
    for (unsigned i = 0; i < w->n; ++i)
        if (w->code[i] == code) {
            for (unsigned j = i + 1; j < w->n; ++j) w->code[j - 1] = w->code[j];
            --w->n;
            return true;
        }
    return false;
}
#endif
