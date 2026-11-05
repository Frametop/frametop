// FrametopLink, FrametopSink and run_vr's Frametop hooks (FrametopVr) against a private
// stand-in relay (an abstract name of this test's own), so nothing reaches Frametop's relay or
// a desktop, and with SteamVR's reads faked.
#include "framekeyboard/frametop.hpp"
#include "framekeyboard/frametop_vr.hpp"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <optional>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct Steam : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    void send(int code, int value) override { keys.emplace_back(code, value); }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        text.push_back(value);
        return true;
    }
};
sockaddr_un named(const std::string& name, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return address;
}
// The stand-in relay: bound to `name`, it remembers who said hello last.
struct Relay {
    int fd{-1};
    sockaddr_un peer{};
    socklen_t peer_length{};
    explicit Relay(const std::string& name) {
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        socklen_t length{};
        const auto address = named(name, length);
        require(fd >= 0 && bind(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0,
                "bind stand-in relay");
    }
    ~Relay() { close(fd); }
    std::vector<std::string> take() {
        std::vector<std::string> result;
        for (;;) {
            char data[512];
            sockaddr_un from{};
            socklen_t length = sizeof(from);
            const auto count = recvfrom(fd, data, sizeof(data), MSG_DONTWAIT,
                                        reinterpret_cast<sockaddr*>(&from), &length);
            if (count < 0) {
                return result;
            }
            peer = from, peer_length = length;
            result.emplace_back(data, static_cast<std::size_t>(count));
        }
    }
    void say(const std::string& text) {
        require(sendto(fd, text.data(), text.size(), 0, reinterpret_cast<const sockaddr*>(&peer),
                       peer_length) == static_cast<ssize_t>(text.size()),
                "stand-in relay send");
    }
};
// What the hooks pressed, as "move 272", "down 272", "up 272", "cancel 272".
struct Pointers : PointerSink {
    std::vector<std::string> calls;
    bool move(unsigned pointer, double, double) override { return said("move", pointer); }
    bool down(unsigned pointer, double, double, double) override { return said("down", pointer); }
    bool up(unsigned pointer, double, double) override { return said("up", pointer); }
    void cancel_pointer(unsigned pointer) override { said("cancel", pointer); }
    bool said(const char* what, unsigned pointer) {
        calls.push_back(std::string(what) + " " + std::to_string(pointer));
        return true;
    }
    std::vector<std::string> take() { return std::exchange(calls, {}); }
};

// Releases the relay didn't take are sent again until they go. The stand-in relay
// has no hello gate, so going away and coming back stands for a full socket here.
void owed_release_tests() {
    const std::string name = "framekeyboard-test-owed-" + std::to_string(getpid());
    double now = 100;
    std::optional<Relay> relay(std::in_place, name);
    FrametopLink link(name, [&] { return now; });
    Steam steam;
    FrametopSink sink(link, steam);
    link.poll();
    relay->take();
    relay->say("kb 1 1");
    sink.pump();
    sink.send(KEY_A, 1);
    require(relay->take() == std::vector<std::string>{"kbkey 30 1"}, "A down on the desktop");

    // The relay goes away with A down: the release is owed, and kept while it's away.
    relay.reset();
    sink.send(KEY_A, 0);
    require(sink.owed() == std::set<int>{KEY_A}, "A's release is owed");
    sink.pump();
    require(sink.owed() == std::set<int>{KEY_A}, "...and kept while the relay is away");
    relay.emplace(name);
    sink.pump();
    require(relay->take() == std::vector<std::string>{"kbkey 30 0"} && sink.owed().empty(),
            "the relay is back: A's release goes, once");
    sink.pump();
    require(relay->take().empty(), "nothing owed, nothing sent");

    // A press of an owed key on the desktop drops what's owed: its own release covers it.
    sink.send(KEY_B, 1);
    relay->take();
    const auto peer = relay->peer;
    const auto peer_length = relay->peer_length;
    relay.reset();
    sink.send(KEY_B, 0);
    relay.emplace(name);
    relay->peer = peer, relay->peer_length = peer_length;
    sink.send(KEY_B, 1);
    require(sink.owed().empty(), "a new press on the desktop: nothing owed");
    sink.send(KEY_B, 0);
    require(relay->take() == std::vector<std::string>{"kbkey 48 0", "kbkey 48 1", "kbkey 48 0"},
            "the owed B release, then B down and up");

    // What's owed goes before the next key: Shift comes up before A goes down.
    sink.send(KEY_LEFTSHIFT, 1);
    relay->take();
    relay.reset();
    sink.send(KEY_LEFTSHIFT, 0);
    relay.emplace(name);
    relay->peer = peer, relay->peer_length = peer_length;
    sink.send(KEY_A, 1);
    sink.send(KEY_A, 0);
    require(relay->take() == std::vector<std::string>{"kbkey 42 0", "kbkey 30 1", "kbkey 30 0"},
            "Shift up first, then A");

    // A press that goes to Steam sends the desktop's owed release first too.
    sink.send(KEY_C, 1);
    relay->take();
    relay.reset();
    sink.send(KEY_C, 0);
    relay.emplace(name);
    relay->peer = peer, relay->peer_length = peer_length;
    relay->say("kb 0 1");
    link.poll(); // (not a pump: the release stays owed)
    require(!link.desktop(), "typing goes to Steam");
    require(sink.owed() == std::set<int>{KEY_C}, "the desktop's C release is owed");
    sink.send(KEY_C, 1);
    require(steam.keys.back() == std::pair{KEY_C, 1}, "C down in Steam");
    require(relay->take() == std::vector<std::string>{"kbkey 46 0"} && sink.owed().empty(),
            "...after the desktop's C release");
    sink.send(KEY_C, 0);
    require(steam.keys.back() == std::pair{KEY_C, 0}, "Steam's C comes up in Steam");
}

// FrametopVr: when the panel may show, what Frametop's requests do, fingertips, and the tips'
// reads, without SteamVR.
void hooks_tests() {
    const std::string name = "framekeyboard-test-hooks-" + std::to_string(getpid());
    double now = 100;
    Relay relay(name);
    FrametopLink link(name, [&] { return now; });
    Pointers pointers;
    int tip_reads = 0;
    unsigned right = FrametopVrAccess::no_device;
    FrametopVrAccess access{
        .hand_device = [&](std::size_t hand) { return hand == 1 ? right : FrametopVrAccess::no_device; },
        .tip_component = [&](unsigned, bool) -> std::optional<Transform> {
            ++tip_reads;
            return {};
        },
        .pose = [](unsigned) -> std::optional<Transform> { return {}; },
    };
    PanelPlacement displayed;
    displayed.set_transform({{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}});
    KeyHaptics feedback;
    const auto bounds = [] { return Rect{0, 0, 100, 100}; };
    const auto no_grip = [](unsigned) { return false; };

    // Without Frametop: upstream's behaviour, the dashboard alone.
    {
        FrametopVr plain(nullptr, pointers, access);
        require(!plain.lasers(), "plain: no lasers outside the dashboard");
        require(plain.begin_frame(false, false) == false && plain.begin_frame(true, false) == true,
                "plain: relaunch passes through");
        require(plain.surface(true, false) && !plain.surface(false, true), "plain: the dashboard alone");
        require(!plain.hand_pokes(true, displayed, now), "plain: no fingertips");
    }

    FrametopVr hooks(&link, pointers, access);
    require(hooks.lasers(), "Frametop: lasers");
    require(!hooks.begin_frame(false, false), "no request, no relaunch");
    relay.take();
    require(hooks.surface(true, true) && !hooks.surface(false, false), "no word: the dashboard with a Steam field");

    // Frametop's desktop shows, and a text field asks for the keyboard: it opens over it.
    relay.say("kb 1 1");
    relay.say("kb show");
    require(hooks.begin_frame(false, false), "show: placed anew");
    require(hooks.surface(false, false), "show: up over the desktop");
    hooks.shown(true);
    link.poll();
    require(relay.take() == std::vector<std::string>{"kbhello 1"}, "Frametop hears it's up");
    relay.say("kb show");
    require(!hooks.begin_frame(false, true), "show while up: not moved");
    relay.say("kb toggle");
    hooks.begin_frame(false, true);
    require(!hooks.surface(false, false), "toggle while up: closed");
    require(hooks.surface(false, true), "...but a Steam field still has it");
    relay.say("kb toggle");
    require(hooks.begin_frame(false, false) && hooks.surface(false, false), "toggle while closed: opens");
    hooks.dismissed();
    require(!hooks.surface(false, false), "its X closes it");
    require(hooks.begin_frame(true, false) && hooks.surface(false, false), "a recenter opens it for Frametop");
    relay.say("kb hide");
    hooks.begin_frame(false, true);
    require(!hooks.surface(false, false), "hide");
    relay.say("kb 1 0");
    relay.say("kb show");
    hooks.begin_frame(false, false);
    require(!hooks.surface(false, false) && hooks.surface(true, false),
            "asked for while the desktop is hidden: only with the dashboard");
    relay.say("kb 1 1");
    hooks.begin_frame(false, false);

    // Fingertips: a press, then nothing more for hand_silence: it comes up.
    const auto right_pointer = std::to_string(FrametopVr::hand_pointer + 1);
    relay.say("kb poke right press 0 0 0");
    link.poll();
    require(hooks.hand_pokes(true, displayed, now), "fingertips with Frametop");
    require(pointers.take() == std::vector<std::string>{"down " + right_pointer}, "a press goes down");
    now += 1;
    relay.say("kb 1 1");
    link.poll();
    hooks.hand_pokes(true, displayed, now);
    require(pointers.take().empty(), "held while it's heard from within the silence");
    now += FrametopVr::hand_silence + .1;
    relay.say("kb 1 1"); // still fresh: it's the press that went quiet
    link.poll();
    hooks.hand_pokes(true, displayed, now);
    require(pointers.take() == std::vector<std::string>{"cancel " + right_pointer},
            "silence: the press is let go");
    relay.say("kb poke right press 0 0 0");
    relay.say("kb poke right press 0 0 0");
    relay.say("kb poke right up");
    link.poll();
    hooks.hand_pokes(true, displayed, now);
    require(pointers.take() == std::vector<std::string>{"down " + right_pointer, "up " + right_pointer,
                                                        "down " + right_pointer, "up " + right_pointer},
            "a second press lets the first up");
    relay.say("kb poke right hover 0 0 0");
    link.poll();
    hooks.hand_pokes(false, displayed, now);
    require(pointers.take() == std::vector<std::string>{"cancel " + right_pointer},
            "the panel moving: a hover isn't taken, and the hand lets go");
    relay.say("kb poke right press 0 0 0");
    relay.say("kb poke right end");
    link.poll();
    hooks.hand_pokes(true, displayed, now);
    require(pointers.take() == std::vector<std::string>{"down " + right_pointer, "cancel " + right_pointer},
            "an end cancels");

    // The tips: nothing to do while off and none is near; a new controller gives no tip that
    // frame; a missing tip component is tried again at most once a second.
    require(!hooks.touch(false, displayed, bounds, no_grip, feedback, now), "touch off: nothing");
    right = 7;
    require(hooks.touch(true, displayed, bounds, no_grip, feedback, now), "touch on");
    require(tip_reads == 0, "a new controller: no read that frame");
    hooks.touch(true, displayed, bounds, no_grip, feedback, now);
    hooks.touch(true, displayed, bounds, no_grip, feedback, now + .5);
    require(tip_reads == 1, "a missing tip: read once");
    hooks.touch(true, displayed, bounds, no_grip, feedback, now + 1.5);
    require(tip_reads == 2, "...and again after a second");
    require(pointers.take().empty() && !hooks.tips_near(), "no tip, no presses");
}
} // namespace

int main() {
    try {
        const std::string name = "framekeyboard-test-relay-" + std::to_string(getpid());
        double now = 100;
        Relay relay(name);
        FrametopLink link(name, [&] { return now; });
        Steam steam;
        FrametopSink sink(link, steam);

        // Hello on the first poll; no word yet, so typing goes to Steam.
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 0"}, "hello on first poll");
        require(!link.fresh() && !link.desktop() && !link.surface(), "no word: not fresh");
        sink.send(KEY_A, 1);
        sink.send(KEY_A, 0);
        require(steam.keys.size() == 2, "no word: keys go to Steam");

        // The relay says the desktop has typing and shows.
        relay.say("kb 1 1");
        link.poll();
        require(link.fresh() && link.desktop() && link.surface(), "word from the relay");
        sink.send(KEY_B, 1);
        require(sink.commit_text("héllo"), "desktop text accepted");
        sink.send(KEY_B, 2); // repeats stay with the receiver
        auto got = relay.take();
        require(got == std::vector<std::string>{"kbkey 48 1", "kbtext héllo"},
                "desktop typing to relay");
        require(steam.keys.size() == 2 && steam.text.empty(), "nothing to Steam meanwhile");

        // Typing moves to Steam with B held: B is still released on the desktop.
        relay.say("kb 0 1");
        link.poll();
        require(!link.desktop() && link.surface(), "typing moved to Steam");
        sink.send(KEY_B, 0);
        sink.send(KEY_C, 1);
        sink.send(KEY_C, 0);
        require(sink.commit_text("x"), "Steam text accepted");
        got = relay.take();
        require(got == std::vector<std::string>{"kbkey 48 0"}, "release where it was pressed");
        require(steam.keys.size() == 4 && steam.keys[2] == std::pair{KEY_C, 1} && steam.text.size() == 1,
                "C and text to Steam");

        // Requests.
        relay.say("kb show");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Show, "show");
        require(link.take_request() == FrametopLink::Request::None, "taken once");
        relay.say("kb toggle");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Toggle, "toggle");
        relay.say("kb hide");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Hide, "hide");

        // A fingertip's pokes, from ft-screens through the relay.
        relay.say("kb poke right press 0.1200 1.0500 -0.6500");
        relay.say("kb poke left end");
        relay.say("kb poke left up");
        relay.say("kb poke right hover");        // a hover says where
        relay.say("kb poke middle hover 0 0 0"); // no such hand
        relay.say("kb poke right type 0 0 0");   // no such verb
        relay.say("kb poke right move 0 0 nan");
        relay.say("kb poke right move 0 0 0 junk");
        link.poll();
        auto pokes = link.take_pokes();
        require(pokes.size() == 3, "three good pokes, the rest dropped");
        require(pokes[0].hand == 1 && pokes[0].verb == HandPoke::Verb::Press && pokes[0].point &&
                    (*pokes[0].point)[1] == 1.05,
                "the press and its point");
        require(pokes[1].hand == 0 && pokes[1].verb == HandPoke::Verb::End && !pokes[1].point, "an end");
        require(pokes[2].verb == HandPoke::Verb::Up && !pokes[2].point, "an up without a point");
        require(link.take_pokes().empty(), "taken once");

        // Silence: the word goes stale, and typing goes back to Steam.
        relay.say("kb 1 1");
        link.poll();
        require(link.desktop(), "desktop again");
        now += FrametopLink::stale_after + .1;
        require(!link.fresh() && !link.desktop() && !link.surface(), "stale after silence");
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 0"}, "hello again after a second");
        link.set_open(true);
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 1"}, "hello at once when it opens");
        link.set_open(true);
        link.poll();
        require(relay.take().empty(), "no change, no hello within the second");

        // Another name can't speak for the relay.
        {
            const int other = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            socklen_t length{};
            const auto impostor = named(name + "-impostor", length);
            require(bind(other, reinterpret_cast<const sockaddr*>(&impostor), length) == 0,
                    "bind impostor");
            const std::string text = "kb 1 1";
            sendto(other, text.data(), text.size(), 0, reinterpret_cast<const sockaddr*>(&relay.peer),
                   relay.peer_length);
            close(other);
            link.poll();
            require(!link.fresh(), "impostor ignored");
        }
        owed_release_tests();
        hooks_tests();
        std::cout << "frametop: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frametop: " << error.what() << '\n';
        return 1;
    }
}
