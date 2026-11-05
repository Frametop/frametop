#include "framekeyboard/output.hpp"

#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Transport : KeySink {
    std::vector<std::string> events;
    bool pending{}, done{}, success{true}, slow_keys{}, native_pending{}, native_done{};
    bool native_success{true}, barrier{};
    std::string text;
    void send(int code, int value) override {
        events.push_back(std::to_string(code) + ":" + std::to_string(value));
    }
    Delivery deliver_key(int code, int value) override {
        if (!slow_keys || value == 0) {
            send(code, value);
            return Delivery::Success;
        }
        if (native_pending) {
            if (!native_done) {
                return Delivery::Waiting;
            }
            native_pending = false;
            return native_success ? Delivery::Success : Delivery::Failure;
        }
        send(code, value);
        native_pending = true;
        native_done = false;
        return Delivery::Waiting;
    }
    bool commit_text(const std::string& value) override {
        if (pending) {
            require(value == text, "pending text must stay immutable");
            if (!done) {
                return false;
            }
            pending = false;
            return success;
        }
        events.push_back(value);
        text = value;
        pending = true;
        done = false;
        return false;
    }
    bool text_pending() const override { return pending; }
    bool output_blocked() const override { return barrier || pending; }
    void cancel_text() override { pending = false; }
    void cancel_delivery() override { pending = native_pending = false; }
};
void character(OutputController& output, unsigned pointer, const std::string& text, double now = 1) {
    require(output.admit(true), "character admitted");
    require(output.text_press(pointer, text, now), "character queued");
    output.release(pointer);
}
void ordering() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    character(output, 1, "b");
    require(!output.take_status(), "normal pending text and queued presses stay silent");
    require(output.admit(true), "Backspace admitted behind characters");
    output.native_press(2, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(2);
    character(output, 3, "c");
    require(sink.events == std::vector<std::string>{"a"}, "later output waits for the first result");
    sink.done = true;
    output.tick(2);
    require(!output.take_status(), "the next pending delivery stays silent");
    require(sink.events == std::vector<std::string>{"a", "b"}, "second character follows first ack");
    sink.done = true;
    output.tick(3);
    require(sink.events == std::vector<std::string>{"a", "b", "14:1", "14:0", "c"},
            "text, Backspace and text preserve press order and release pairing");
    sink.done = true;
    output.tick(4);
    require(!output.text_pending(), "last result consumed after release");
    require(!output.take_status(), "successful delivery does not flash the status area");
}
void failure_and_retry() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    character(output, 1, "b");
    output.native_press(2, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(2);
    character(output, 3, "c");
    sink.done = true;
    sink.success = false;
    output.tick(2);
    require(sink.events == std::vector<std::string>{"a"} && output.retry_text() == "ab",
            "failed text retains only the prefix before discarded native actions");
    require(output.take_status()->find("discarded") != std::string::npos,
            "discarded native actions are reported");
    output.tick(100);
    require(sink.events.size() == 1, "failed delivery never retries automatically");
    sink.barrier = true;
    require(!output.retry() && sink.events.size() == 1, "retry respects a late compositor barrier");
    sink.barrier = false;
    sink.success = true;
    require(output.retry(), "one explicit retry admits the retained prefix");
    sink.done = true;
    output.tick(101);
    sink.done = true;
    output.tick(102);
    require(sink.events == std::vector<std::string>{"a", "a", "b"},
            "retry never replays Backspace or text beyond its boundary");
}
void held_keys() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    output.native_press(1, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.tick(20);
    require(sink.events == std::vector<std::string>{"a"}, "queued held key never repeats early");
    sink.done = true;
    output.tick(20);
    output.tick(20.49);
    require(sink.events == std::vector<std::string>{"a", "14:1"},
            "repeat delay begins at delivery, not acceptance");
    output.tick(20.51);
    output.release(1);
    output.tick(100);
    require(sink.events == std::vector<std::string>{"a", "14:1", "14:2", "14:0"},
            "held key repeats and receives exactly one release");
}
void release_and_capacity() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    for (unsigned i = 0; i < OutputController::capacity; ++i) {
        character(output, i + 1, "x");
    }
    require(!output.admit(true), "press beyond queue bound is rejected");
    require(output.take_status()->find("full") != std::string::npos, "queue bound has a status");
    output.release(0);
    require(sink.events == std::vector<std::string>{"14:1", "14:0"},
            "full queue cannot prevent a delivered key's release");
    output.tick(2);
    require(sink.events.back() == "x", "queued text starts after the physical hold ends");
    output.cancel();
    sink.done = true;
    output.tick(3);
    require(sink.events.size() == 3, "cancellation never drains the old queue");
}
void modifier_snapshot() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    output.native_press(1, {KEY_LEFTCTRL, KEY_C}, KEY_C, 1);
    output.release_modifier(KEY_LEFTCTRL);
    sink.done = true;
    output.tick(2);
    output.release(1);
    require(sink.events == std::vector<std::string>{"a", "29:1", "46:1", "29:0", "46:0"},
            "queued chord retains press-time modifiers even when released before delivery");
}
void native_acknowledgement() {
    Transport sink;
    sink.slow_keys = true;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(0);
    character(output, 1, "b");
    require(sink.events == std::vector<std::string>{"14:1", "14:0"},
            "release passes while native acknowledgement still blocks later text");
    sink.native_done = true;
    sink.native_success = false;
    output.tick(2);
    require(sink.events.size() == 2 && !output.text_pending(),
            "failed native action discards subsequent text even after its release");
}
void delayed_admission_clock() {
    Transport sink;
    sink.slow_keys = true;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    sink.native_done = true;
    require(output.admit(true, 20), "a new key consumes an earlier native acknowledgement");
    output.tick(20.1);
    require(sink.events == std::vector<std::string>{"14:1"},
            "a long gap before admission cannot start repeat from an old frame timestamp");
    output.release(0);
}
void composition_ownership() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    sink.done = true;
    sink.success = false;
    require(output.commit_text("preedit"),
            "accepted preedit transfers ownership even if an earlier result fails during enqueue");
    require(output.retry_text() == "apreedit" && sink.events == std::vector<std::string>{"a"},
            "failed prefix and accepted preedit are retained once, without another transport send");
}
void cancellation() {
    for (bool disable : {false, true}) {
        Transport sink;
        OutputController output(sink);
        output.set_enabled(true);
        character(output, 0, "a");
        character(output, 1, "b");
        output.native_press(2, {KEY_ENTER}, KEY_ENTER, 1);
        if (disable) {
            output.set_enabled(false);
        } else {
            output.cancel(); // Same operation used for request/route changes.
        }
        sink.done = true;
        output.tick(2);
        require(sink.events == std::vector<std::string>{"a"},
                "disable or target cancellation discards all undelivered work");
        output.set_enabled(true);
        character(output, 3, "new");
        require(sink.events == std::vector<std::string>{"a", "new"},
                "new target receives only newly accepted input");
    }
}
} // namespace
int main() {
    try {
        ordering();
        failure_and_retry();
        held_keys();
        release_and_capacity();
        modifier_snapshot();
        native_acknowledgement();
        delayed_admission_clock();
        composition_ownership();
        cancellation();
        std::cout << "Ordered output, retries, cancellation, bounds and held-key cleanup passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
