#include "framekeyboard/app.hpp"
#include "framekeyboard/ei_input.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
// Opt-in end-to-end check. Only our disposable receiver's events are inspected,
// and focus is checked before every simulated press through the production App.
int main() {
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
    XStoreName(display, window, "Full Keyboard isolated language test");
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    XMapRaised(display, window);
    XSync(display, False);
    XIM im = XOpenIM(display, nullptr, nullptr, nullptr);
    XIC ic = im ? XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                            window, XNFocusWindow, window, nullptr)
                : nullptr;
    std::string directory = "/tmp/framekeyboard-unicode-receiver-XXXXXX";
    int result = 1;
    try {
        if (!ic || !mkdtemp(directory.data())) {
            throw std::runtime_error("Cannot create isolated receiver");
        }
        EiSink sink("/run/user/" + std::to_string(getuid()) + "/gamescope-0-ei");
        Options options;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.config_dir = directory;
        App app(options, sink); // No --target-language and no changes to system layouts.
        for (int i = 0; i < 200 && !sink.text_available(); ++i) {
            app.tick(monotonic_seconds());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!sink.text_available()) {
            throw std::runtime_error("Gamescope Unicode text protocol unavailable");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        XSync(display, False);
        XSetICFocus(ic);
        unsigned test_step = 0;
        auto focused = [&] {
            Window focus{};
            int ignored{};
            XGetInputFocus(display, &focus, &ignored);
            if (focus != window) {
                throw std::runtime_error("Receiver lost focus at step " + std::to_string(test_step) +
                                         "; stopped input");
            }
        };
        auto click = [&](const std::string& action) {
            ++test_step;
            // Gamescope may restore its selected dashboard app between commits.
            // Re-focus only our own receiver, then verify ownership before sending.
            XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
            XSync(display, False);
            focused();
            app.tick(monotonic_seconds());
            auto view = app.view();
            for (int y = 96; y < panel_height; y += 4) {
                for (int x = 0; x < panel_width; x += 4) {
                    const auto* key = app.renderer.hit_key(view, x, y);
                    if (key && key->action == action) {
                        if (!app.down(0, x, y, monotonic_seconds())) {
                            throw std::runtime_error("App rejected test key");
                        }
                        app.up(0, x, y);
                        return;
                    }
                }
            }
            throw std::runtime_error("Test key unavailable");
        };
        std::string expected, received;
        auto await_text = [&](const std::string& addition) {
            expected += addition;
            // Drain each commit before the next temporary Unicode keymap is sent.
            for (int attempt = 0; attempt < 100 && received != expected; ++attempt) {
                app.tick(monotonic_seconds());
                // No input is sent here. Already-delivered events in our own
                // receiver remain safe to inspect even if dashboard focus returns.
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
                        KeySym sym{};
                        Status status{};
                        const int n =
                            Xutf8LookupString(ic, &event.xkey, buffer, sizeof(buffer), &sym, &status);
                        if ((status == XLookupChars || status == XLookupBoth) && n > 0) {
                            received.append(buffer, static_cast<std::size_t>(n));
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (received != expected) {
                throw std::runtime_error("Receiver text does not match selected layout");
            }
        };
        app.apply({"international-full", "de-de", "graphite"});
        click("KeyY");
        await_text("z");
        click("BracketLeft");
        await_text("ü");
        click("Equal");
        click("KeyE");
        await_text("é");
        app.apply({"en-us-full", "ru-ru", "graphite"});
        click("KeyQ");
        await_text("й");
        app.apply({"en-us-full", "uk-ua", "graphite"});
        click("KeyS");
        await_text("і");
        app.apply({"international-full", "fr-fr", "graphite"});
        click("KeyQ");
        await_text("a");
        app.apply({"br-abnt2-full", "pt-br", "graphite"});
        click("Semicolon");
        await_text("ç");
        app.apply({"en-us-full", "ko-kr", "graphite"});
        for (const auto* code : {"KeyG", "KeyK", "KeyS", "KeyR", "KeyM", "KeyF", "Enter"}) {
            click(code);
        }
        await_text("한글");
        for (const auto* code : {"KeyG", "KeyK", "KeyS", "Space"}) {
            click(code);
        }
        await_text("한 ");
        click("KeyR");
        click("Digit1");
        await_text("ㄱ1");
        click("KeyM");
        click("Comma");
        await_text("ㅡ,");
        for (const auto* profile : {"zh-cn-pinyin", "zh-tw-pinyin"}) {
            app.apply({"en-us-full", profile, "graphite"});
            for (const auto* code :
                 {"KeyZ", "KeyH", "KeyO", "KeyN", "KeyG", "KeyG", "KeyU", "KeyO", "Enter"}) {
                click(code);
            }
            await_text(std::string(profile) == "zh-cn-pinyin" ? "中国" : "中國");
            for (const auto* code : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO", "Comma"}) {
                click(code);
            }
            await_text("你好,");
        }
        std::cout << "Production App language switching, accents and CJK delivery passed in isolated "
                     "receiver.\n";
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
    if (directory.find("XXXXXX") == std::string::npos) {
        fs::remove_all(directory);
    }
    return result;
}
