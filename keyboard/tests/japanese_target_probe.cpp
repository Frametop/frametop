#include "framekeyboard/ei_input.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <chrono>
#include <clocale>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

// Opt-in live test. Emit only while our newly created disposable window owns
// X keyboard focus; never inspect or record events from another application.
int main(int argc, char** argv) {
    const bool multilingual = argc == 2 && std::string(argv[1]) == "--multilingual";
    const std::string expected = multilingual ? "éñç中国中國한글йії" : "日本";
    if (argc != 1 && argc != 3 && !multilingual) {
        std::cerr << "usage: japanese-target-probe [--multilingual | EI_SOCKET TEXT_SOCKET]\n";
        return 2;
    }
    std::setlocale(LC_ALL, "C.UTF-8");
    XSetLocaleModifiers("@im=none");
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
    XIM im = XOpenIM(display, nullptr, nullptr, nullptr);
    XIC ic = im ? XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                            window, XNFocusWindow, window, nullptr)
                : nullptr;
    int result = 1;
    try {
        const auto key_socket = argc == 3 ? std::string(argv[1])
                                          : "/run/user/" + std::to_string(getuid()) + "/gamescope-0-ei";
        framekeyboard::EiSink input(key_socket, argc == 3 ? argv[2] : "");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        XSync(display, False);
        if (!ic) {
            throw std::runtime_error("Cannot create test text receiver");
        }
        XSetICFocus(ic);
        for (int i = 0; i < 200 && !input.text_available(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!input.text_available()) {
            throw std::runtime_error("Gamescope Unicode text protocol unavailable");
        }
        Window focus{};
        int ignored{};
        XGetInputFocus(display, &focus, &ignored);
        if (focus != window) {
            throw std::runtime_error("Test window lost focus; no text sent");
        }
        bool committed = input.commit_text(expected);
        while (!committed && input.text_pending()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            committed = input.commit_text(expected);
        }
        if (!committed) {
            throw std::runtime_error("Text commit rejected");
        }
        std::string received;
        for (int attempt = 0; attempt < 100 && received != expected; ++attempt) {
            while (XPending(display)) {
                XEvent event{};
                XNextEvent(display, &event);
                if (event.type == MappingNotify) {
                    XRefreshKeyboardMapping(&event.xmapping);
                }
                if (XFilterEvent(&event, window)) {
                    continue;
                }
                if (event.xany.window == window && event.type == KeyPress) {
                    char buffer[128]{};
                    KeySym symbol{};
                    Status status{};
                    int n = Xutf8LookupString(ic, &event.xkey, buffer, sizeof(buffer), &symbol, &status);
                    if ((status == XLookupChars || status == XLookupBoth) && n > 0) {
                        received.append(buffer, static_cast<std::size_t>(n));
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (received != expected) {
            throw std::runtime_error("Disposable receiver did not receive expected Unicode text");
        }
        std::cout << "Dedicated Xwayland text receiver passed UTF-8 delivery.\n";
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
    if (ic) {
        XDestroyIC(ic);
    }
    if (im) {
        XCloseIM(im);
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return result;
}
