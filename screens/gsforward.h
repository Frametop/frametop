// Apps in SteamOS's own gamescope session as Frametop panels (screens/vrgsapps.cpp's panels;
// docs/design.md "Gamescope apps"). Graduated from the 2026-11-16 spike (branch
// spike-gamescope-panels; its README has what it showed).
//
// gamescope forwards an X11 window's frames to any SteamVR overlay that the window's
// STEAM_GAMESCOPE_VROVERLAY_TARGET property names (two 32-bit cardinals: the handle's low half,
// then its high half), once the overlay names gamescope its renderer (SetOverlayRenderingPid with
// the root window's GAMESCOPE_PID). The root's GAMESCOPE_VROVERLAY_FORWARDING says gamescope can.
// gamescope sets only the picture: where the overlay is, its size and its input stay ours. So
// ft-screens watches gamescope's app display (`:1` on the Frame; `:0` holds Steam), gives each
// app window a panel and tags it before it maps (an untagged one gets a panel of gamescope's own
// for a moment), and its menus, tooltips and dialogs overlays over it where the app put them.
//
// Input: the pointer through XTest, which on a gamescope Xwayland goes through gamescope's own
// input socket (LIBEI_SOCKET) to the window gamescope gives the pointer: that reaches a tagged
// window. Keys don't go that way: gamescope drops a tagged window from its focus list, so its
// keyboard focus (where XTest keys, and a libei client's such as Frametop's VR keyboard, land)
// is never on it. Keys go as synthetic events straight to the window instead: most apps take
// them; ones that refuse synthetic events (xterm by default, games reading raw input) don't.
//
// Two halves: the policy here (pure, tested by screens/tests/gsforward-test.cpp), and Link, the
// X connection (gsforward.cpp, xcb; tested against Xvfb by the same test). Its replies wait for the
// X server, so ft-screens runs it on a thread of its own (gslink.h).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gsforward {

// ---------------------------------------------------------------- policy

// What a window on the app display is to us.
enum class Kind {
    Ignore,   // too small, input only, or not a top-level one
    Main,     // an app's window: a panel of its own
    Popup,    // a menu, tooltip or dialog: an overlay over its app's panel
};
constexpr int kMinMain = 64;    // pixels each way: smaller ones are helpers (an app's leader window)
constexpr int kMinPopup = 4;

inline Kind Classify(bool inputOutput, bool overrideRedirect, bool transient, int w, int h) {
    if (!inputOutput) return Kind::Ignore;
    if (overrideRedirect || transient) return w >= kMinPopup && h >= kMinPopup ? Kind::Popup : Kind::Ignore;
    return w >= kMinMain && h >= kMinMain ? Kind::Main : Kind::Ignore;
}

// Which app's panel a popup goes over: the window it's transient for, if that's one of ours;
// else the one the pointer was on last (a menu opens where you clicked); else the first.
inline uint32_t PopupOwner(uint32_t transientFor, uint32_t lastPointed, const std::vector<uint32_t> &mains) {
    auto has = [&](uint32_t w) {
        for (uint32_t m : mains)
            if (m == w) return true;
        return false;
    };
    if (transientFor && has(transientFor)) return transientFor;
    if (lastPointed && has(lastPointed)) return lastPointed;
    return mains.empty() ? 0 : mains.front();
}

// The key in ft-screens' tile memory (dashtiles::Memory) for an app: Steam's app id when Steam
// gave the window one (STEAM_GAME), else the window's class, else its instance name.
inline std::string AppKey(uint32_t steamGame, const std::string &wmClass, const std::string &wmInstance) {
    if (steamGame) return "steam:" + std::to_string(steamGame);
    if (!wmClass.empty()) return "x11:" + wmClass;
    if (!wmInstance.empty()) return "x11:" + wmInstance;
    return "";
}

// A Linux key code (linux/input-event-codes.h) as X's: Xwayland's keymap is evdev's, plus 8.
inline uint8_t XKeycode(uint32_t evdev) { return evdev + 8 <= 255 ? uint8_t(evdev + 8) : 0; }

// The X modifier state a key event carries, from the modifiers held.
struct Mods {
    bool shift = false, ctrl = false, alt = false, meta = false;
};
inline uint16_t State(const Mods &m) {
    return uint16_t((m.shift ? 1 : 0) | (m.ctrl ? 4 : 0) | (m.alt ? 8 : 0) | (m.meta ? 64 : 0));
}

// The modifiers as X gives them with a key: as they were before it (a Shift's own press isn't
// shifted, its release is). `now` is after it.
inline Mods Before(Mods now, uint32_t code, bool down) {
    bool *m = nullptr;
    switch (code) {
        case 42: case 54: m = &now.shift; break;    // KEY_LEFTSHIFT, KEY_RIGHTSHIFT
        case 29: case 97: m = &now.ctrl; break;     // KEY_LEFTCTRL, KEY_RIGHTCTRL
        case 56: case 100: m = &now.alt; break;     // KEY_LEFTALT, KEY_RIGHTALT
        case 125: case 126: m = &now.meta; break;   // KEY_LEFTMETA, KEY_RIGHTMETA
        default: break;
    }
    if (m) *m = !down;
    return now;
}

// A Linux mouse button (BTN_LEFT 0x110 ...) as X's: 1 left, 2 middle, 3 right, 8 back, 9 forward.
inline uint8_t XButton(int linuxButton) {
    switch (linuxButton) {
        case 0x111: return 3;
        case 0x112: return 2;
        case 0x113: return 8;
        case 0x114: return 9;
        default: return 1;
    }
}

// The overlay handle as the target property's two cardinals.
inline void TargetHalves(uint64_t overlay, uint32_t out[2]) {
    out[0] = uint32_t(overlay & 0xffffffffu), out[1] = uint32_t(overlay >> 32);
}

// ---------------------------------------------------------------- the X connection

struct Window {
    uint32_t id = 0;
    int x = 0, y = 0, w = 0, h = 0;   // on the X screen
    Kind kind = Kind::Ignore;
    uint32_t transientFor = 0;
    bool overrideRedirect = false;    // (a menu, a tooltip: no window manager focuses it)
    std::string key, title;           // AppKey's, and its title (read when it maps)
    uint64_t target = 0;              // Role::Steam only: the overlay its target names (0: none)
};

struct Event {
    enum Type { Created, Mapped, Moved, Unmapped, Gone, Retarget } type = Created;
    Window win;   // Moved: the new rectangle; Unmapped, Gone: the id only; Retarget: the id and its target now
};

// Which of gamescope's displays a Link watches. Apps: its app display (`:1`), whose windows we
// tag, never Steam's own (GAMESCOPE_XWAYLAND_SERVER_ID 0); a tag left there by an ft-screens that
// went without untagging is cleared on opening. Steam: only Steam's own (`:0`), whose windows
// Steam tags with its own overlays: nothing is cleared, each window's target comes with it, and a
// change of one comes as Retarget (steamwin.h: Steam's dashboard window as a panel of ours).
enum class Role { Apps, Steam };

// What's sent to the app display (Link does it now; gslink.h's AsyncLink on its own thread).
struct XSide {
    virtual ~XSide() = default;
    virtual void Tag(uint32_t win, uint64_t overlay) = 0;
    virtual void Untag(uint32_t win) = 0;
    // The X pointer to (x, y) on the screen, and buttons and the wheel there.
    virtual void Pointer(int x, int y) = 0;
    virtual void Button(uint8_t button, bool down) = 0;
    virtual void Scroll(int notches) = 0;   // up positive
    // A key straight to the window (a synthetic event; see the top).
    virtual void Key(uint32_t win, uint8_t keycode, uint16_t state, bool down) = 0;
    virtual void Raise(uint32_t win) = 0;
    virtual void Resize(uint32_t win, int w, int h) = 0;
};

// The connection, on the caller's thread: its replies wait for the X server.
class Link : public XSide {
public:
    Link();
    ~Link();
    Link(const Link &) = delete;
    Link &operator=(const Link &) = delete;

    // Connects to `display`. With `needForwarding`, only a gamescope that forwards (and says its
    // pid), and not its first Xwayland (GAMESCOPE_XWAYLAND_SERVER_ID 0: Steam's own); `why` says
    // what's missing. Role::Steam: only a gamescope that forwards, and only its first Xwayland.
    // The windows already there come as Created (and Mapped).
    bool Open(const char *display, bool needForwarding, std::string *why, Role role = Role::Apps);
    void Close();   // (the tags stay: Untag first)
    bool IsOpen() const;
    uint32_t GamescopePid() const;

    // What happened since the last call (none once the connection broke: IsOpen is false then).
    std::vector<Event> Poll();
    // A window's key and title, now (it maps with them set).
    void Describe(Window *w);

    // Connected socket's fd (-1 closed): readable when events wait.
    int Fd() const;

    void Tag(uint32_t win, uint64_t overlay) override;
    void Untag(uint32_t win) override;
    void Pointer(int x, int y) override;
    void Button(uint8_t button, bool down) override;
    void Scroll(int notches) override;
    void Key(uint32_t win, uint8_t keycode, uint16_t state, bool down) override;
    void Raise(uint32_t win) override;
    void Resize(uint32_t win, int w, int h) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace gsforward
