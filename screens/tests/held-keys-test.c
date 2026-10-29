#include <assert.h>
#include "../held-keys.h"
int main(void) {
    // Releases with no focus wait for the next one, once each, oldest first.
    struct ft_lost_keys l = {0};
    assert(ft_lost_keys_take(&l) == 0);
    ft_lost_keys_add(&l, KEY_LEFTMETA);
    ft_lost_keys_add(&l, KEY_A);
    ft_lost_keys_add(&l, KEY_LEFTMETA);  // twice is once
    assert(l.n == 2);
    assert(ft_lost_keys_take(&l) == KEY_LEFTMETA && ft_lost_keys_take(&l) == KEY_A);
    assert(ft_lost_keys_take(&l) == 0 && l.n == 0);
    // Full: the oldest goes, the newest (a modifier let go last) stays.
    for (uint32_t c = KEY_1; c < KEY_1 + FT_LOST_KEYS_MAX; ++c) ft_lost_keys_add(&l, c);
    ft_lost_keys_add(&l, KEY_LEFTMETA);
    assert(l.n == FT_LOST_KEYS_MAX && l.code[0] == KEY_2 && l.code[FT_LOST_KEYS_MAX - 1] == KEY_LEFTMETA);
    while (ft_lost_keys_take(&l)) {}

    // Every modifier, both sides, is in the release-all list.
    const uint32_t mods[] = {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                             KEY_LEFTALT,  KEY_RIGHTALT,  KEY_LEFTMETA,  KEY_RIGHTMETA};
    assert(sizeof ft_modifier_keys / sizeof *ft_modifier_keys == 8);
    for (unsigned i = 0; i < 8; ++i) {
        bool found = false;
        for (unsigned j = 0; j < 8; ++j) found |= ft_modifier_keys[j] == mods[i];
        assert(found);
    }

    // Focus after the focused screen closes: the pointer's screen, else the first desktop
    // screen, else a spare, else none.
    bool alive[6] = {true, true, true, false, true, false};  // 3 screens, then spares 4-6
    assert(ft_focus_survivor(alive, 6, 0, 2, 3) == 2);
    assert(ft_focus_survivor(alive, 6, 0, 0, 3) == 1);   // the pointer was on the closing one
    assert(ft_focus_survivor(alive, 6, 0, -1, 3) == 1);
    assert(ft_focus_survivor(alive, 6, 1, 3, 3) == 0);   // the pointer's screen is gone too
    assert(ft_focus_survivor(alive, 6, 1, 4, 3) == 4);   // the pointer on a floating window
    assert(ft_focus_survivor(alive, 6, 0, 9, 3) == 1);   // out of range
    bool spares_only[6] = {false, false, false, true, false, true};
    assert(ft_focus_survivor(spares_only, 6, 3, -1, 3) == 5);
    bool last[6] = {true, false, false, false, false, false};
    assert(ft_focus_survivor(last, 6, 0, -1, 3) == -1);
}
