#ifndef FT_PEERINPUT_H
#define FT_PEERINPUT_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include "vr.h"
// Input from the other people of a Together room, on one of this desktop's own screens or on a
// window of the wearer's PC that stands here as a panel (docs/together.md, "Control and
// moving"). Whose input is taken, for which screen and when
// is ft-together's to say (together/flow/control.h). What it took comes here from
// ft-roominput as "peer ..." on the control socket:
//
//   peer on                          someone may be given a screen now: for the next few
//                                    seconds ft-together hears when the wearer's own hand is
//                                    on the desktop (ft_peer_own)
//   peer <screen> move X Y           their pointer, across and down the screen, 0 to 65535
//   peer <screen> button B 0|1       B: 1 left, 2 right, 3 middle; where they last moved to
//   peer <screen> scroll RIGHT DOWN  120 a notch
//   peer <screen> key CODE 0|1       a key, by its Linux code
//   peer letgo                       every button and key they hold comes up
//
// They share the seat with the wearer, so what is kept here is theirs alone: where their
// pointer is (the wearer's may have moved the seat's since), and what they hold. A release
// goes to the seat only for what they pressed, so nothing they send lets go of a key or a
// button the wearer holds, and "letgo" leaves nothing of theirs down. The desk says which
// screens take any of it (ft_peer_desk, which the compositor answers): the desktop's own, and
// another machine's display that a stream brings here, which ft-roominput names only when it
// is a window of the wearer's PC that the room was given. Never a room's panel, a floating
// window's output, Steam or frunner's search.
//
// A screen's input goes to a seat: the desktop's one, or the other machine's through that
// display's own stream. What they hold is at one seat, their pointer's. Before their pointer
// goes to a screen of another seat everything they hold comes up where it went down, and a
// key goes down only at the seat their pointer is at.
//
// One pointer for two people is taken in turns, as ft-together has it (room/desk.h), and here
// to the event:
//
//   - The wearer's hand on the desktop (ft_peer_hand: a press, a scroll, a spin to the front,
//     anything with a button of theirs down, and a key) lets go at once of what the other
//     holds, and ft-together hears of it (ft_peer_own), so it passes nothing of theirs on for
//     a moment.
//   - The wearer's pointer only resting on a screen or passing over it (a laser, the gaze)
//     moves nothing while the other holds a button or was at the pointer in the last
//     FT_PEER_HOLD_MS (ft_peer_holds): their drag isn't pulled to where the wearer looks.
//
// The rules only: the seat is behind ft_peer_desk.

#define FT_PEER_KEYS 8          // keys they hold at once (ft-together's desk allows as many)
#define FT_PEER_FAR 65535       // a screen's far edge
#define FT_PEER_SCROLL 2400     // the most one scroll says: twenty notches
#define FT_PEER_WANTED_MS 5000  // how long "peer on" stands
#define FT_PEER_OWN_MS 500      // the wearer's own hand is told at most this often
#define FT_PEER_HOLD_MS 1500    // the pointer is theirs this long after they were at it (the desk's kHoldS)

// What the compositor does for them.
struct ft_peer_desk {
    void *ctx;
    // The size in pixels of screen `screen` (from 0). False: not one that takes their input.
    bool (*size)(void *ctx, int screen, int *width, int *height);
    // The seat its input goes to: 0 the desktop's (and for no screen, -1), another number for
    // each display of another machine.
    int (*seat)(void *ctx, int screen);
    // A motion, a button or a scroll, as if from that screen's panel.
    void (*pointer)(void *ctx, const struct ft_event *e);
    // A key. False for a press that isn't taken: typing isn't that screen's now. A release
    // comes with the screen their pointer is on.
    bool (*key)(void *ctx, int screen, uint32_t code, bool pressed);
};

struct ft_peer {
    int screen;                   // where their pointer is (from 0); -1: nowhere yet
    double x, y;                  // ...in that screen's pixels
    uint8_t buttons;              // held: a bit each for left, right, middle
    uint32_t keys[FT_PEER_KEYS];  // held, in the order they went down
    int n_keys;
    bool pointing;                // they were at the pointer since the wearer's hand was
    uint32_t pointed_at;          // ...last then (ms)
    bool wanted, said;            // "peer on" was said; the wearer's hand was told
    uint32_t wanted_at, own_at;   // ...and when (ms)
};

static inline void ft_peer_init(struct ft_peer *p) {
    memset(p, 0, sizeof *p);
    p->screen = -1;
}

static inline void ft_peer_point(const struct ft_peer *p, const struct ft_peer_desk *d, enum ft_event_type type,
                                 uint32_t button, bool pressed, double dx, double dy) {
    const struct ft_event e = {.type = type, .screen = p->screen, .x = p->x, .y = p->y, .button = button,
                               .pressed = pressed, .dx = dx, .dy = dy};
    d->pointer(d->ctx, &e);
}

// Everything they hold comes up: the keys, the last pressed first, then the buttons.
static inline void ft_peer_letgo(struct ft_peer *p, const struct ft_peer_desk *d) {
    while (p->n_keys) d->key(d->ctx, p->screen, p->keys[--p->n_keys], false);
    int width, height;
    const bool there = p->screen >= 0 && d->size(d->ctx, p->screen, &width, &height);
    for (int b = 0; b < 3; ++b)
        if (p->buttons & (1u << b)) {
            p->buttons &= (uint8_t)~(1u << b);
            if (there) ft_peer_point(p, d, FT_BUTTON, BTN_LEFT + (uint32_t)b, false, 0, 0);  // a screen that went took its buttons along
        }
}

// The seat let go of every button itself (the pointer left the screens, Steam's PIN locked).
static inline void ft_peer_buttons_gone(struct ft_peer *p) { p->buttons = 0; }
// ...and of every key.
static inline void ft_peer_keys_gone(struct ft_peer *p) { p->n_keys = 0; }

// Whether one of the wearer's own pointer events is their hand on the desktop, and not a
// pointer that rests on a screen or passes over it. `down`: the seat holds a button.
static inline bool ft_peer_hand(const struct ft_peer *p, const struct ft_event *e, bool down) {
    if (e->type == FT_SCROLL || e->type == FT_FRONT || (e->type == FT_BUTTON && e->pressed)) return true;
    return down && !p->buttons;   // a drag of theirs, and its end
}

// Whether the pointer is the other's now: what of the wearer's isn't their hand is dropped.
static inline bool ft_peer_holds(const struct ft_peer *p, uint32_t now) {
    return p->buttons || (p->pointing && now - p->pointed_at < FT_PEER_HOLD_MS);
}

// The wearer's own hand is on the desktop (ft_peer_hand, or a key): what the other holds
// comes up, and the pointer is the wearer's. True when ft-together should hear it now:
// someone may be given a screen, and it wasn't told just before.
static inline bool ft_peer_own(struct ft_peer *p, const struct ft_peer_desk *d, uint32_t now) {
    ft_peer_letgo(p, d);
    p->pointing = false;
    if (!p->wanted || now - p->wanted_at >= FT_PEER_WANTED_MS) return p->wanted = false;
    if (p->said && now - p->own_at < FT_PEER_OWN_MS) return false;
    p->said = true, p->own_at = now;
    return true;
}

// The words after "peer ", done. The reply: "ok", "ok" and why nothing was done, or "error" and why.
static inline const char *ft_peer_command(struct ft_peer *p, const struct ft_peer_desk *d, const char *words, uint32_t now) {
    static const char *const usage = "error peer on | peer letgo | peer <screen> move X Y | button B 0|1 | scroll RIGHT DOWN | key CODE 0|1";
    if (!strcmp(words, "on")) return p->wanted = true, p->wanted_at = now, "ok";
    if (!strcmp(words, "letgo")) return ft_peer_letgo(p, d), "ok";
    int screen, a, b, width, height;
    char what[8], tail;
    if (p->screen >= 0 && !d->size(d->ctx, p->screen, &width, &height)) ft_peer_letgo(p, d), p->screen = -1;  // their screen went
    if (sscanf(words, "%9d %7s %9d %9d %c", &screen, what, &a, &b, &tail) != 4) return usage;
    if (screen < 1 || !d->size(d->ctx, --screen, &width, &height)) return "error not a screen that takes their input";
    const bool same_seat = d->seat(d->ctx, screen) == d->seat(d->ctx, p->screen);
    if (!strcmp(what, "move")) {
        if (a < 0 || a > FT_PEER_FAR || b < 0 || b > FT_PEER_FAR) return usage;
        if (!same_seat) ft_peer_letgo(p, d);  // what they hold stays behind, at the seat it went down at
        p->screen = screen, p->x = (a + 0.5) * width / (FT_PEER_FAR + 1.0), p->y = (b + 0.5) * height / (FT_PEER_FAR + 1.0);
        p->pointing = true, p->pointed_at = now;
        ft_peer_point(p, d, FT_MOTION, 0, false, 0, 0);
        return "ok";
    }
    if (!strcmp(what, "button")) {
        if (a < 1 || a > 3 || (b != 0 && b != 1)) return usage;
        if (p->screen != screen) return "error their pointer isn't on that screen";
        const uint8_t bit = (uint8_t)(1u << (a - 1));
        const bool held = p->buttons & bit;
        if (held == (b == 1)) return held ? "ok held already" : "ok not held";
        p->buttons ^= bit;
        if (b) p->pointing = true, p->pointed_at = now;  // not a release: the desk lets go for them after the wearer's hand
        ft_peer_point(p, d, FT_BUTTON, a == 1 ? BTN_LEFT : a == 2 ? BTN_RIGHT : BTN_MIDDLE, b, 0, 0);
        return "ok";
    }
    if (!strcmp(what, "scroll")) {
        if (a < -FT_PEER_SCROLL || a > FT_PEER_SCROLL || b < -FT_PEER_SCROLL || b > FT_PEER_SCROLL || (!a && !b)) return usage;
        if (p->screen != screen) return "error their pointer isn't on that screen";
        p->pointing = true, p->pointed_at = now;
        ft_peer_point(p, d, FT_MOTION, 0, false, 0, 0);  // back where they point: the wearer's pointer may have been elsewhere since
        ft_peer_point(p, d, FT_SCROLL, 0, false, a / 120.0, b / 120.0);
        return "ok";
    }
    if (!strcmp(what, "key")) {
        if (a < 1 || a >= BTN_MISC || (b != 0 && b != 1)) return usage;
        int at = 0;
        while (at < p->n_keys && p->keys[at] != (uint32_t)a) ++at;
        if (b) {
            if (at < p->n_keys) return "ok held already";
            if (p->n_keys == FT_PEER_KEYS) return "error too many keys held";
            if (!same_seat) return "ok not taken: their pointer is at another machine";
            if (!d->key(d->ctx, screen, (uint32_t)a, true)) return "ok not taken: typing isn't that screen's, or the key is down";
            p->keys[p->n_keys++] = (uint32_t)a;
            return "ok";
        }
        if (at == p->n_keys) return "ok not held";
        --p->n_keys;
        memmove(&p->keys[at], &p->keys[at + 1], (size_t)(p->n_keys - at) * sizeof p->keys[0]);
        d->key(d->ctx, p->screen, (uint32_t)a, false);  // where it went down: their pointer's seat
        return "ok";
    }
    return usage;
}
#endif
