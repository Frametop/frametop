// The laser on our panels of Steam's own windows (steamwin.h), as those windows' pages' mouse:
// each is a page of Steam's CEF, which Steam runs with DevTools on 127.0.0.1:8080, titled as its
// X window is. X's pointer can't do it: Steam takes its windows' input from its own overlays' laser
// events and hands it to CEF itself, so XTest on gamescope's :0 reaches none of them (tried: the
// pointer doesn't move, clicks land nowhere), while Input.dispatchMouseEvent on the page does.
//
// One thread of its own (the sockets block), one socket a page, opened the first time it's needed
// (the page found by title in GET /json) and again when Steam drops it. Coordinates are window
// pixels from the top left; each page's devicePixelRatio (read when it opens) turns them into CSS
// pixels. Moves are merged while the thread is behind. The wire: cdpwire.h. It also asks a page
// what part of itself it draws (AskDrawn: Steam's menu's panel shows only that, steamglue.h).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace steamcdp {

enum class Button { Left, Right, Middle };
// From a Linux button code (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE); false for others.
inline bool FromLinux(uint32_t code, Button *b) {
    if (code < 0x110 || code > 0x112) return false;
    *b = code == 0x110 ? Button::Left : code == 0x111 ? Button::Right : Button::Middle;
    return true;
}

class Pages {
public:
    Pages();
    ~Pages();   // (its thread let go of; a send under way finishes first)
    void Move(const std::string &title, int x, int y);
    void Press(const std::string &title, int x, int y, Button b, bool down);
    void Wheel(const std::string &title, int notches);   // where it was last; + up, as OpenVR's
    // The laser left `title`: its buttons up where it was last, then its mouse off the page, so
    // what it hovered (the menu's labels, a button's highlight) hears it leave.
    void Leave(const std::string &title);
    void LetGo();   // the buttons held on any page up, where each was last

    // The part of `title`'s page it draws (steamglue.h): what its elements with a background, and
    // its leaves, cover, in CSS pixels, with the page's pixel ratio. AskDrawn asks (one ask a page
    // waits at a time); LastDrawn has the latest answer, and when it was asked (NowMs-like ms,
    // steady clock). False: none yet.
    struct Drawn {
        double l = 0, t = 0, r = 0, b = 0, dpr = 1;
        int64_t askedMs = 0;
    };
    void AskDrawn(const std::string &title);
    bool LastDrawn(const std::string &title, Drawn *out) const;

private:
    struct D;
    std::unique_ptr<D> d_;
};

}  // namespace steamcdp
