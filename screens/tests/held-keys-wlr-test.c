// ft-screens' keyboard against real wlroots and xkbcommon (held-keys.h, DEE-222).
#define WLR_USE_UNSTABLE
#include <assert.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include "../held-keys.h"
static const struct wlr_keyboard_impl impl = {.name = "test"};

// What the guard is for: wlroots counts a second press of a held key in xkb, and one release
// leaves the modifier down; a normal press and release after that doesn't clear it.
static void raw(struct wlr_keyboard *kb, uint32_t code, bool pressed) {
    struct wlr_keyboard_key_event ev = {.keycode = code, .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
    wlr_keyboard_notify_key(kb, &ev);
}

int main(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    struct wlr_keyboard kb;
    wlr_keyboard_init(&kb, &impl, "test");
    wlr_keyboard_set_keymap(&kb, keymap);

    raw(&kb, KEY_LEFTMETA, true);
    raw(&kb, KEY_LEFTMETA, true);
    raw(&kb, KEY_LEFTMETA, false);
    assert(kb.num_keycodes == 0 && (kb.modifiers.depressed & WLR_MODIFIER_LOGO));
    raw(&kb, KEY_LEFTMETA, true);
    raw(&kb, KEY_LEFTMETA, false);
    assert(kb.modifiers.depressed & WLR_MODIFIER_LOGO);  // stuck
    // "keys release" clears it, and leaves nothing else down.
    ft_keyboard_release_modifiers(&kb, 0);
    assert(kb.num_keycodes == 0 && kb.modifiers.depressed == 0);

    // Through ft_keyboard_key, a second press of a held key is refused, and one release lets go.
    assert(ft_keyboard_key(&kb, KEY_LEFTMETA, true, 0));
    assert(!ft_keyboard_key(&kb, KEY_LEFTMETA, true, 0));
    assert(kb.num_keycodes == 1 && (kb.modifiers.depressed & WLR_MODIFIER_LOGO));
    assert(ft_keyboard_key(&kb, KEY_LEFTMETA, false, 0));
    assert(kb.num_keycodes == 0 && kb.modifiers.depressed == 0);
    // Both Meta keys are two keys: each goes, and Meta stays down until both are up.
    assert(ft_keyboard_key(&kb, KEY_LEFTMETA, true, 0) && ft_keyboard_key(&kb, KEY_RIGHTMETA, true, 0));
    assert(ft_keyboard_key(&kb, KEY_LEFTMETA, false, 0) && (kb.modifiers.depressed & WLR_MODIFIER_LOGO));
    assert(ft_keyboard_key(&kb, KEY_RIGHTMETA, false, 0) && kb.modifiers.depressed == 0);
    // A release of a key that isn't down changes nothing.
    assert(ft_keyboard_key(&kb, KEY_LEFTCTRL, false, 0) && kb.modifiers.depressed == 0 && kb.num_keycodes == 0);
    // Releasing the modifiers leaves a held letter and Caps Lock's lock alone.
    assert(ft_keyboard_key(&kb, KEY_CAPSLOCK, true, 0) && ft_keyboard_key(&kb, KEY_CAPSLOCK, false, 0));
    assert(ft_keyboard_key(&kb, KEY_A, true, 0) && ft_keyboard_key(&kb, KEY_LEFTSHIFT, true, 0));
    ft_keyboard_release_modifiers(&kb, 0);
    assert(kb.num_keycodes == 1 && kb.keycodes[0] == KEY_A && kb.modifiers.depressed == 0);
    assert(kb.modifiers.locked & WLR_MODIFIER_CAPS);

    wlr_keyboard_finish(&kb);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
}
