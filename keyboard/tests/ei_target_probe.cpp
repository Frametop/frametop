#include "framekeyboard/ei_input.hpp"
#include <X11/Xlib.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

// Opt-in live test. Emit only while our newly created disposable window owns
// X keyboard focus; never inspect or record events from another application.
int main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        return 2;
    }
    Window previous{};
    int revert{};
    XGetInputFocus(display, &previous, &revert);
    const auto window =
        XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 500, 180, 0, 0, 0x252525);
    XStoreName(display, window, "FrameKeyboard input test");
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    XMapRaised(display, window);
    XSync(display, False);
    int result = 1;
    try {
        framekeyboard::EiSink input("/run/user/" + std::to_string(getuid()) + "/gamescope-0-ei");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        XSync(display, False);
        const std::vector<std::pair<int, int>> sequence = {{30, 1}, {30, 0}, {28, 1}, {28, 0},
                                                           {29, 1}, {30, 1}, {30, 0}, {29, 0}};
        for (const auto& [code, value] : sequence) {
            Window focus{};
            int ignored{};
            XGetInputFocus(display, &focus, &ignored);
            if (focus != window) {
                throw std::runtime_error("Test window lost focus; stopped");
            }
            input.send(code, value);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        std::vector<std::pair<int, int>> received;
        for (int attempt = 0; attempt < 100 && received.size() < sequence.size(); ++attempt) {
            while (XPending(display)) {
                XEvent event{};
                XNextEvent(display, &event);
                if (event.xany.window == window &&
                    (event.type == KeyPress || event.type == KeyRelease)) {
                    received.emplace_back(static_cast<int>(event.xkey.keycode) - 8,
                                          event.type == KeyPress ? 1 : 0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (received != sequence) {
            throw std::runtime_error("Receiver did not receive exact compositor key sequence");
        }
        std::cout << "Dedicated Xwayland receiver passed A, Enter and Ctrl+A press/release via EIS.\n";
        result = 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    Window focus{};
    int ignored{};
    XGetInputFocus(display, &focus, &ignored);
    if (focus == window && previous != None) {
        XSetInputFocus(display, previous, revert, CurrentTime);
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return result;
}
