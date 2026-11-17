// gsforward::Link on a thread of its own (vr.cpp's "gamescope apps"). Link's replies wait for the X
// server, and a gamescope that hangs without closing its socket (a GPU reset) would hold the
// compositor's tick with them: every panel, the pointer and the keys. So this thread owns the
// connection: it opens the app display (every kRetryMs while it's on and closed), reads its
// events (a window's name and app with its map), and sends what it's asked to in order. The
// caller's side never waits on X. Tested against Xvfb, a stopped one too, by
// screens/tests/gslink-test.cpp.
#pragma once

#include "gsforward.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gsforward {

// What happened over there since the last Take.
struct Update {
    enum Type { Opened, Closed, Window } type = Window;
    Event event;          // Window: one of the display's
    uint32_t pid = 0;     // Opened: gamescope's
    std::string why;      // Closed: why (or why it didn't open; said again only when it changes)
    std::string display;  // Opened: which
};

class AsyncLink : public XSide {
public:
    static constexpr int kRetryMs = 10000;
    static constexpr size_t kMaxQueued = 4096;   // sends waiting at most (a pointer move replaces the last one;
                                                 // past it moves and presses drop, never a release or a tag;
                                                 // a raise or a resize replaces the last one queued)

    // `displays`: the displays to try, in order (the first that opens is the app display), asked
    // again before each try (gamescope picks them as it starts: gsdisplay.h); `needForwarding` as
    // Link::Open's.
    // `role`: which of gamescope's displays (gsforward.h).
    AsyncLink(std::function<std::vector<std::string>()> displays, bool needForwarding, Role role = Role::Apps);
    AsyncLink(std::string display, bool needForwarding)
        : AsyncLink([display] { return std::vector<std::string>{display}; }, needForwarding) {}
    ~AsyncLink() override;   // Stop(0) if not stopped
    AsyncLink(const AsyncLink &) = delete;
    AsyncLink &operator=(const AsyncLink &) = delete;

    // On: the display opened (and again after it goes). Off: closed, after what's been sent.
    void SetOn(bool on);
    std::vector<Update> Take();
    bool IsOpen() const;
    // What's been sent goes, then the connection; waits `waitMs` at most for that (a hung server:
    // the thread is left to it).
    void Stop(int waitMs);

    void Tag(uint32_t win, uint64_t overlay) override;
    void Untag(uint32_t win) override;
    void Pointer(int x, int y) override;
    void Button(uint8_t button, bool down) override;
    void Scroll(int notches) override;
    void Key(uint32_t win, uint8_t keycode, uint16_t state, bool down) override;
    void Raise(uint32_t win) override;
    void Resize(uint32_t win, int w, int h) override;

private:
    struct Shared;
    std::shared_ptr<Shared> d_;   // (shared with the thread: a thread left to a hung server keeps it)
};

}  // namespace gsforward
