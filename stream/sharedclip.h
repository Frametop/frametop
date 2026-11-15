// The shared clipboard (stream/clipboard.h, docs/shared-clipboard.md): with "clipboard": true
// on its host in the layout (Frametop Remote Displays: Share the clipboard), the desktop's text
// or image goes to the host when typing goes to this panel ("focus", or a key after a blur),
// and the host's comes back when typing leaves it ("blur"). The desktop's side is ft-clip, next
// to ft-stream, on the Frametop desktop's own Wayland socket; the host's is Client::Clipboard.
// Both run on a thread of their own, in order, so the stream never waits for them. Only sizes
// are logged, never the content.
//
// ft-stream owns one per stream (stream/ft-stream.cpp): Open when the stream starts, Tick on
// every pass of its main loop, Focus and Blur as typing comes and goes, Close as it ends.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "clipboard.h"
#include "host.h"

struct SharedClipboard {
    ftstream::Client *client = nullptr;
    std::string host, state, pin, clip, wayland;
    std::atomic<bool> on{false}, gone{false};
    bool images = true;  // the thread's: false once the host says it takes text only
    bool focused = false;  // the main thread's
    int64_t checkAt = 0;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<char> ops;  // 's' send the desktop's clipboard, 'b' bring the host's back
    std::atomic<bool> quit{false};  // set under mu, read by a sync under way too
    std::set<std::string> said;  // the thread's: each problem is logged once
    std::thread thread;

    ~SharedClipboard() { Close(); }
    // ownPin: the pin kept with this stream's token. An instance with a token of its own (a
    // host's Frametop windows) may have a certificate of its own too; else the host's pin.
    void Open(ftstream::Client *c, const std::string &address, const std::string &ownPin);
    void Close();
    // The main loop, every pass (now: CLOCK_MONOTONIC ns): the switch, `shared()` asked again
    // every 3 s (the host's "clipboard" in the layout).
    void Tick(int64_t now, const std::function<bool()> &shared);
    void Focus();
    void Blur();

  private:
    void Queue(char op);
    void Loop();
    void Say(const std::string &why);
    bool Answered(long code, bool set, bool image);
    // The desktop's text, else its PNG image; empty when it holds neither. False when there's
    // no desktop to ask.
    bool Desktop(clipboard::Content *c);
    void Send();
    void Bring();
    // ft-clip WHAT on the desktop's socket, with *in on its stdin, its stdout in *out. Its exit
    // status, -1 if it didn't run or didn't finish in time (it's killed then).
    int Run(const char *what, const std::string *in, std::string *out);
};
