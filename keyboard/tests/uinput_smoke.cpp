#include "framekeyboard/input.hpp"

#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

// This executable never reads another keyboard. It opens the event node belonging
// to its newly created device and grabs it before emitting any test events.
int main() {
    std::unique_ptr<framekeyboard::UInputSink> sink;
    int receiver = -1;
    try {
        sink = std::make_unique<framekeyboard::UInputSink>();
        for (int attempt = 0; attempt < 100 && receiver < 0; ++attempt) {
            const auto path = sink->event_node();
            if (!path.empty()) {
                receiver = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            }
            if (receiver < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (receiver < 0 || ioctl(receiver, EVIOCGRAB, 1) < 0) {
            throw std::runtime_error("cannot grab test device; no events sent");
        }
        // Feed LED feedback only to this exclusively grabbed disposable device.
        // No system keyboard, focused app or global lock state is changed.
        for (bool enabled : {true, false}) {
            input_event leds[2]{};
            leds[0].type = EV_LED;
            leds[0].code = LED_NUML;
            leds[0].value = enabled;
            leds[1].type = EV_SYN;
            leds[1].code = SYN_REPORT;
            if (write(receiver, leds, sizeof(leds)) != sizeof(leds)) {
                throw std::runtime_error("cannot feed test-device LED state");
            }
            sink->pump();
            if (sink->num_lock_state() != std::optional<bool>(enabled)) {
                throw std::runtime_error("uinput Num Lock feedback does not match receiver LEDs");
            }
        }
        const std::vector<std::pair<int, int>> expected = {
            {KEY_ENTER, 1},    {KEY_ENTER, 0},   {KEY_LEFTCTRL, 1}, {KEY_C, 1},   {KEY_C, 0},
            {KEY_LEFTCTRL, 0}, {KEY_LEFTALT, 1}, {KEY_TAB, 1},      {KEY_TAB, 0}, {KEY_LEFTALT, 0}};
        for (const auto& [code, value] : expected) {
            sink->send(code, value);
        }
        std::vector<std::pair<int, int>> received;
        for (int attempt = 0; attempt < 20 && received.size() < expected.size(); ++attempt) {
            pollfd wait{receiver, POLLIN, 0};
            if (poll(&wait, 1, 100) <= 0) {
                continue;
            }
            input_event events[64];
            const auto bytes = read(receiver, events, sizeof(events));
            if (bytes < 0) {
                continue;
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(input_event); ++i) {
                if (events[i].type == EV_KEY) {
                    received.emplace_back(events[i].code, events[i].value);
                }
            }
        }
        if (received != expected) {
            throw std::runtime_error("kernel event sequence differs from expected sequence");
        }
        // Destroy while still grabbed, including on failure, so cleanup events
        // cannot reach the user's focused application.
        sink.reset();
        close(receiver);
        receiver = -1;
        std::cout << "Isolated uinput Enter, Ctrl+C and Alt+Tab sequences passed.\n";
        return 0;
    } catch (const std::exception& error) {
        sink.reset();
        if (receiver >= 0) {
            close(receiver);
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
