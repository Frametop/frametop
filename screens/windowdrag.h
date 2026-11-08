// Window drag (user, 2026-11-07: "drag windows by pressing the super key and clicking on them ...
// without having to actually click the interface"): a modifier held with a mouse button moves a
// window from anywhere in it, as KWin does on the desktop's screens. ft-screens does it for the
// panels KWin can't reach: floating windows (carried like by their title bar) and PC window panels
// (carried; KWin's on the host never hears of it). The wheel then pushes a carried window into a
// screen or one of its PC's displays, where it lands, or pulls a window off a PC's display.
//
// Frametop Settings keeps the setting in frametop.conf (WINDOW_DRAG_MOD and WINDOW_DRAG_LEFT,
// _RIGHT and _MIDDLE) and sends it live ("windowdrag MOD LEFT RIGHT MIDDLE"); KWin's own mouse
// bindings follow the same setting (session/window-drag.sh). Header only, no OpenVR, so its test
// runs anywhere (screens/tests/windowdrag-test.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <linux/input-event-codes.h>

namespace windowdrag {

enum Mod { Off, Meta, Alt };
enum Action { Nothing, Move, Resize };

struct Config {
    Mod mod = Meta;
    Action left = Move, right = Resize, middle = Nothing;
};

// "meta" | "super" | "alt" | "off"; false (and `out` unchanged) for anything else.
inline bool ParseMod(const char *word, Mod *out) {
    if (!std::strcmp(word, "meta") || !std::strcmp(word, "super")) return *out = Meta, true;
    if (!std::strcmp(word, "alt")) return *out = Alt, true;
    if (!std::strcmp(word, "off")) return *out = Off, true;
    return false;
}

// "move" | "resize" | "none"; false (and `out` unchanged) for anything else.
inline bool ParseAction(const char *word, Action *out) {
    if (!std::strcmp(word, "move")) return *out = Move, true;
    if (!std::strcmp(word, "resize")) return *out = Resize, true;
    if (!std::strcmp(word, "none")) return *out = Nothing, true;
    return false;
}

// The command's words, all four or nothing changes.
inline bool ParseCommand(const char *mod, const char *left, const char *right, const char *middle, Config *out) {
    Config c = *out;
    if (!ParseMod(mod, &c.mod) || !ParseAction(left, &c.left) || !ParseAction(right, &c.right) ||
        !ParseAction(middle, &c.middle))
        return false;
    *out = c;
    return true;
}

// Whether the modifier is held now.
inline bool Held(const Config &c, bool meta, bool alt) { return c.mod == Meta ? meta : c.mod == Alt ? alt : false; }

// What a press of this button (BTN_*) does with the modifier held.
inline Action ActionOf(const Config &c, uint32_t button) {
    return button == BTN_LEFT ? c.left : button == BTN_RIGHT ? c.right : button == BTN_MIDDLE ? c.middle : Nothing;
}

// A press that carries a window: the modifier held and a button that moves.
inline bool Moves(const Config &c, uint32_t button, bool meta, bool alt) {
    return Held(c, meta, alt) && ActionOf(c, button) == Move;
}

// A host as ft-stream reaches it ("192.168.1.61" or "192.168.1.61:48989"), without the port:
// one PC's displays and its windows instance have different ports. An address with more than
// one colon (IPv6) is taken whole.
inline bool SameHost(const char *a, const char *b) {
    const auto len = [](const char *h) {
        const char *c = std::strchr(h, ':');
        return c && !std::strchr(c + 1, ':') ? std::size_t(c - h) : std::strlen(h);
    };
    const std::size_t n = len(a);
    return n == len(b) && n > 0 && !std::strncmp(a, b, n);
}

// Whether a window panel (app "window:0x...") can land on a remote panel: one of its own PC's
// displays ("display:NAME"), not another PC's, nor another window.
inline bool CanDropOn(const char *fromHost, const char *fromApp, const char *toHost, const char *toApp) {
    return !std::strncmp(fromApp, "window:", 7) && !std::strncmp(toApp, "display:", 8) && SameHost(fromHost, toHost);
}

}  // namespace windowdrag
