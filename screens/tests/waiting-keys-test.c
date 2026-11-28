#include <assert.h>
#include <linux/input-event-codes.h>
#include "../waiting-keys.h"
int main(void) {
    // The word after a key's value says where it goes; anything else is where typing goes.
    assert(ft_key_to_word("") == FT_KEY_TYPING && ft_key_to_word("here") == FT_KEY_HERE);
    assert(ft_key_to_word("held") == FT_KEY_HELD && ft_key_to_word("hel") == FT_KEY_TYPING);
    assert(ft_key_to_word("heldx") == FT_KEY_TYPING && ft_key_to_word("HERE") == FT_KEY_TYPING);

    // Modifiers wait once each, and a click takes them in the order they went down.
    struct ft_waiting_keys w = {0};
    assert(ft_waiting_keys_take(&w) == 0);
    assert(ft_waiting_keys_add(&w, KEY_LEFTMETA) && ft_waiting_keys_add(&w, KEY_LEFTSHIFT));
    assert(ft_waiting_keys_add(&w, KEY_LEFTMETA) && w.n == 2);  // twice is once
    assert(ft_waiting_keys_take(&w) == KEY_LEFTMETA && ft_waiting_keys_take(&w) == KEY_LEFTSHIFT);
    assert(ft_waiting_keys_take(&w) == 0 && w.n == 0);

    // Its release while it still waits: it's gone, and the others keep their order.
    assert(ft_waiting_keys_add(&w, KEY_LEFTMETA) && ft_waiting_keys_add(&w, KEY_LEFTALT));
    assert(ft_waiting_keys_add(&w, KEY_LEFTSHIFT));
    assert(ft_waiting_keys_drop(&w, KEY_LEFTALT) && w.n == 2);
    assert(!ft_waiting_keys_drop(&w, KEY_LEFTALT));  // once
    assert(ft_waiting_keys_take(&w) == KEY_LEFTMETA && ft_waiting_keys_take(&w) == KEY_LEFTSHIFT);
    // ...and after a click took it: not waiting, so its release is a key's like any other.
    assert(!ft_waiting_keys_drop(&w, KEY_LEFTMETA) && !ft_waiting_keys_drop(&w, KEY_A));

    // No room past every modifier: the ninth isn't kept, the eight are.
    for (uint32_t c = KEY_1; c < KEY_1 + FT_WAITING_KEYS_MAX; ++c) assert(ft_waiting_keys_add(&w, c));
    assert(!ft_waiting_keys_add(&w, KEY_A) && w.n == FT_WAITING_KEYS_MAX);
    assert(ft_waiting_keys_add(&w, KEY_1));  // one that waits already still does
    assert(ft_waiting_keys_take(&w) == KEY_1 && ft_waiting_keys_add(&w, KEY_A));
}
