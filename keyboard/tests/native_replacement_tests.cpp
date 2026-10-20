#include "framekeyboard/app.hpp"
#include "framekeyboard/native_replacement.hpp"
#include "framekeyboard/vr_runtime.hpp"
#include <cerrno>
#include <chrono>
#include <iostream>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct Local : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    void send(int code, int value) override { keys.emplace_back(code, value); }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        text.push_back(value);
        return true;
    }
};
void put(int fd, const std::string& text) {
    require(::send(fd, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()),
            "fixture packet send");
}
std::string take(int fd) {
    char data[32769];
    const auto size = recv(fd, data, sizeof(data), MSG_DONTWAIT);
    require(size > 0, "fixture packet receive");
    return {data, static_cast<std::size_t>(size)};
}
void activate(int fd, NativeReplacement& replacement, double now = 10) {
    put(fd, R"({"op":"state","version":1,"request":2,"open":true,"eligible":true,"active":true})");
    replacement.poll(now);
    require(replacement.state().active && replacement.state().request == 2,
            "versioned session metadata");
    replacement.route(true);
}
void update_shutdown() {
    Local local;
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0,
            "private replacement shutdown transport");
    {
        NativeReplacement replacement(local, pair[0]);
        activate(pair[1], replacement);
        replacement.shutdown_for_update();
        const auto packet = take(pair[1]);
        json_object* message = json_tokener_parse(packet.c_str());
        require(message, "shutdown control is a valid protocol message");
        require(std::string(json_object_get_string(json_object_object_get(message, "op"))) == "stop" &&
                    json_object_object_get(message, "restore") &&
                    !json_object_get_boolean(json_object_object_get(message, "restore")),
                "prepared update must explicitly close the old session without native recovery");
        json_object_put(message);
        require(!replacement.state().open && !replacement.state().active,
                "retired replacement no longer owns a typing session");
    }
    close(pair[1]);
}
void hidden_session_wakeup() {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    Local local;
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0, "wakeup socketpair");
    {
        NativeReplacement replacement(local, pair[0]);
        const std::string idle =
            R"({"op":"state","version":1,"request":1,"open":false,"eligible":false,"active":false})";
        put(pair[1], idle);
        replacement.poll(monotonic_seconds());
        {
            std::jthread helper([&] {
                std::this_thread::sleep_for(20ms);
                put(pair[1], idle);
                std::this_thread::sleep_for(40ms);
                put(pair[1],
                    R"({"op":"state","version":1,"request":2,"open":true,"eligible":false,"active":false})");
            });
            const auto start = Clock::now();
            replacement.wait_until(start + 250ms);
            require(replacement.state().open && !replacement.state().eligible,
                    "pending native close wakes the hidden loop before eligibility");
            require(Clock::now() - start < 200ms,
                    "summon cannot consume a second 250 ms idle sleep from its 400 ms ack budget");
        }
        {
            std::jthread helper([&] {
                std::this_thread::sleep_for(20ms);
                put(pair[1],
                    R"({"op":"state","version":1,"request":2,"open":true,"eligible":true,"active":false})");
            });
            replacement.wait_until(Clock::now() + 250ms);
            require(replacement.state().eligible, "native close completion also wakes the hidden loop");
        }
        char byte;
        require(recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN,
                "idle waiting never renews progress, readiness or presentation acknowledgements");
        put(pair[1], idle);
        replacement.poll(monotonic_seconds());
        {
            std::jthread helper([&] {
                std::this_thread::sleep_for(20ms);
                put(pair[1], idle);
            });
            const auto start = Clock::now();
            replacement.wait_until(start + 100ms);
            require(Clock::now() - start >= 100ms,
                    "unchanged idle replies retain the original frame deadline");
        }
        {
            std::jthread helper([&] {
                std::this_thread::sleep_for(20ms);
                put(pair[1], R"({"op":"unavailable","reason":"reply-timeout"})");
            });
            replacement.wait_until(Clock::now() + 250ms);
            require(!replacement.state().available, "helper loss wakes the idle loop for fallback");
        }
        require(local.keys.empty() && local.text.empty(), "idle wakeup never delivers input");
    }
    close(pair[1]);
}
void transport() {
    Local local;
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0, "fixture socketpair");
    {
        NativeReplacement replacement(local, pair[0]);
        require(replacement.commit_text("local"), "ordinary text bypasses bridge");
        activate(pair[1], replacement);
        require(!replacement.commit_text("café 世界") && replacement.text_pending(),
                "commit waits for acknowledgement");
        auto* message = json_tokener_parse(take(pair[1]).c_str());
        require(std::string(json_object_get_string(json_object_object_get(message, "text"))) ==
                    "café 世界",
                "Unicode retained by transport");
        const auto id = json_object_get_uint64(json_object_object_get(message, "id"));
        json_object_put(message);
        require(!replacement.commit_text("other"), "pending text cannot be overwritten");
        put(pair[1], "{\"op\":\"text-result\",\"id\":" + std::to_string(id + 1) + ",\"ok\":true}");
        replacement.poll(10.1);
        require(!replacement.commit_text("café 世界"), "stale result ignored");
        put(pair[1], "{\"op\":\"text-result\",\"id\":" + std::to_string(id) + ",\"ok\":true}");
        replacement.poll(10.2);
        require(replacement.commit_text("café 世界") && !replacement.text_pending(),
                "success consumed once");
        require(local.text.size() == 1, "remote text never leaks to the focused local app");
        replacement.send(KEY_ENTER, 1);
        replacement.send(KEY_ENTER, 0);
        require(take(pair[1]).find("Enter") != std::string::npos && local.keys.empty(),
                "native field Enter uses Steam session");
        require(take(pair[1]).find("key-up") != std::string::npos,
                "release cancels helper repeats without sending local input");
        replacement.dismiss_request();
        require(take(pair[1]) == "{\"op\":\"dismiss\",\"request\":2}",
                "panel dismissal is scoped to the current native request");
        replacement.send(KEY_LEFTCTRL, 1);
        replacement.send(KEY_ENTER, 1);
        replacement.send(KEY_LEFTCTRL, 0);
        replacement.send(KEY_ENTER, 2);
        replacement.send(KEY_ENTER, 0);
        for (const auto code : {KEY_W, KEY_C, KEY_V}) {
            replacement.send(KEY_LEFTCTRL, 1);
            replacement.send(code, 1);
            replacement.send(code, 2);
            replacement.send(KEY_LEFTCTRL, 0);
            replacement.send(code, 0);
        }
        replacement.send(KEY_LEFTMETA, 1);
        replacement.send(KEY_LEFTMETA, 0);
        replacement.send(KEY_DELETE, 1);
        replacement.send(KEY_DELETE, 0);
        char byte;
        require(local.keys.empty() && recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN,
                "Steam fields block shortcuts and unsupported controls on both input routes");
        require(replacement.take_unsupported_control() && !replacement.take_unsupported_control(),
                "unsupported controls produce a consumable user notice");
        replacement.route(false);
        replacement.send(KEY_LEFTCTRL, 1);
        replacement.send(KEY_ENTER, 1);
        replacement.send(KEY_LEFTCTRL, 0);
        replacement.route(true);
        replacement.send(KEY_ENTER, 2);
        replacement.send(KEY_ENTER, 0);
        require(local.keys ==
                    std::vector<std::pair<int, int>>{
                        {KEY_LEFTCTRL, 1}, {KEY_ENTER, 1}, {KEY_LEFTCTRL, 0}, {KEY_ENTER, 0}},
                "local shortcuts remain supported, with release preserved across a route change");
        require(!replacement.commit_text("unsent"), "begin next commit");
        take(pair[1]);
        activate(pair[1], replacement, 10.5);
        replacement.poll(10.9);
        require(!replacement.commit_text("unsent") && !replacement.text_pending(),
                "timeout consumes failure without success");
        replacement.update(true, true, 2, false, 10.9);
        require(take(pair[1]).find("\"fallback\":true") != std::string::npos,
                "timeout requests native restoration");
    }
    close(pair[1]);
}
void ordered_transport() {
    Local local;
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0, "ordered socketpair");
    {
        NativeReplacement replacement(local, pair[0]);
        activate(pair[1], replacement);
        OutputController output(replacement);
        output.set_enabled(true);
        auto packet_id = [&](const char* operation) {
            auto* message = json_tokener_parse(take(pair[1]).c_str());
            require(message && std::string(json_object_get_string(
                                   json_object_object_get(message, "op"))) == operation,
                    "ordered fixture operation");
            const auto id = json_object_get_uint64(json_object_object_get(message, "id"));
            json_object_put(message);
            return id;
        };
        auto empty = [&] {
            char byte;
            return recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN;
        };
        auto result = [&](const char* op, std::uint64_t id, bool ok, double now) {
            put(pair[1], std::string("{\"op\":\"") + op + "-result\",\"id\":" + std::to_string(id) +
                             ",\"ok\":" + (ok ? "true}" : "false}"));
            replacement.poll(now);
            output.tick(now);
        };
        output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 10);
        const auto first = packet_id("key");
        output.release(0);
        packet_id("key-up");
        require(output.text_press(1, "b", 10), "text accepted behind native action");
        output.release(1);
        require(empty(), "native acknowledgement orders text even after early release");
        result("key", first, true, 10.1);
        const auto text = packet_id("text");
        result("text", text, true, 10.2);
        require(empty(), "each confirmed action is delivered once");

        output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 10.2);
        const auto failed = packet_id("key");
        output.release(0);
        packet_id("key-up");
        output.text_press(1, "discard", 10.2);
        output.release(1);
        result("key", failed, false, 10.3);
        require(empty(), "failed native action cannot leak queued text after release");

        output.cancel();
        output.text_press(0, "old", 10.3);
        const auto old = packet_id("text");
        output.release(0);
        output.text_press(1, "queued-old", 10.3);
        output.release(1);
        output.cancel();
        put(pair[1],
            R"({"op":"state","version":1,"request":3,"open":true,"eligible":true,"active":true})");
        replacement.poll(10.4);
        output.text_press(0, "new", 10.4);
        const auto fresh = packet_id("text");
        output.release(0);
        result("text", old, true, 10.4);
        require(empty() && output.text_pending(), "late old acknowledgement cannot unlock new work");
        result("text", fresh, true, 10.5);
        require(empty() && !output.text_pending() && local.keys.empty() && local.text.empty(),
                "request change never replays queued input or escapes to compositor input");
    }
    close(pair[1]);
}
void delivered_local_route() {
    Local local;
    NativeReplacement replacement(local);
    require(replacement.deliver_key(KEY_LEFTCTRL, 1) == Delivery::Success,
            "local delivery retains wrapper ownership");
    replacement.route(true);
    require(replacement.deliver_key(KEY_LEFTCTRL, 0) == Delivery::Success &&
                local.keys == std::vector<std::pair<int, int>>{{KEY_LEFTCTRL, 1}, {KEY_LEFTCTRL, 0}},
            "a local down always releases locally after route change");
}
void helper_retry_backoff() {
    Local local;
    int peer = -1;
    unsigned attempts = 0;
    NativeReplacement replacement(local, -1, [&] {
        int pair[2];
        require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0,
                "retry fixture socketpair");
        if (peer >= 0) {
            close(peer);
        }
        peer = pair[1];
        ++attempts;
        return pair[0];
    });
    double now = 100;
    auto update = [&](double time) { replacement.update(true, false, 0, false, time); };
    auto fail = [&](const std::string& reason) {
        put(peer, "{\"op\":\"unavailable\",\"reason\":\"" + reason + "\"}");
        replacement.poll(now);
    };
    auto expect_delay = [&](double delay) {
        const auto previous = attempts;
        update(now + delay - .01);
        require(attempts == previous, "helper retry must wait for backoff");
        now += delay;
        update(now);
        require(attempts == previous + 1, "helper retries when backoff expires");
    };
    update(now);
    for (double delay : {5, 10, 20, 40, 60, 60}) {
        fail("debugger-unavailable");
        expect_delay(delay);
    }
    put(peer, R"({"op":"state","version":1,"open":false})");
    replacement.poll(now);
    fail("debugger-unavailable");
    expect_delay(5); // A successful state resets consecutive failures.
    fail("debugger-blocked");
    expect_delay(5);
    fail("steam-incompatible");
    expect_delay(10);
    fail("helper-unavailable");
    replacement.update(false, false, 0, false, now);
    update(now);
    fail("helper-unavailable");
    expect_delay(5); // Re-enable resets the delay.
    fail("debugger-unavailable");
    const auto previous = attempts;
    replacement.retry_now(++now);
    update(now);
    require(attempts == previous + 1, "explicit summon retries promptly");
    for (const auto* reason : {"replacement-busy", "connection-lost"}) {
        for (int i = 0; i < 3; ++i) {
            fail(reason);
            expect_delay(5);
        }
    }
    replacement.update(false, false, 0, false, now);
    close(peer);

    unsigned failures = 0;
    NativeReplacement missing(local, -1, [&] {
        ++failures;
        return -1;
    });
    missing.update(true, false, 0, false, 0);
    missing.update(true, false, 0, false, 4.9);
    require(failures == 1, "spawn failure waits five seconds");
    missing.update(true, false, 0, false, 5);
    missing.update(true, false, 0, false, 14.9);
    require(failures == 2, "consecutive spawn failures also back off");
    missing.update(true, false, 0, false, 15);
    require(failures == 3, "spawn failure retry uses doubled delay");
}
void installation_budget() {
    for (bool complete : {false, true}) {
        Local local;
        int peer = -1;
        NativeReplacement replacement(local, -1, [&] {
            int pair[2];
            require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0,
                    "installation fixture socketpair");
            peer = pair[1];
            return pair[0];
        });
        replacement.update(true, false, 0, false, 100);
        take(peer);
        const std::string waiting = replacement.unavailable_reason();
        replacement.poll(102.2);
        require(replacement.unavailable_reason() == waiting && !replacement.state().available,
                "startup survives ordinary freshness timeout without claiming availability");
        if (complete) {
            put(peer, R"({"op":"state","version":1,"open":false})");
            replacement.poll(102.3);
            require(replacement.state().available, "slow installation reaches normal operation");
            replacement.poll(103.11);
            require(!replacement.state().available,
                    "first state restores the ordinary 800 ms freshness limit");
        } else {
            replacement.update(true, false, 0, false, 102.9);
            put(peer, R"({"op":"state","version":1,"open":false})");
            replacement.poll(103.01);
            require(!replacement.state().available &&
                        std::string(replacement.unavailable_reason()) ==
                            "Steam keyboard bridge installation timed out.",
                    "startup has a fixed deadline, including late state and repeated updates");
        }
        replacement.update(false, false, 0, false, 104);
        close(peer);
    }
}
void sanitized_diagnostics() {
    std::ostringstream output;
    struct Capture {
        std::streambuf* previous;
        explicit Capture(std::ostream& out) : previous(std::cout.rdbuf(out.rdbuf())) {}
        ~Capture() { std::cout.rdbuf(previous); }
    } capture(output);
    Local local;
    int peer = -1;
    NativeReplacement replacement(local, -1, [&] {
        int pair[2];
        require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0,
                "diagnostic fixture socketpair");
        if (peer >= 0) {
            close(peer);
        }
        peer = pair[1];
        return pair[0];
    });
    auto update = [&](double now) { replacement.update(true, false, 0, false, now); };
    auto state = [&](double now, const std::string& events) {
        put(peer,
            "{\"op\":\"state\",\"version\":1,\"events\":" + events + ",\"text\":\"PRIVATE_SENTINEL\"}");
        replacement.poll(now);
    };
    update(100);
    const std::string events =
        R"([{"id":1,"request":2,"code":"begin"},{"id":2,"request":2,"code":"acknowledged"},{"id":3,"request":3,"code":"owner-changed"},{"id":4,"request":4,"code":"owner-hidden"},{"id":5,"request":4,"code":"PRIVATE_SENTINEL"}])";
    state(100, events);
    const auto first = output.str();
    require(first.find("[native-replacement] owner-changed") != std::string::npos &&
                first.find("[native-replacement] owner-hidden") != std::string::npos,
            "frame-switch session events reach the sanitized diagnostic log");
    state(100.1, events);
    require(output.str() == first, "repeated event sequences log each transition once");
    state(100.1, R"([null,42,"PRIVATE_SENTINEL",{"id":6,"request":4,"code":"PRIVATE_SENTINEL"}])");
    require(output.str() == first, "unknown and malformed diagnostic events are ignored");
    for (int i = 0; i < 3; ++i) {
        put(peer, R"({"op":"unavailable","reason":"debugger-unavailable","text":"PRIVATE_SENTINEL"})");
        replacement.poll(100.2);
        replacement.retry_now(100.2);
        update(100.2);
    }
    const auto failed = output.str();
    const auto failure = failed.find("debugger-unavailable retry=5s");
    require(failure != std::string::npos &&
                failed.find("debugger-unavailable", failure + 1) == std::string::npos,
            "identical failures and retry delays are logged once within the repeat window");
    std::string flood = "[";
    for (int i = 1; i <= 100; ++i) {
        if (i > 1) {
            flood += ',';
        }
        flood += "{\"id\":" + std::to_string(i) + ",\"request\":2,\"code\":\"begin\"}";
    }
    flood += ']';
    state(101.5, flood);
    const auto limited = output.str();
    state(101.6, flood);
    require(output.str() == limited, "event replays cannot bypass the per-second log limit");
    put(peer, R"({"op":"unavailable","reason":"PRIVATE_SENTINEL"})");
    replacement.poll(103);
    const auto all = output.str();
    require(all.find("PRIVATE_SENTINEL") == std::string::npos,
            "diagnostics never echo packet text or unknown codes");
    std::istringstream lines(all);
    std::string line;
    std::size_t count = 0;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string prefix, code;
        fields >> prefix >> code;
        require(prefix == "[native-replacement]" &&
                    std::set<std::string>{"helper-start", "available", "begin", "acknowledged",
                                          "owner-changed", "owner-hidden", "debugger-unavailable",
                                          "helper-unavailable"}
                        .contains(code),
                "every logged code belongs to the fixed diagnostic set");
        ++count;
    }
    require(count <= 17, "rapid event and failure bursts produce bounded log output");
    replacement.update(false, false, 0, false, 104);
    close(peer);
}
void named_key_repeats() {
    Local local;
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0, "repeat socketpair");
    {
        NativeReplacement replacement(local, pair[0]);
        activate(pair[1], replacement);
        const auto read_id = [&](const char* kind, int value) {
            auto* message = json_tokener_parse(take(pair[1]).c_str());
            require(std::string(json_object_get_string(json_object_object_get(message, "op"))) == kind,
                    "key operation");
            if (value) {
                require(json_object_get_int(json_object_object_get(message, "value")) == value,
                        "key distinguishes initial press from repeat");
            }
            const auto id = json_object_get_uint64(json_object_object_get(message, "id"));
            json_object_put(message);
            return id;
        };
        const auto empty = [&] {
            char byte;
            require(recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN,
                    "unacknowledged repeats do not accumulate");
        };
        const auto ack = [&](std::uint64_t id) {
            put(pair[1], "{\"op\":\"key-result\",\"id\":" + std::to_string(id) + ",\"ok\":true}");
            replacement.poll(10.1);
        };
        replacement.send(KEY_BACKSPACE, 1);
        const auto first = read_id("key", 1);
        for (int i = 0; i < 10; ++i) {
            replacement.send(KEY_BACKSPACE, 2);
        }
        empty();
        ack(first);
        replacement.send(KEY_BACKSPACE, 2);
        require(read_id("key", 2) == first, "repeat belongs to its initial hold");
        replacement.send(KEY_BACKSPACE, 2);
        empty();
        replacement.route(false); // App cancellation releases through the original hold.
        replacement.send(KEY_BACKSPACE, 0);
        require(read_id("key-up", 0) == first && local.keys.empty(),
                "route change releases remote hold");
        replacement.route(true);
        replacement.send(KEY_BACKSPACE, 1);
        const auto second = read_id("key", 1);
        require(second != first, "separate rapid clicks have distinct identities");
        ack(first);
        replacement.send(KEY_BACKSPACE, 2);
        empty();
        ack(second);
        replacement.send(KEY_LEFTCTRL, 1);
        replacement.send(KEY_BACKSPACE, 2);
        empty();
        replacement.send(KEY_LEFTCTRL, 0);
        replacement.send(KEY_BACKSPACE, 2);
        require(read_id("key", 2) == second, "remote repeat resumes after modifier release");
        ack(second);
        put(pair[1],
            R"({"op":"state","version":1,"request":3,"open":true,"eligible":true,"active":true})");
        replacement.poll(10.2);
        replacement.send(KEY_BACKSPACE, 2);
        empty();
        close(pair[1]);
        replacement.send(KEY_BACKSPACE, 0);
        require(!replacement.state().available && replacement.take_input_reset(),
                "a lost release withdraws the helper instead of leaving repeats live");
    }
}
struct Deferred : Local {
    bool pending{}, done{}, success{};
    std::string held;
    int deliveries{};
    bool commit_text(const std::string& value) override {
        if (pending) {
            if (value != held || !done) {
                return false;
            }
            pending = false;
            return success;
        }
        held = value;
        pending = true;
        done = false;
        ++deliveries;
        return false;
    }
    bool text_pending() const override { return pending; }
    void cancel_text() override { pending = false; }
};
void app_commit() {
    Deferred sink;
    Options options;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.config_dir =
        fs::temp_directory_path() / ("framekeyboard-bridge-test-" + std::to_string(getpid()));
    fs::remove_all(options.config_dir);
    {
        App app(options, sink);
        auto press = [&](const std::string& action, double now) {
            const auto view = app.view();
            for (int y = 96; y < panel_height; y += 4) {
                for (int x = 0; x < panel_width; x += 4) {
                    const auto* key = app.renderer.hit_key(view, x, y);
                    if (!key || key->action != action) {
                        continue;
                    }
                    const bool accepted = app.down(1, x, y, now);
                    if (accepted) {
                        require(app.pointer_pressed(1), "accepted queued key owns its pressed visual");
                    }
                    app.up(1, x, y);
                    return accepted;
                }
            }
            throw std::runtime_error("fixture key missing");
        };
        require(!app.replace_native_keyboard(), "replacement defaults off");
        require(press("AltLeft", .5) && press("AltLeft", .6), "tap and unlatch Left Alt");
        require(sink.keys.empty() && sink.deliveries == 0 && !app.take_native_fallback(),
                "Left Alt alone emits no input and does not request native fallback");
        require(press("KeyA", 1) && sink.held == "a", "App queues printable Unicode");
        require(press("KeyB", 1.1) && sink.deliveries == 1,
                "another key is accepted without overtaking pending text");
        sink.done = true;
        sink.success = false;
        app.tick(1.2);
        press("Enter", 1.3);
        require(sink.deliveries == 2 && sink.held == "a", "failure preserves text for explicit retry");
        sink.done = sink.success = true;
        app.tick(1.4);
        require(sink.pending && sink.held == "b", "one retry preserves the queued character too");
        sink.done = true;
        app.tick(1.45);
        require(!sink.pending && press("KeyB", 1.5) && sink.held == "b",
                "acknowledgement unlocks next text");
        require(press("KeyC", 1.6), "queued character accepted before target cancellation");
        app.cancel();
        require(!sink.pending, "target cancellation invalidates deferred text");
        const auto view = app.view();
        for (int y = 96; y < panel_height; y += 4) {
            bool found = false;
            for (int x = 0; x < panel_width; x += 4) {
                const auto* key = app.renderer.hit_key(view, x, y);
                if (!key || key->action != "KeyA") {
                    continue;
                }
                require(app.down(2, x, y, 2), "held printable key starts async commit");
                sink.done = sink.success = true;
                app.tick(2.1);
                const int before = sink.deliveries;
                app.tick(2.6);
                require(sink.pending && sink.deliveries == before + 1 && sink.held == "a",
                        "held printable key repeats without losing async commit ownership");
                app.up(2, x, y);
                sink.done = sink.success = true;
                app.tick(2.7);
                require(!sink.pending, "final repeat acknowledgement is consumed after release");
                found = true;
                break;
            }
            if (found) {
                break;
            }
        }
        require(press("KeyA", 3) && press("KeyB", 3.1), "hide fixture has pending and queued text");
        const auto before_hide = sink.deliveries;
        app.set_interaction_active(false);
        sink.done = true;
        app.tick(3.2);
        require(!sink.pending && sink.deliveries == before_hide,
                "hiding discards queued output before any late result can deliver it");
        app.set_interaction_active(true);
        app.apply({"en-us-full", "ja-romaji", "graphite"});
        require(press("KeyA", 4) && press("Enter", 4.1) && app.view().preedit.empty(),
                "accepted IME commit transfers preedit to the controller");
        require(press("KeyA", 4.2) && app.view().preedit == "あ",
                "new local preedit can start while the previous commit waits");
        sink.done = true;
        sink.success = false;
        app.tick(4.3);
        const auto before_retry = sink.deliveries;
        require(press("Enter", 4.4) && sink.deliveries == before_retry + 1 && app.view().preedit == "あ",
                "retrying earlier IME text never consumes identical newly typed preedit");
        sink.done = sink.success = true;
        app.tick(4.5);
        require(press("Enter", 4.6) && sink.deliveries == before_retry + 2 && app.view().preedit.empty(),
                "new preedit remains independently available after retry");
        app.cancel();
    }
    fs::remove_all(options.config_dir);
}
void handoff_status() {
    const auto directory =
        fs::temp_directory_path() / ("framekeyboard-handoff-tests-" + std::to_string(getpid()));
    fs::remove_all(directory);
    Settings settings;
    settings.replace_native_keyboard = true;
    save_settings(directory, settings);
    Options options;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.config_dir = directory;
    struct Availability : Local {
        bool ready{true}, text_ready{true};
        bool pump() override { return ready; }
        bool text_available() override { return text_ready; }
    } sink;
    {
        App app(options, sink);
        ReplacementPresentation presentation;
        NativeReplacement::State state{2, true, true, false, true};
        presentation.update(true, state);
        app.set_interaction_active(presentation.interactive());
        require(presentation.show() && app.replacement_ready(), "handoff prepares a ready panel");
        require(app.view().status.empty(), "healthy handoff must not flash a typing-disabled warning");
        std::optional<std::pair<double, double>> point;
        const auto view = app.view();
        for (int y = 96; y < panel_height && !point; y += 4) {
            for (int x = 0; x < panel_width; x += 4) {
                const auto* key = app.renderer.hit_key(view, x, y);
                if (key && key->action == "KeyA") {
                    point = {x, y};
                    break;
                }
            }
        }
        require(point.has_value(), "handoff fixture key exists");
        const auto [x, y] = *point;
        require(!app.down(1, x, y, 1) && sink.keys.empty() && sink.text.empty(),
                "quiet handoff still blocks input before acknowledgement");
        state.active = true;
        presentation.update(true, state);
        app.set_interaction_active(presentation.interactive());
        require(app.view().status.empty() && app.down(1, x, y, 2),
                "acknowledged handoff enables typing without changing the notice");
        app.up(1, x, y);
        require(sink.text == std::vector<std::string>{"a"}, "typing resumes after acknowledgement");
        state = {state.request + 1, false, false, false, true, false};
        presentation.update(true, state);
        app.set_interaction_active(presentation.interactive());
        require(!presentation.show() && !presentation.interactive() && !presentation.remote(),
                "cleared Steam targets hide the managed panel without selecting local input");
        require(!app.down(1, x, y, 2.1) && sink.keys.empty() &&
                    sink.text == std::vector<std::string>{"a"},
                "field loss cannot deliver text or controls into the focused local receiver");
        app.set_interaction_active(false);
        sink.ready = false;
        app.tick(3);
        require(app.view().status.starts_with("Keyboard connection lost."),
                "a real backend failure remains visible during the handoff");
        require(app.view().status.find("Settings > Quit Full Keyboard") != std::string::npos,
                "unrecoverable resident failure explains how to exit the instance");
        sink.ready = true;
        sink.text_ready = false;
        app.tick(4);
        require(app.view().status.starts_with("Text delivery unavailable."),
                "missing text transport still shows the typing warning");
        app.paint(4);
        sink.text_ready = true;
        require(app.tick(5) && app.view().status.empty(),
                "transport recovery repaints the warning while input remains paused");
        require(!app.down(1, x, y, 5), "recovery cannot bypass the handoff input gate");
        app.report_status("Input connection error");
        require(app.view().status == "Input connection error", "handoff preserves explicit errors");
    }
    {
        Options disabled = options;
        disabled.start_enabled = false;
        App app(disabled, sink);
        app.set_interaction_active(false);
        require(app.view().status.starts_with("Typing disabled:"),
                "an input-disabled launch still shows its warning");
    }
    {
        options.mode = Mode::Preview;
        options.input = InputBackend::None;
        options.start_enabled = false;
        App app(options, sink);
        app.set_interaction_active(false);
        require(app.view().status.starts_with("Preview only:"), "preview retains its input notice");
    }
    fs::remove_all(directory);
}
void resident_visibility() {
    {
        ReplacementPresentation background(true);
        background.update(true, {});
        require(!background.show() && !background.interactive(),
                "boot remains hidden before bridge readiness");
        background.update(true, {1, false, false, false, true});
        require(!background.show(), "bridge readiness does not summon a boot resident");
        background.update(true, {2, true, true, false, true});
        require(background.show() && !background.interactive(),
                "a native request reveals the boot resident");
        background.update(true, {2, true, true, true, true});
        require(background.interactive(), "boot resident follows ordinary takeover readiness");
        background.dismiss();
        background.update(true, {3, false, false, false, true});
        require(!background.show(), "boot resident dismisses normally");
        background.summon();
        require(background.show(), "manual relaunch can summon a boot resident");
        ReplacementPresentation unavailable(true);
        unavailable.update(true, {});
        unavailable.summon();
        require(unavailable.show(), "manual launch reaches Settings even if the bridge is unavailable");
    }
    ReplacementPresentation view;
    NativeReplacement::State state{1, false, false, false, true};
    view.update(true, state);
    require(view.show(), "first launch can show the manual panel");
    view.dismiss();
    view.update(true, state);
    require(!view.show() && !view.interactive(), "manual X remains hidden while resident");
    state = {2, true, true, false, true};
    view.update(true, state);
    require(view.show() && !view.interactive(), "native summon prepares the hidden resident panel");
    state.active = true;
    view.update(true, state);
    require(view.interactive() && view.remote(), "acknowledged summon enables session input");
    state.local = true;
    view.update(true, state);
    require(view.show() && view.interactive() && !view.remote(),
            "Gamescope takeover uses local libei and Unicode rather than Steam's field manager");
    state.local = false;
    view.update(true, state);
    view.dismiss();
    view.update(true, state);
    require(!view.show() && !view.remote(), "X cannot reopen the still-cached native request");
    state = {3, false, false, false, true};
    view.update(true, state);
    require(!view.show(), "native close acknowledgement keeps the resident panel hidden");
    state = {4, true, true, false, true};
    view.update(true, state);
    require(view.show(), "a later native request reopens ours without relaunch");
    view.dismiss();
    view.update(true, {});
    require(!view.show(), "helper loss cannot reopen a dismissed manual panel");
    view.update(true, {1, false, false, false, true});
    view.summon();
    require(view.show() && view.interactive(), "manual relaunch can reach Settings while resident");
    view.dismiss();
    view.update(false, state);
    require(view.show() && view.interactive() && !view.remote(),
            "disable returns to ordinary app behavior");
    view.update(true, {5, true, false, false, true});
    require(!view.show(), "unknown requests stay native");
}
void tracking_recovery() {
    TrackingReadiness tracking;
    ReplacementPresentation view;
    require(!tracking.should_sample(true, false, false, 0),
            "idle resident does not poll tracking without a request or tracking loss");
    view.update(true, {2, true, true, true, true, true});
    require(tracking.should_sample(true, view.show(), true, 10), "visible replacement checks tracking");
    tracking.sampled(false, 10);
    // A lost pose withdraws readiness. The lease expires and native takes over.
    view.update(true, {3, false, false, false, true});
    require(tracking.waiting() && !view.show(), "tracking loss survives native fallback");
    require(!tracking.should_sample(true, view.show(), false, 10.1),
            "hidden recovery retains the bounded tracking cadence");
    require(!tracking.should_sample(false, view.show(), false, 11),
            "closed dashboard does not poll tracking");
    require(tracking.should_sample(true, view.show(), false, 11),
            "hidden replacement still checks whether tracking recovered after lease expiry");
    tracking.sampled(false, 11);
    require(tracking.waiting(), "another invalid pose cannot restore readiness");
    require(tracking.should_sample(true, view.show(), false, 11.25),
            "hidden replacement keeps checking after another invalid pose");
    tracking.sampled(true, 11.25);
    require(!tracking.waiting() && !view.show() && !view.interactive(),
            "valid tracking restores readiness without reopening the expired request");
    view.update(true, {4, true, true, false, true, true});
    require(view.show() && !view.interactive(), "fresh summon still needs acknowledgement");
    view.update(true, {4, true, true, true, true, true});
    require(view.interactive(), "a new request can type after tracking recovers");
    for (bool fallback : {false, true}) {
        ReplacementPresentation hidden;
        hidden.update(true, {5, true, true, true, true, true});
        if (fallback) {
            hidden.fallback();
        } else {
            hidden.dismiss();
        }
        tracking.sampled(false, 12);
        require(tracking.should_sample(true, hidden.show(), false, 13),
                "dismissal and explicit fallback allow tracking recovery");
        tracking.sampled(true, 13);
        require(!hidden.show() && !hidden.interactive(),
                "tracking recovery preserves dismissal and explicit native fallback");
    }
    tracking.sampled(false, 20);
    require(!tracking.should_sample(true, true, false, 20.1), "hidden checks remain throttled");
    tracking.request_sample();
    require(tracking.should_sample(true, true, false, 20.1) && tracking.waiting(),
            "recenter can request an immediate check without inventing a valid pose");
}
void resident_close() {
    const auto directory =
        fs::temp_directory_path() / ("framekeyboard-resident-tests-" + std::to_string(getpid()));
    fs::remove_all(directory);
    Settings settings;
    settings.replace_native_keyboard = true;
    save_settings(directory, settings);
    Options options;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.config_dir = directory;
    Local sink;
    auto has = [](App& app, const std::string& id) {
        for (const auto& c : app.view().controls) {
            if (c.id == id) {
                return true;
            }
        }
        return false;
    };
    auto click = [](App& app, const std::string& id) {
        for (const auto& c : app.view().controls) {
            if (c.id != id) {
                continue;
            }
            const auto r = c.bounds;
            app.down(3, r.x + r.width / 2, r.y + r.height / 2, 1);
            app.up(3, r.x + r.width / 2, r.y + r.height / 2);
            return;
        }
        throw std::runtime_error("resident fixture control missing");
    };
    {
        App app(options, sink);
        auto hold = [&](const std::string& action, unsigned pointer) {
            const auto view = app.view();
            for (int y = 96; y < panel_height; y += 4) {
                for (int x = 0; x < panel_width; x += 4) {
                    const auto* key = app.renderer.hit_key(view, x, y);
                    if (key && key->action == action) {
                        require(app.down(pointer, x, y, 1), "resident fixture key accepted");
                        return;
                    }
                }
            }
            throw std::runtime_error("resident fixture key missing");
        };
        require(!has(app, "quit"), "Quit is absent from the main toolbar");
        hold("ControlLeft", 1);
        hold("KeyC", 2);
        click(app, "close");
        require(!app.quitting() && app.take_dismissal() && !app.take_dismissal(),
                "replacement X dismisses once without exiting");
        require(app.replacement_ready(),
                "a dismissed resident retains backend readiness for new summons");
        require(sink.keys ==
                    std::vector<std::pair<int, int>>{
                        {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
                "dismissal releases both controllers through the original input gate");
        app.summon();
        app.set_interaction_active(true);
        app.show_settings();
        require(has(app, "quit"), "Quit is available in Settings while replacement is enabled");
        app.set_replacement_available(true);
        app.set_native_session(true);
        app.paint(1);
        app.renderer.write_png("/tmp/framekeyboard-resident-settings.png");
        click(app, "quit");
        require(app.quitting() && !app.take_dismissal(), "Settings Quit exits the resident app");
    }
    settings.replace_native_keyboard = false;
    save_settings(directory, settings);
    {
        App app(options, sink);
        app.show_settings();
        require(!has(app, "quit"), "Quit is absent with replacement disabled");
        click(app, "replace-native-keyboard");
        require(!has(app, "quit"), "pending enable does not expose Quit before Apply");
        click(app, "apply");
        app.show_settings();
        require(has(app, "quit"), "applied enable exposes Settings Quit");
        click(app, "replace-native-keyboard");
        require(has(app, "quit"), "pending disable retains Quit until Apply");
        click(app, "apply");
        app.show_settings();
        require(!has(app, "quit"), "applied disable removes Settings Quit");
        app.back();
        click(app, "close");
        require(app.quitting() && !app.take_dismissal(), "ordinary X retains its quit behavior");
    }
    settings.replace_native_keyboard = true;
    save_settings(directory, settings);
    {
        Options preview = options;
        preview.mode = Mode::Preview;
        preview.input = InputBackend::None;
        preview.start_enabled = false;
        App app(preview, sink);
        app.show_settings();
        require(!has(app, "quit"), "desktop preview does not enter resident replacement mode");
        app.back();
        click(app, "close");
        require(app.quitting() && !app.take_dismissal(),
                "preview X still exits with a saved replacement preference");
    }
    fs::remove_all(directory);
}
void unavailable_reasons() {
    const auto directory =
        fs::temp_directory_path() / ("framekeyboard-reason-tests-" + std::to_string(getpid()));
    Settings settings;
    settings.replace_native_keyboard = true;
    save_settings(directory, settings);
    Options options;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.config_dir = directory;
    Local local;
    {
        App app(options, local);
        app.show_settings();
        for (const auto& [code, message] : std::vector<std::pair<std::string, std::string>>{
                 {"debugger-unavailable", "Steam debugger is unavailable."},
                 {"debugger-blocked", "Local Steam debugger access is blocked."},
                 {"steam-incompatible", "Steam keyboard interfaces are incompatible."},
                 {"install-timeout", "Steam keyboard bridge installation timed out."},
                 {"reply-timeout", "Steam debugger stopped responding."},
                 {"replacement-busy", "Another keyboard replacement is active."},
                 {"connection-lost", "Replacement connection was lost."},
                 {"private fixture", "Replacement helper is unavailable."}}) {
            int pair[2];
            require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0,
                    "failure reason fixture socketpair");
            NativeReplacement replacement(local, pair[0]);
            put(pair[1], "{\"op\":\"unavailable\",\"reason\":\"" + code + "\"}");
            close(pair[1]); // The last status must survive helper EOF.
            replacement.poll(1);
            require(!replacement.state().available && replacement.unavailable_reason() == message,
                    "unavailable metadata maps only to fixed user-facing reasons");
            app.dirty = false;
            app.set_replacement_available(false, replacement.unavailable_reason());
            require(app.dirty && app.view().status == message + " Steam keyboard remains available.",
                    "a changed unavailable reason repaints Settings without changing availability");
            require(app.view().status.find("private fixture") == std::string::npos,
                    "unrecognized helper text is never displayed");
        }
        app.set_replacement_available(true);
        require(app.view().status.empty(), "restored availability clears the failure notice");
    }
    fs::remove_all(directory);
}
void slow_work_allowance() {
    const auto directory =
        fs::temp_directory_path() / ("framekeyboard-slow-work-" + std::to_string(getpid()));
    Options options;
    options.config_dir = directory;
    NullSink sink;
    {
        App app(options, sink);
        require(app.slow_work_deadline() == 0, "ordinary frames have no slow-work allowance");
        const auto before = monotonic_seconds();
        app.apply({});
        const auto deadline = app.slow_work_deadline();
        require(deadline >= before + 30 && deadline <= monotonic_seconds() + 30,
                "profile activation grants a bounded deadline");
        app.flush_settings();
        app.reload();
        require(app.slow_work_deadline() == deadline,
                "nested or repeated storage/profile work cannot extend a frame's allowance");
        app.frame_completed();
        require(app.slow_work_deadline() == 0, "only a completed frame clears slow-work state");
        app.flush_settings();
        require(app.slow_work_deadline() > 0, "a storage barrier explicitly declares slow work");
        app.frame_completed();
        app.reload();
        require(app.slow_work_deadline() > 0, "profile reload explicitly declares slow work");
    }
    fs::remove_all(directory);
}
void runtime_wait_allowance() {
    NullSink local;
    NativeReplacement replacement(local);
    replacement.begin_runtime_wait(10);
    require(replacement.runtime_wait_deadline() == 40, "runtime waits get a fixed deadline");
    replacement.begin_runtime_wait(12);
    replacement.end_runtime_wait(15);
    require(replacement.runtime_wait_deadline() == 40, "nested runtime calls keep their outer scope");
    replacement.end_runtime_wait(20);
    require(replacement.runtime_wait_deadline() == 23,
            "returning leaves a short frame-completion margin");
    replacement.begin_runtime_wait(25);
    require(replacement.runtime_wait_deadline() == 40, "repeated calls cannot extend the frame limit");
    replacement.end_runtime_wait(39);
    require(replacement.runtime_wait_deadline() == 40, "completion margin cannot extend the hard limit");
    replacement.frame_completed();
    require(replacement.runtime_wait_deadline() == 0, "completed frames clear runtime wait state");
    replacement.begin_runtime_wait(100);
    require(replacement.runtime_wait_deadline() == 130, "a later frame can wait for runtime recovery");
    replacement.end_runtime_wait(101);
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--instance-fixture") {
            VrInstance instance(argv[2]);
            require(instance.is_owner(), "a recovered process must release the instance lock");
            std::cout << "Recovered instance lock acquired.\n";
            return 0;
        }
        if (argc == 3 && (std::string(argv[1]) == "--stalled-main-fixture" ||
                          std::string(argv[1]) == "--runtime-wait-fixture" ||
                          std::string(argv[1]) == "--stopped-runtime-fixture")) {
            VrInstance instance(argv[2]);
            require(instance.is_owner(), "isolated stalled process owns its instance lock");
            NullSink local;
            NativeReplacement replacement(local);
            require(replacement.prepare_watchdog(), "private recovery health pipe");
            std::jthread watchdog([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    replacement.watchdog_tick(monotonic_seconds());
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
            replacement.update(true, false, 0, false, monotonic_seconds(), true);
            if (std::string(argv[1]) != "--stalled-main-fixture") {
                replacement.begin_runtime_wait(monotonic_seconds());
                if (std::string(argv[1]) == "--stopped-runtime-fixture") {
                    replacement.watchdog_tick(monotonic_seconds());
                    std::cout << "Runtime wait ready.\n" << std::flush;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4200));
                replacement.end_runtime_wait(monotonic_seconds());
                replacement.update(true, false, 0, false, monotonic_seconds(), true);
                replacement.frame_completed();
                std::cout << "Main loop resumed after a runtime delay longer than three seconds.\n";
                return 0;
            }
            // The independent watchdog stays live; the main loop never polls or
            // handles recenter again. No Steam, VR or input endpoint is used.
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (argc == 2 &&
            (std::string(argv[1]) == "--steam-unarmed" || std::string(argv[1]) == "--helper-fixture" ||
             std::string(argv[1]) == "--helper-error-fixture")) {
            NullSink local;
            NativeReplacement replacement(local);
            require(replacement.prepare_watchdog(), "private health pipe available");
            const auto start = monotonic_seconds();
            while (monotonic_seconds() - start < 3) {
                const double now = monotonic_seconds();
                replacement.poll(now);
                replacement.watchdog_tick(now);
                replacement.update(true, false, 0, false, now);
                if (std::string(argv[1]) == "--helper-error-fixture") {
                    if (std::string(replacement.unavailable_reason()) ==
                        "Local Steam debugger access is blocked.") {
                        std::cout << "Exited helper failure reason preserved.\n";
                        return 0;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if (replacement.state().available) {
                    require(!replacement.state().active, "unarmed bridge must not take over");
                    replacement.update(false, false, 0, false, now);
                    std::cout
                        << (std::string(argv[1]) == "--helper-fixture"
                                ? "Private helper spawn, health pipe and detach passed.\n"
                                : "C++ helper spawn, unarmed Steam handshake and detach passed.\n");
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            throw std::runtime_error(std::string("unarmed Steam handshake unavailable: ") +
                                     replacement.unavailable_reason());
        }
        require(argc == 1, "unknown test option");
        update_shutdown();
        hidden_session_wakeup();
        transport();
        ordered_transport();
        delivered_local_route();
        helper_retry_backoff();
        installation_budget();
        sanitized_diagnostics();
        named_key_repeats();
        app_commit();
        handoff_status();
        resident_visibility();
        tracking_recovery();
        resident_close();
        slow_work_allowance();
        unavailable_reasons();
        runtime_wait_allowance();
        std::cout << "Native replacement transport and commit ownership passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
